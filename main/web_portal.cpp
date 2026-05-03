#include "web_portal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "board_config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gb_time.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "mem.h"
#include "nvs_flash.h"
#include "touch_input.h"

#if GB_ENABLE_AUDIO
#include "apu.h"
#endif

namespace {
constexpr const char *TAG = "web_portal";

// One client is the realistic case for this device — the second one would
// halve the bitrate budget. We keep the array form to make it easy to bump.
constexpr uint8_t MAX_WS_CLIENTS = 1;
constexpr size_t MAX_HTTP_CLIENTS = 7;
constexpr size_t PACKED_FRAME_SIZE =
    6 + ((board::GAMEBOY_WIDTH * board::GAMEBOY_HEIGHT + 3) / 4);
#if GB_ENABLE_AUDIO
constexpr size_t AUDIO_CHUNK_TARGET_SAMPLES = 256;
constexpr size_t AUDIO_CHUNK_MAX_SAMPLES = 1024;
constexpr uint32_t AUDIO_PARTIAL_FLUSH_MS = 32;
#endif

// Throttle state broadcasts so a button-mashing user can't flood the WS link
// with redundant JSON.
constexpr uint32_t STATE_PUBLISH_MIN_INTERVAL_MS = 100;
constexpr uint32_t STATS_LOG_INTERVAL_MS = 2000;

// Polling intervals for the WS client-list sync. ESP-IDF v6 doesn't deliver a
// HTTP_GET upgrade call to user handlers (the framework swallows it) and
// `close_fn` only fires on socket teardown, so the *only* reliable way to
// notice a fresh WebSocket session — before the client sends us anything — is
// to enumerate httpd_get_client_list periodically.
constexpr uint32_t WS_SCAN_IDLE_INTERVAL_MS = 100;
constexpr uint32_t WS_SCAN_ACTIVE_INTERVAL_MS = 1000;

WebPortalConfig portal_config;
httpd_handle_t server = nullptr;
char ip_string[16] = "0.0.0.0";
bool spiffs_mounted = false;
bool audio_stream_enabled = false;
uint32_t last_frame_ms = 0;
uint32_t last_state_publish_ms = 0;
uint32_t last_stream_log_ms = 0;
uint32_t streamed_frames = 0;
uint32_t dropped_frames_window = 0;
uint32_t sent_frames_total = 0;
uint32_t last_client_scan_ms = 0;

struct WsClient {
  int fd = -1;
  bool active = false;
  // frame_pending indicates HTTPD still owns the currently-staged buffer via
  // httpd_ws_send_data_async. While set, new frames are dropped instead of
  // overwriting in-flight data.
  bool frame_pending = false;
  size_t frame_len = 0;
  uint8_t frame_buf[PACKED_FRAME_SIZE] = {};
#if GB_ENABLE_AUDIO
  bool audio_pending = false;
  size_t audio_len = 0;
  uint32_t audio_last_send_ms = 0;
  uint8_t audio_buf[8 + AUDIO_CHUNK_MAX_SAMPLES] = {};
#endif
};
WsClient ws_clients[MAX_WS_CLIENTS] = {};

#if GB_ENABLE_AUDIO
void reset_audio_stream_buffers() {
  apu_clear_samples();
  for (auto &c : ws_clients) {
    c.audio_last_send_ms = 0;
    if (!c.audio_pending) c.audio_len = 0;
  }
}
#endif

// fds we recently abandoned because of a send error. Stops the polling sync
// from immediately re-registering them while the framework is still tearing
// down the socket.
struct DeadFd {
  int fd = -1;
  uint32_t until_ms = 0;
};
constexpr int DEAD_FD_SLOTS = 4;
constexpr uint32_t DEAD_FD_COOLDOWN_MS = 2000;
DeadFd dead_fds[DEAD_FD_SLOTS] = {};

void mark_fd_dead(int fd, uint32_t now_ms) {
  for (auto &d : dead_fds) {
    if (d.fd == fd) {
      d.until_ms = now_ms + DEAD_FD_COOLDOWN_MS;
      return;
    }
  }
  for (auto &d : dead_fds) {
    if (d.fd == -1 || d.until_ms <= now_ms) {
      d.fd = fd;
      d.until_ms = now_ms + DEAD_FD_COOLDOWN_MS;
      return;
    }
  }
}

bool fd_is_dead(int fd, uint32_t now_ms) {
  for (auto &d : dead_fds) {
    if (d.fd == fd && d.until_ms > now_ms) return true;
  }
  return false;
}

void trigger_session_close(int fd) {
  if (server && fd >= 0) {
    httpd_sess_trigger_close(server, fd);
  }
}

uint8_t active_ws_count() {
  uint8_t count = 0;
  for (auto &c : ws_clients) {
    if (c.active) count++;
  }
  return count;
}

WsClient *find_client(int fd) {
  for (auto &c : ws_clients) {
    if (c.active && c.fd == fd) return &c;
  }
  return nullptr;
}

void register_client(int fd) {
  if (find_client(fd)) return;
  for (auto &c : ws_clients) {
    if (!c.active && !c.frame_pending
#if GB_ENABLE_AUDIO
        && !c.audio_pending
#endif
    ) {
      c.fd = fd;
      c.active = true;
      c.frame_len = 0;
#if GB_ENABLE_AUDIO
      c.audio_len = 0;
      c.audio_last_send_ms = 0;
#endif
      ESP_LOGI(TAG, "WS client registered fd=%d active=%u", fd,
               active_ws_count());
      return;
    }
  }

  // With one stream slot, a fast browser reload can leave the old socket
  // around until TCP notices. Prefer the new fd and let HTTPD retire the old
  // session, but don't overwrite a buffer that is still queued for async send.
  for (auto &c : ws_clients) {
    if (!c.active) continue;
    const int old_fd = c.fd;
    c.active = false;
    c.fd = -1;
    mark_fd_dead(old_fd, millis());
    trigger_session_close(old_fd);
    if (!c.frame_pending
#if GB_ENABLE_AUDIO
        && !c.audio_pending
#endif
    ) {
      c.fd = fd;
      c.active = true;
      c.frame_len = 0;
#if GB_ENABLE_AUDIO
      c.audio_len = 0;
      c.audio_last_send_ms = 0;
#endif
      ESP_LOGW(TAG, "WS client fd=%d replaced stale fd=%d", fd, old_fd);
      return;
    }
    ESP_LOGW(TAG, "WS client fd=%d waiting for stale fd=%d send drain", fd,
             old_fd);
    return;
  }
  ESP_LOGW(TAG, "WS client table full (fd=%d)", fd);
}

bool unregister_client(int fd) {
  bool removed = false;
  for (auto &c : ws_clients) {
    if (c.active && c.fd == fd) {
      c.active = false;
      c.fd = -1;
      // Leave frame_pending alone; the async send callback releases the
      // buffer once HTTPD finishes with it.
      removed = true;
      ESP_LOGI(TAG, "WS client unregistered fd=%d active=%u", fd,
               active_ws_count());
    }
  }
#if GB_ENABLE_AUDIO
  if (removed && active_ws_count() == 0) {
    audio_stream_enabled = false;
    reset_audio_stream_buffers();
  }
#endif
  return removed;
}

void prune_dead_clients() {
  for (auto &c : ws_clients) {
    if (!c.active) continue;
    const httpd_ws_client_info_t info = httpd_ws_get_fd_info(server, c.fd);
    if (info == HTTPD_WS_CLIENT_INVALID) {
      ESP_LOGI(TAG, "WS client fd=%d invalidated, pruning", c.fd);
      c.active = false;
      c.fd = -1;
      if (!c.frame_pending) c.frame_len = 0;
#if GB_ENABLE_AUDIO
      if (!c.audio_pending) c.audio_len = 0;
#endif
    }
  }
#if GB_ENABLE_AUDIO
  if (active_ws_count() == 0 && audio_stream_enabled) {
    audio_stream_enabled = false;
    reset_audio_stream_buffers();
  }
#endif
}

void sync_ws_clients(uint32_t now_ms) {
  if (!server) return;

  const uint32_t interval = active_ws_count() > 0 ? WS_SCAN_ACTIVE_INTERVAL_MS
                                                  : WS_SCAN_IDLE_INTERVAL_MS;
  if (last_client_scan_ms != 0 && now_ms - last_client_scan_ms < interval) {
    return;
  }
  last_client_scan_ms = now_ms;

  prune_dead_clients();

  int client_fds[MAX_HTTP_CLIENTS] = {};
  size_t client_count = MAX_HTTP_CLIENTS;
  if (httpd_get_client_list(server, &client_count, client_fds) != ESP_OK) {
    return;
  }

  for (size_t i = 0; i < client_count; ++i) {
    const int fd = client_fds[i];
    if (fd_is_dead(fd, now_ms)) continue;
    if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
      register_client(fd);
    }
  }
}

esp_err_t on_http_client_open(httpd_handle_t /*hd*/, int sockfd) {
  // TCP_NODELAY → no Nagle, important for fresh frames.
  int yes = 1;
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  // Bound the time send() may block; the async sender drops subsequent frames
  // rather than letting a shaky link build an unbounded HTTPD work backlog.
  struct timeval tv = {};
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Bigger socket-level send buffer keeps a full ~5.7 KB packed frame from
  // stalling behind tiny writes when the browser is reading normally.
  int sndbuf = 16384;
  setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  // TCP keepalive lets lwIP reap stale browser sockets after a phone roams or
  // sleeps without waiting for the WebSocket client-list poll to infer it.
  setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  int keep_idle = 10;
  int keep_interval = 3;
  int keep_count = 3;
#ifdef TCP_KEEPIDLE
  setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
#endif
#ifdef TCP_KEEPINTVL
  setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval,
             sizeof(keep_interval));
#endif
#ifdef TCP_KEEPCNT
  setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keep_count,
             sizeof(keep_count));
#endif

  return ESP_OK;
}

const char *content_type_for_path(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) return "application/octet-stream";
  if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
  if (!strcmp(dot, ".js")) return "application/javascript; charset=utf-8";
  if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
  if (!strcmp(dot, ".svg")) return "image/svg+xml";
  if (!strcmp(dot, ".png")) return "image/png";
  if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
  if (!strcmp(dot, ".ico")) return "image/x-icon";
  if (!strcmp(dot, ".woff2")) return "font/woff2";
  if (!strcmp(dot, ".txt")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

bool path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool stream_spiffs_file(httpd_req_t *req, const char *uri_path) {
  if (!spiffs_mounted) return false;

  char path[160];
  if (!uri_path || uri_path[0] == '\0') uri_path = "/";

  char clean[128];
  size_t i = 0;
  for (; i < sizeof(clean) - 1 && uri_path[i] && uri_path[i] != '?'; ++i) {
    clean[i] = uri_path[i];
  }
  clean[i] = '\0';

  if (clean[0] != '/') {
    snprintf(path, sizeof(path), "/spiffs/%s", clean);
  } else {
    snprintf(path, sizeof(path), "/spiffs%s", clean);
  }

  size_t len = strlen(path);
  if (len > 0 && path[len - 1] == '/') {
    if (len + strlen("index.html") + 1 > sizeof(path)) return false;
    strcat(path, "index.html");
  }

  if (!path_exists(path)) return false;

  struct stat st = {};
  if (stat(path, &st) != 0) return false;

  FILE *fp = fopen(path, "rb");
  if (!fp) return false;

  httpd_resp_set_type(req, content_type_for_path(path));
  if (strstr(path, ".html")) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  } else {
    httpd_resp_set_hdr(req, "Cache-Control",
                       "public, max-age=31536000, immutable");
  }

  // 4 KB chunks pair well with the LWIP send buffer + MSS. Using static here
  // would make the handler not reentrant; httpd serializes requests per
  // socket so a stack buffer is fine.
  char buf[2048];
  while (true) {
    size_t r = fread(buf, 1, sizeof(buf), fp);
    if (r == 0) break;
    if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
      fclose(fp);
      return true;
    }
  }
  fclose(fp);
  httpd_resp_send_chunk(req, nullptr, 0);
  return true;
}

void send_ui_unavailable(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_sendstr(
      req,
      "Web UI not found in SPIFFS. Build the Vite app and upload the "
      "filesystem image.");
}

void append_escaped(char *dst, size_t cap, size_t &pos, const char *src) {
  if (!src) return;
  for (; *src && pos < cap - 6; ++src) {
    char c = *src;
    switch (c) {
      case '\\':
      case '"':
        if (pos < cap - 1) dst[pos++] = '\\';
        if (pos < cap - 1) dst[pos++] = c;
        break;
      case '\n':
        if (pos + 2 < cap) {
          dst[pos++] = '\\';
          dst[pos++] = 'n';
        }
        break;
      case '\r':
        if (pos + 2 < cap) {
          dst[pos++] = '\\';
          dst[pos++] = 'r';
        }
        break;
      case '\t':
        if (pos + 2 < cap) {
          dst[pos++] = '\\';
          dst[pos++] = 't';
        }
        break;
      default:
        if (pos < cap - 1) dst[pos++] = c;
        break;
    }
  }
  if (pos < cap) dst[pos] = '\0';
}

size_t build_state_json(char *out, size_t out_cap) {
  char ssid_esc[64] = {};
  size_t ssid_pos = 0;
  append_escaped(ssid_esc, sizeof(ssid_esc), ssid_pos,
                 portal_config.ap_ssid ? portal_config.ap_ssid : "");

  const uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
  const uint32_t min_free = (uint32_t)esp_get_minimum_free_heap_size();
  const uint16_t sample_rate =
#if GB_ENABLE_AUDIO
      apu_sample_rate();
#else
      0;
#endif

  return snprintf(
      out, out_cap,
      "{\"type\":\"state\",\"network\":{\"ssid\":\"%s\",\"ip\":\"%s\","
      "\"socketClients\":%u,\"websocketPort\":%u},"
      "\"memory\":{\"freeHeap\":%lu,\"minFreeHeap\":%lu},"
      "\"stream\":{\"width\":%d,\"height\":%d,\"intervalMs\":%u},"
      "\"audio\":{\"available\":%s,\"enabled\":%s,\"sampleRate\":%u},"
      "\"input\":{\"buttons\":%u,\"directions\":%u,\"webButtons\":%u,"
      "\"webDirections\":%u,\"ff00\":%u}}",
      ssid_esc, ip_string, active_ws_count(), portal_config.websocket_port,
      (unsigned long)free_heap, (unsigned long)min_free,
      board::GAMEBOY_WIDTH, board::GAMEBOY_HEIGHT,
      portal_config.stream_interval_ms,
      board::WEB_AUDIO_ENABLED ? "true" : "false",
      (board::WEB_AUDIO_ENABLED && audio_stream_enabled) ? "true" : "false",
      sample_rate, touch_input_buttons(), touch_input_directions(),
      touch_input_web_buttons(), touch_input_web_directions(),
      mem_get_joypad_register());
}

bool extract_string_field(const char *json, size_t /*json_len*/,
                          const char *field, char *out, size_t out_cap) {
  char token[32];
  snprintf(token, sizeof(token), "\"%s\"", field);
  const char *p = strstr(json, token);
  if (!p) return false;
  p = strchr(p + strlen(token), ':');
  if (!p) return false;
  while (*p && *p != '"') p++;
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  bool escaping = false;
  while (*p && i < out_cap - 1) {
    if (escaping) {
      out[i++] = *p++;
      escaping = false;
      continue;
    }
    if (*p == '\\') {
      escaping = true;
      p++;
      continue;
    }
    if (*p == '"') break;
    out[i++] = *p++;
  }
  out[i] = '\0';
  return true;
}

bool extract_bool_field(const char *json, const char *field, bool *value) {
  char token[32];
  snprintf(token, sizeof(token), "\"%s\"", field);
  const char *p = strstr(json, token);
  if (!p) return false;
  p = strchr(p + strlen(token), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (!strncmp(p, "true", 4) || !strncmp(p, "1", 1) ||
      !strncmp(p, "\"true\"", 6)) {
    *value = true;
    return true;
  }
  if (!strncmp(p, "false", 5) || !strncmp(p, "0", 1) ||
      !strncmp(p, "\"false\"", 7)) {
    *value = false;
    return true;
  }
  return false;
}

void broadcast_text(const char *payload, size_t len) {
  if (!server) return;
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t *)payload;
  frame.len = len;
  for (auto &c : ws_clients) {
    if (!c.active) continue;
    if (httpd_ws_get_fd_info(server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      unregister_client(c.fd);
      continue;
    }
    if (httpd_ws_send_frame_async(server, c.fd, &frame) != ESP_OK) {
      const int dead_fd = c.fd;
      unregister_client(dead_fd);
      mark_fd_dead(dead_fd, millis());
      trigger_session_close(dead_fd);
    }
  }
}

void publish_state_throttled(uint32_t now_ms, bool force) {
  if (!force && last_state_publish_ms != 0 &&
      now_ms - last_state_publish_ms < STATE_PUBLISH_MIN_INTERVAL_MS) {
    return;
  }
  last_state_publish_ms = now_ms;
  char buf[512];
  size_t n = build_state_json(buf, sizeof(buf));
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  broadcast_text(buf, n);
}

void on_video_send_complete(esp_err_t err, int socket, void *arg) {
  auto *client = static_cast<WsClient *>(arg);
  if (!client) return;

  client->frame_pending = false;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "WS frame send failed fd=%d len=%u: %s", socket,
             (unsigned)client->frame_len, esp_err_to_name(err));
    unregister_client(socket);
    mark_fd_dead(socket, millis());
    trigger_session_close(socket);
    return;
  }

  sent_frames_total++;
  httpd_sess_update_lru_counter(server, socket);
  if (sent_frames_total <= 3 || (sent_frames_total % 60) == 0) {
    ESP_LOGI(TAG, "WS frame sent #%lu fd=%d len=%u",
             (unsigned long)sent_frames_total, socket,
             (unsigned)client->frame_len);
  }
}

#if GB_ENABLE_AUDIO
void on_audio_send_complete(esp_err_t err, int socket, void *arg) {
  auto *client = static_cast<WsClient *>(arg);
  if (!client) return;

  client->audio_pending = false;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "WS audio send failed fd=%d len=%u: %s", socket,
             (unsigned)client->audio_len, esp_err_to_name(err));
    unregister_client(socket);
    mark_fd_dead(socket, millis());
    trigger_session_close(socket);
  }
}
#endif

esp_err_t handle_static(httpd_req_t *req) {
  const char *uri = req->uri;
  ESP_LOGI(TAG, "static: %s method=%d", uri, req->method);
  if (!strncmp(uri, "/api/", 5)) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
    return ESP_OK;
  }

  if (stream_spiffs_file(req, uri)) return ESP_OK;

  const char *dot = strrchr(uri, '.');
  const char *slash = strrchr(uri, '/');
  if (!(dot && slash && dot > slash)) {
    if (stream_spiffs_file(req, "/index.html")) return ESP_OK;
  }

  if (spiffs_mounted) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "static file not found");
  } else {
    send_ui_unavailable(req);
  }
  return ESP_OK;
}

esp_err_t handle_api_state(httpd_req_t *req) {
  char buf[512];
  size_t n = build_state_json(buf, sizeof(buf));
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, n);
}

bool find_form_value(const char *body, const char *key, char *out, size_t cap) {
  size_t key_len = strlen(key);
  const char *p = body;
  while (p && *p) {
    if (!strncmp(p, key, key_len) && p[key_len] == '=') {
      const char *v = p + key_len + 1;
      size_t i = 0;
      while (*v && *v != '&' && i < cap - 1) {
        out[i++] = (*v == '+') ? ' ' : *v;
        v++;
      }
      out[i] = '\0';
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

esp_err_t handle_api_input(httpd_req_t *req) {
  ESP_LOGI(TAG, "api/input POST len=%d", req->content_len);
  char body[256];
  int total = 0;
  int to_read = req->content_len;
  if (to_read > (int)sizeof(body) - 1) to_read = sizeof(body) - 1;
  while (total < to_read) {
    int r = httpd_req_recv(req, body + total, to_read - total);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      break;
    }
    total += r;
  }
  body[total >= 0 ? total : 0] = '\0';

  char control[32] = {};
  char pressed_arg[8] = {};
  find_form_value(body, "control", control, sizeof(control));
  find_form_value(body, "pressed", pressed_arg, sizeof(pressed_arg));

  const bool pressed = !strcmp(pressed_arg, "1") ||
                       !strcmp(pressed_arg, "true") ||
                       !strcmp(pressed_arg, "down");
  if (control[0]) {
    touch_input_set_web_control(control, pressed, millis());
  }

  return handle_api_state(req);
}

esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err) {
  if (err != HTTPD_404_NOT_FOUND) {
    httpd_resp_send_err(req, err, nullptr);
    return ESP_OK;
  }
  return handle_static(req);
}

esp_err_t handle_ws(httpd_req_t *req) {
  const int fd = httpd_req_to_sockfd(req);

  // Register the client on every entry — idempotent. ESP-IDF v6 completes the
  // WebSocket handshake internally without delivering a HTTP_GET-method call
  // to user handlers; the first time we see this fd is for an actual data
  // frame (method != HTTP_GET). If we only registered on HTTP_GET we would
  // never start streaming video to the client even though the WS is fully
  // established.
  register_client(fd);

  if (req->method == HTTP_GET) {
    // Older ESP-IDF versions still deliver the upgrade through here. Nothing
    // to recv on the upgrade — just acknowledge.
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) return err;
  if (frame.len >= 1024) {
    ESP_LOGW(TAG, "WS frame too large fd=%d type=%d len=%u", fd, frame.type,
             (unsigned)frame.len);
    return ESP_FAIL;
  }
  if (frame.len == 0) return ESP_OK;

  uint8_t buf[1024] = {};
  frame.payload = buf;
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK) return err;
  buf[frame.len] = '\0';

  if (frame.type == HTTPD_WS_TYPE_TEXT) {
    char type_value[16] = {};
    if (!extract_string_field((const char *)buf, frame.len, "type", type_value,
                              sizeof(type_value))) {
      return ESP_OK;
    }
    if (!strcmp(type_value, "input")) {
      char control[16] = {};
      bool pressed = false;
      if (extract_string_field((const char *)buf, frame.len, "control", control,
                               sizeof(control)) &&
          extract_bool_field((const char *)buf, "pressed", &pressed)) {
        touch_input_set_web_control(control, pressed, millis());
        publish_state_throttled(millis(), false);
      }
    } else if (!strcmp(type_value, "audio")) {
      bool enabled = false;
      if (extract_bool_field((const char *)buf, "enabled", &enabled)) {
        const bool next_audio_enabled = board::WEB_AUDIO_ENABLED && enabled;
        if (audio_stream_enabled != next_audio_enabled) {
          audio_stream_enabled = next_audio_enabled;
#if GB_ENABLE_AUDIO
          reset_audio_stream_buffers();
#endif
        }
        publish_state_throttled(millis(), true);
      }
    }
  }
  return ESP_OK;
}

void on_session_close(httpd_handle_t hd, int sockfd) {
  // Reach the underlying close logic the framework uses internally so the
  // socket actually goes away (otherwise httpd_get_client_list would still
  // report it).
  if (unregister_client(sockfd)) {
    touch_input_clear_web_controls();
    if (active_ws_count() == 0) {
      audio_stream_enabled = false;
#if GB_ENABLE_AUDIO
      reset_audio_stream_buffers();
#endif
    }
  }
  // Default close path so httpd cleans the session.
  if (hd) {
    close(sockfd);
  }
}

bool mount_spiffs() {
  esp_vfs_spiffs_conf_t conf = {};
  conf.base_path = board::SPIFFS_MOUNT;
  conf.partition_label = "storage";
  conf.max_files = 6;
  conf.format_if_mount_failed = false;
  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err == ESP_ERR_INVALID_STATE) return true;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    return false;
  }
  size_t total = 0, used = 0;
  esp_spiffs_info(conf.partition_label, &total, &used);
  ESP_LOGI(TAG, "SPIFFS mounted at %s (used %u/%u)", conf.base_path,
           (unsigned)used, (unsigned)total);
  return true;
}

void on_wifi_event(void * /*arg*/, esp_event_base_t base, int32_t id,
                   void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    auto *event = static_cast<wifi_event_ap_staconnected_t *>(data);
    ESP_LOGI(TAG, "Station connected: " MACSTR " aid=%u", MAC2STR(event->mac),
             event->aid);
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    auto *event = static_cast<wifi_event_ap_stadisconnected_t *>(data);
    ESP_LOGI(TAG, "Station disconnected: " MACSTR " aid=%u reason=%u",
             MAC2STR(event->mac), event->aid, event->reason);
  }
}

bool start_wifi_ap(const WebPortalConfig &config) {
  static bool nvs_initialized = false;
  if (!nvs_initialized) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      e = nvs_flash_init();
    }
    if (e != ESP_OK) {
      ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(e));
      return false;
    }
    nvs_initialized = true;
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

  esp_event_handler_instance_t any_inst;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, &any_inst));

  wifi_config_t ap_cfg = {};
  strncpy((char *)ap_cfg.ap.ssid, config.ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
  ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
  if (!board::WEB_AP_OPEN && config.ap_password &&
      strlen(config.ap_password) >= 8) {
    strncpy((char *)ap_cfg.ap.password, config.ap_password,
            sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
  }
  ap_cfg.ap.channel = 1;
  ap_cfg.ap.max_connection = 2;
  ap_cfg.ap.beacon_interval = 100;
  ap_cfg.ap.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  // BGN keeps single-stream throughput high without the 11ax overhead.
  const esp_err_t protocol_err = esp_wifi_set_protocol(
      WIFI_IF_AP,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  if (protocol_err != ESP_OK) {
    ESP_LOGW(TAG, "WiFi protocol tuning skipped: %s",
             esp_err_to_name(protocol_err));
  }
  const esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
  if (bw_err != ESP_OK) {
    ESP_LOGW(TAG, "WiFi bandwidth tuning skipped: %s", esp_err_to_name(bw_err));
  }
  // No power save in AP mode (the radio is already serving clients).
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_netif_ip_info_t ip_info = {};
  if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
    snprintf(ip_string, sizeof(ip_string), IPSTR, IP2STR(&ip_info.ip));
  }

  ESP_LOGI(TAG, "AP up: SSID=%s ip=%s", config.ap_ssid, ip_string);
  return true;
}

bool start_http_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = portal_config.http_port;
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 12;
  cfg.max_open_sockets = MAX_HTTP_CLIENTS;
  cfg.stack_size = 6144;
  // Short timeouts: any individual send/recv that doesn't make progress within
  // 1 s is treated as failed, freeing the worker for the next request.
  cfg.send_wait_timeout = 1;
  cfg.recv_wait_timeout = 1;
  cfg.keep_alive_enable = true;
  cfg.keep_alive_idle = 10;
  cfg.keep_alive_interval = 3;
  cfg.keep_alive_count = 3;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.open_fn = on_http_client_open;
  cfg.close_fn = on_session_close;
  cfg.task_priority = tskIDLE_PRIORITY + 5;

  if (httpd_start(&server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return false;
  }

  static httpd_uri_t uri_state = {};
  uri_state.uri = "/api/state";
  uri_state.method = HTTP_GET;
  uri_state.handler = handle_api_state;

  static httpd_uri_t uri_input_state = {};
  uri_input_state.uri = "/api/input_state";
  uri_input_state.method = HTTP_GET;
  uri_input_state.handler = handle_api_state;

  static httpd_uri_t uri_input_post = {};
  uri_input_post.uri = "/api/input";
  uri_input_post.method = HTTP_POST;
  uri_input_post.handler = handle_api_input;

  static httpd_uri_t uri_ws = {};
  uri_ws.uri = "/ws";
  uri_ws.method = HTTP_GET;
  uri_ws.handler = handle_ws;
  uri_ws.is_websocket = true;
  uri_ws.handle_ws_control_frames = false;

  static httpd_uri_t uri_static = {};
  uri_static.uri = "/*";
  uri_static.method = HTTP_GET;
  uri_static.handler = handle_static;

  httpd_register_uri_handler(server, &uri_state);
  httpd_register_uri_handler(server, &uri_input_state);
  httpd_register_uri_handler(server, &uri_input_post);
  httpd_register_uri_handler(server, &uri_ws);
  httpd_register_uri_handler(server, &uri_static);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_not_found);

  return true;
}

void pack_frame(uint8_t *out, const uint8_t *framebuffer) {
  out[0] = 'G';
  out[1] = 'B';
  out[2] = 'F';
  out[3] = 1;
  out[4] = board::GAMEBOY_WIDTH;
  out[5] = board::GAMEBOY_HEIGHT;

  const size_t pixels = board::GAMEBOY_WIDTH * board::GAMEBOY_HEIGHT;
  size_t src = 0;
  size_t dst = 6;
  while (src + 3 < pixels) {
    const uint8_t p0 = framebuffer[src++] & 0x03;
    const uint8_t p1 = framebuffer[src++] & 0x03;
    const uint8_t p2 = framebuffer[src++] & 0x03;
    const uint8_t p3 = framebuffer[src++] & 0x03;
    out[dst++] = p0 | (p1 << 2) | (p2 << 4) | (p3 << 6);
  }
  if (src < pixels) {
    uint8_t packed = 0;
    uint8_t shift = 0;
    while (src < pixels) {
      packed |= (framebuffer[src++] & 0x03) << shift;
      shift += 2;
    }
    out[dst] = packed;
  }
}

void maybe_stream_frame(uint32_t now_ms, const uint8_t *framebuffer) {
  if (!server || !framebuffer || active_ws_count() == 0) {
    return;
  }
  if (now_ms - last_frame_ms < portal_config.stream_interval_ms) return;
  last_frame_ms = now_ms;

  bool any_queued = false;
  for (auto &c : ws_clients) {
    if (!c.active) continue;
    if (c.frame_pending) {
      // HTTPD has not completed the previous async send yet. Drop instead of
      // queueing stale frames or overwriting the in-flight payload buffer.
      dropped_frames_window++;
      continue;
    }
    if (httpd_ws_get_fd_info(server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      unregister_client(c.fd);
      continue;
    }

    pack_frame(c.frame_buf, framebuffer);
    c.frame_len = PACKED_FRAME_SIZE;
    c.frame_pending = true;

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = c.frame_buf;
    frame.len = c.frame_len;
    const int send_fd = c.fd;
    const esp_err_t err =
        httpd_ws_send_data_async(server, send_fd, &frame,
                                 on_video_send_complete, &c);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "WS frame queue failed fd=%d len=%u: %s", send_fd,
               (unsigned)frame.len, esp_err_to_name(err));
      c.frame_pending = false;
      unregister_client(send_fd);
      mark_fd_dead(send_fd, now_ms);
      trigger_session_close(send_fd);
      continue;
    }
    httpd_sess_update_lru_counter(server, send_fd);
    any_queued = true;
  }

  if (any_queued) {
    streamed_frames++;
    if (streamed_frames <= 3) {
      ESP_LOGI(TAG, "WS frame queued #%lu len=%u clients=%u",
               (unsigned long)streamed_frames, (unsigned)PACKED_FRAME_SIZE,
               active_ws_count());
    }
  }

  if (now_ms - last_stream_log_ms >= STATS_LOG_INTERVAL_MS) {
    ESP_LOGI(TAG, "WS stream %lu frames %lu drops in %lums (clients=%u)",
             (unsigned long)streamed_frames,
             (unsigned long)dropped_frames_window,
             (unsigned long)(now_ms - last_stream_log_ms), active_ws_count());
    streamed_frames = 0;
    dropped_frames_window = 0;
    last_stream_log_ms = now_ms;
  }
}

void maybe_stream_audio(uint32_t now_ms) {
#if GB_ENABLE_AUDIO
  if (!server || active_ws_count() == 0 || !audio_stream_enabled) return;
  for (auto &c : ws_clients) {
    if (!c.active) continue;
    if (c.audio_pending) continue;
    if (httpd_ws_get_fd_info(server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      unregister_client(c.fd);
      continue;
    }

    const size_t available = apu_available_samples();
    if (!available) return;
    if (available < AUDIO_CHUNK_TARGET_SAMPLES) {
      const bool first_audio_packet = c.audio_last_send_ms == 0;
      if (first_audio_packet ||
          now_ms - c.audio_last_send_ms < AUDIO_PARTIAL_FLUSH_MS) {
        return;
      }
    }

    size_t samples_to_read = available;
    if (samples_to_read > AUDIO_CHUNK_MAX_SAMPLES) {
      samples_to_read = AUDIO_CHUNK_MAX_SAMPLES;
    }

    const size_t sample_count =
        apu_read_samples(c.audio_buf + 8, samples_to_read);
    if (!sample_count) return;
    const uint16_t sample_rate = apu_sample_rate();
    c.audio_buf[0] = 'G';
    c.audio_buf[1] = 'B';
    c.audio_buf[2] = 'A';
    c.audio_buf[3] = 1;
    c.audio_buf[4] = sample_rate & 0xFF;
    c.audio_buf[5] = sample_rate >> 8;
    c.audio_buf[6] = sample_count & 0xFF;
    c.audio_buf[7] = sample_count >> 8;
    c.audio_len = 8 + sample_count;
    c.audio_pending = true;

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = c.audio_buf;
    frame.len = c.audio_len;
    const int send_fd = c.fd;
    const esp_err_t err =
        httpd_ws_send_data_async(server, send_fd, &frame,
                                 on_audio_send_complete, &c);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "WS audio queue failed fd=%d len=%u: %s", send_fd,
               (unsigned)frame.len, esp_err_to_name(err));
      c.audio_pending = false;
      unregister_client(send_fd);
      mark_fd_dead(send_fd, millis());
      trigger_session_close(send_fd);
      continue;
    }
    c.audio_last_send_ms = now_ms;
  }
#endif
}

}  // namespace

bool web_portal_mount_storage(void) {
  if (spiffs_mounted) return true;
  spiffs_mounted = mount_spiffs();
  return spiffs_mounted;
}

bool web_portal_begin(const WebPortalConfig &config) {
  portal_config = config;
  if (!spiffs_mounted) spiffs_mounted = mount_spiffs();
  if (!start_wifi_ap(config)) return false;
  if (!start_http_server()) return false;
  return true;
}

void web_portal_loop(uint32_t now_ms, const uint8_t *framebuffer) {
  // ESP-IDF v6's WebSocket layer doesn't call our handler with HTTP_GET on
  // upgrade, so we poll httpd_get_client_list to discover newly-established
  // sessions even before the client sends us its first input. The polling
  // intervals are aggressive only while idle — once we have a registered
  // client we slow down to once per second.
  sync_ws_clients(now_ms);
  touch_input_maintain(now_ms);
  maybe_stream_audio(now_ms);
  maybe_stream_frame(now_ms, framebuffer);
}

const char *web_portal_ip(void) { return ip_string; }

uint8_t web_portal_client_count(void) { return active_ws_count(); }
