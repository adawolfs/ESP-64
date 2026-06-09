#include "web_portal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "gb_cartridge.h"
#include "joybus_rmt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "n64_accessory.h"
#include "n64_controller.h"
#include "n64_joybus.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pokemon_stadium_compat.h"
#include "save_store.h"
#include "status_display.h"
#include "transfer_pak.h"

namespace {
constexpr const char *TAG = "web_portal";
constexpr uint8_t MAX_WS_CLIENTS = 1;
constexpr size_t MAX_HTTP_CLIENTS = 7;
constexpr uint32_t STATE_PUBLISH_MIN_INTERVAL_MS = 100;
constexpr uint32_t WS_SCAN_IDLE_INTERVAL_MS = 100;
constexpr uint32_t WS_SCAN_ACTIVE_INTERVAL_MS = 1000;

WebPortalConfig portal_config;
httpd_handle_t server = nullptr;
char ip_string[16] = "0.0.0.0";      // AP IP (always available for setup)
char sta_ip_string[16] = "0.0.0.0";  // STA IP once joined to the home network
bool sta_provisioned = false;        // credentials are stored in NVS
bool sta_connected = false;          // currently joined with an IP
int sta_retry_count = 0;
esp_netif_t *sta_netif = nullptr;
bool spiffs_mounted = false;
uint32_t last_state_publish_ms = 0;
uint32_t last_client_scan_ms = 0;

struct WsClient {
  int fd = -1;
  bool active = false;
};
WsClient ws_clients[MAX_WS_CLIENTS] = {};

struct DeadFd {
  int fd = -1;
  uint32_t until_ms = 0;
};
constexpr int DEAD_FD_SLOTS = 4;
constexpr uint32_t DEAD_FD_COOLDOWN_MS = 2000;
DeadFd dead_fds[DEAD_FD_SLOTS] = {};

extern "C" {
extern const uint8_t web_asset_index_html_start[];
extern const uint8_t web_asset_index_html_end[];
extern const uint8_t web_asset_index_js_start[];
extern const uint8_t web_asset_index_js_end[];
extern const uint8_t web_asset_index_css_start[];
extern const uint8_t web_asset_index_css_end[];
extern const uint8_t web_asset_manifest_start[];
extern const uint8_t web_asset_manifest_end[];
extern const uint8_t web_asset_icon_svg_start[];
extern const uint8_t web_asset_icon_svg_end[];
extern const uint8_t web_asset_sw_js_start[];
extern const uint8_t web_asset_sw_js_end[];
}

struct EmbeddedAsset {
  const char *uri;
  const char *alias;
  const uint8_t *start;
  const uint8_t *end;
};

const EmbeddedAsset embedded_assets[] = {
    {"/index.html", "/", web_asset_index_html_start, web_asset_index_html_end},
    {"/assets/index.js", nullptr, web_asset_index_js_start,
     web_asset_index_js_end},
    {"/assets/index.css", nullptr, web_asset_index_css_start,
     web_asset_index_css_end},
    {"/manifest.webmanifest", nullptr, web_asset_manifest_start,
     web_asset_manifest_end},
    {"/icon.svg", nullptr, web_asset_icon_svg_start, web_asset_icon_svg_end},
    {"/sw.js", nullptr, web_asset_sw_js_start, web_asset_sw_js_end},
};

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
  if (server && fd >= 0) httpd_sess_trigger_close(server, fd);
}

bool unregister_client(int fd) {
  bool removed = false;
  for (auto &c : ws_clients) {
    if (c.active && c.fd == fd) {
      c.active = false;
      c.fd = -1;
      removed = true;
      ESP_LOGI(TAG, "WS client unregistered fd=%d active=%u", fd,
               active_ws_count());
    }
  }
  return removed;
}

void register_client(int fd) {
  if (find_client(fd)) return;
  for (auto &c : ws_clients) {
    if (!c.active) {
      c.fd = fd;
      c.active = true;
      ESP_LOGI(TAG, "WS client registered fd=%d active=%u", fd,
               active_ws_count());
      return;
    }
  }

  const int old_fd = ws_clients[0].fd;
  ws_clients[0].fd = fd;
  ws_clients[0].active = true;
  mark_fd_dead(old_fd, 0);
  trigger_session_close(old_fd);
  ESP_LOGW(TAG, "WS client fd=%d replaced stale fd=%d", fd, old_fd);
}

void publish_wifi_status() {
  status_display_set_wifi({
      true,
      sta_connected,
      ip_string,
      sta_ip_string,
  });
}

void publish_upload_status(StatusDisplayUploadKind kind,
                           StatusDisplayUploadPhase phase,
                           size_t bytes_done,
                           size_t bytes_total,
                           const char *detail = nullptr) {
  status_display_set_upload({
      kind,
      phase,
      bytes_done,
      bytes_total,
      detail,
  });
}

esp_err_t on_http_client_open(httpd_handle_t hd, int sockfd) {
  (void)hd;
  int keepalive = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  return ESP_OK;
}

void sync_ws_clients(uint32_t now_ms) {
  if (!server) return;
  const uint32_t interval = active_ws_count() ? WS_SCAN_ACTIVE_INTERVAL_MS
                                             : WS_SCAN_IDLE_INTERVAL_MS;
  if (last_client_scan_ms != 0 && now_ms - last_client_scan_ms < interval) {
    return;
  }
  last_client_scan_ms = now_ms;

  size_t count = MAX_HTTP_CLIENTS;
  int fds[MAX_HTTP_CLIENTS] = {};
  if (httpd_get_client_list(server, &count, fds) != ESP_OK) return;

  for (size_t i = 0; i < count; ++i) {
    const int fd = fds[i];
    if (fd_is_dead(fd, now_ms)) continue;
    if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
      register_client(fd);
    }
  }

  for (auto &c : ws_clients) {
    if (!c.active) continue;
    bool still_present = false;
    for (size_t i = 0; i < count; ++i) {
      if (fds[i] == c.fd) {
        still_present = true;
        break;
      }
    }
    if (!still_present ||
        httpd_ws_get_fd_info(server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      unregister_client(c.fd);
    }
  }
}

const char *content_type_for(const char *path) {
  const char *ext = strrchr(path, '.');
  if (!ext) return "text/plain; charset=utf-8";
  if (!strcmp(ext, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(ext, ".css")) return "text/css; charset=utf-8";
  if (!strcmp(ext, ".js")) return "application/javascript";
  if (!strcmp(ext, ".json")) return "application/json";
  if (!strcmp(ext, ".webmanifest")) return "application/manifest+json";
  if (!strcmp(ext, ".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

void normalize_uri(const char *uri, char *out, size_t out_cap) {
  if (!uri || out_cap == 0) return;
  size_t i = 0;
  while (uri[i] && uri[i] != '?' && uri[i] != '#' && i < out_cap - 1) {
    out[i] = uri[i];
    i++;
  }
  out[i] = '\0';
}

bool stream_embedded_file(httpd_req_t *req, const char *uri) {
  char clean_uri[128] = {};
  normalize_uri(uri, clean_uri, sizeof(clean_uri));
  if (strstr(clean_uri, "..")) return false;

  for (const auto &asset : embedded_assets) {
    if (strcmp(clean_uri, asset.uri) &&
        (!asset.alias || strcmp(clean_uri, asset.alias))) {
      continue;
    }

    const size_t len = static_cast<size_t>(asset.end - asset.start);
    if (len == 0) return false;
    httpd_resp_set_type(req, content_type_for(asset.uri));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_send(req, reinterpret_cast<const char *>(asset.start), len);
    return true;
  }
  return false;
}

bool stream_spiffs_file(httpd_req_t *req, const char *uri) {
  if (!spiffs_mounted) return false;
  char clean_uri[128] = {};
  normalize_uri(uri, clean_uri, sizeof(clean_uri));
  if (strstr(clean_uri, "..")) return false;

  char path[160];
  const char *relative = (!strcmp(clean_uri, "/")) ? "/index.html" : clean_uri;
  snprintf(path, sizeof(path), "%s%s", board::SPIFFS_MOUNT, relative);

  struct stat st = {};
  if (stat(path, &st) != 0 || st.st_size <= 0) return false;

  FILE *f = fopen(path, "rb");
  if (!f) return false;
  const size_t file_size = static_cast<size_t>(st.st_size);
  uint8_t *file_buf = static_cast<uint8_t *>(malloc(file_size));
  if (!file_buf) {
    fclose(f);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "static file buffer allocation failed");
    return true;
  }

  const size_t bytes_read = fread(file_buf, 1, file_size, f);
  fclose(f);
  if (bytes_read != file_size) {
    free(file_buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "static file read failed");
    return true;
  }

  httpd_resp_set_type(req, content_type_for(path));
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_send(req, reinterpret_cast<const char *>(file_buf), file_size);
  free(file_buf);
  return true;
}

void send_ui_unavailable(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(
      req,
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<title>espN64</title><body><h1>espN64</h1>"
      "<p>N64 controller and Transfer Pak runtime is active.</p>"
      "<p>Use <code>/api/state</code> or the WebSocket endpoint at "
      "<code>/ws</code>.</p></body>");
}

void append_escaped(char *dst, size_t cap, size_t &pos, const char *src) {
  while (src && *src && pos + 1 < cap) {
    const char c = *src++;
    if (c == '"' || c == '\\') {
      if (pos + 2 >= cap) break;
      dst[pos++] = '\\';
      dst[pos++] = c;
    } else if (c >= 0x20) {
      dst[pos++] = c;
    }
  }
  if (pos < cap) dst[pos] = '\0';
}

constexpr size_t STATE_JSON_CAP = 3072;

size_t build_state_json(char *out, size_t out_cap) {
  char ssid_esc[64] = {};
  char title_esc[32] = {};
  size_t ssid_pos = 0;
  size_t title_pos = 0;
  append_escaped(ssid_esc, sizeof(ssid_esc), ssid_pos,
                 portal_config.ap_ssid ? portal_config.ap_ssid : "");
  append_escaped(title_esc, sizeof(title_esc), title_pos,
                 gb_cartridge_status().title);

  const N64ControllerState controller = n64_controller_get_state();
  const TransferPakStatus &pak = transfer_pak_status();
  const GbCartridgeStatus &cart = gb_cartridge_status();
  const GbCartridgeSaveDebug save = gb_cartridge_save_debug();
  const SaveStoreStatus &store = save_store_status();
  const N64JoybusDebug &joybus = n64_joybus_debug();
  const N64AccessoryDebug &accessory = n64_accessory_debug();
  const PokemonStadiumCompatStatus compat = pokemon_stadium_compat_status();

  return snprintf(
      out, out_cap,
      "{\"type\":\"state\",\"network\":{\"ssid\":\"%s\",\"ip\":\"%s\","
      "\"staIp\":\"%s\",\"staConnected\":%s,\"provisioned\":%s,"
      "\"socketClients\":%u,\"websocketPort\":%u},"
      "\"memory\":{\"freeHeap\":%lu,\"minFreeHeap\":%lu},"
      "\"controller\":{\"buttons\":%u,\"stickX\":%d,\"stickY\":%d},"
      "\"transferPak\":{\"powered\":%s,\"accessEnabled\":%s,\"bank\":%u,"
      "\"status\":%u,\"reads\":%lu,\"writes\":%lu,\"invalidAccesses\":%lu},"
      "\"cartridge\":{\"romLoaded\":%s,\"saveLoaded\":%s,\"saveStubbed\":%s,"
      "\"saveDirty\":%s,\"savePersisted\":%s,"
      "\"boundsFault\":%s,\"title\":\"%s\",\"type\":%u,\"romBanks\":%u,"
      "\"ramBanks\":%u,\"headerChecksum\":%u,"
      "\"saveWriteSeq\":%lu,\"saveChangedBytes\":%lu,"
      "\"saveLastOffset\":%lu,\"saveLastGbAddress\":%u,"
      "\"saveLastValue\":%u,\"savePending\":%s,"
      "\"saveLoadedPersisted\":%s,\"saveLoadResult\":\"%s\","
      "\"saveFlushResult\":\"%s\",\"saveLastFlushOk\":%s,"
      "\"saveFlushCount\":%lu,\"saveFailedFlushCount\":%lu,"
      "\"saveLastPersistedSeq\":%lu,\"saveLastChangeMs\":%lu,"
      "\"saveLastFlushMs\":%lu,\"saveLastLoadSize\":%u,"
      "\"saveFlushReason\":\"%s\"},"
      "\"compat\":{\"accessoryPresent\":%s,\"romHeaderOk\":%s,"
      "\"saveReadOnlyOrStubbed\":%s},"
      "\"debug\":{\"joybusStatus\":%lu,\"joybusPoll\":%lu,"
      "\"joybusAccessoryReads\":%lu,\"joybusAccessoryWrites\":%lu,"
      "\"joybusMalformed\":%lu,\"joybusTimingErrors\":%lu,"
      "\"joybusResponseFailures\":%lu,\"joybusDroppedStarts\":%lu,"
      "\"accessoryReads\":%lu,"
      "\"accessoryWrites\":%lu,\"accessoryMalformed\":%lu}}",
      ssid_esc, ip_string, sta_ip_string, sta_connected ? "true" : "false",
      sta_provisioned ? "true" : "false", active_ws_count(),
      portal_config.websocket_port,
      (unsigned long)esp_get_free_heap_size(),
      (unsigned long)esp_get_minimum_free_heap_size(), controller.buttons,
      controller.stick_x, controller.stick_y, pak.powered ? "true" : "false",
      pak.access_enabled ? "true" : "false", pak.bank, pak.status,
      (unsigned long)pak.reads, (unsigned long)pak.writes,
      (unsigned long)pak.invalid_accesses, cart.rom_loaded ? "true" : "false",
      cart.save_loaded ? "true" : "false",
      cart.save_stubbed ? "true" : "false",
      gb_cartridge_save_dirty() ? "true" : "false",
      save_store_persisted() ? "true" : "false",
      cart.bounds_fault ? "true" : "false", title_esc, cart.cartridge_type,
      cart.rom_bank_count, cart.ram_bank_count, cart.header_checksum,
      (unsigned long)save.write_seq, (unsigned long)save.changed_bytes,
      (unsigned long)save.last_offset, save.last_gb_address,
      save.last_value, store.pending ? "true" : "false",
      store.loaded_persisted ? "true" : "false",
      save_store_load_result_name(store.load_result),
      save_store_flush_result_name(store.flush_result),
      store.last_flush_ok ? "true" : "false",
      (unsigned long)store.flush_count,
      (unsigned long)store.failed_flush_count,
      (unsigned long)store.last_persisted_seq,
      (unsigned long)store.last_change_ms,
      (unsigned long)store.last_flush_ms,
      (unsigned)store.last_load_size,
      save_store_flush_reason_name(store.flush_reason),
      compat.accessory_present ? "true" : "false",
      compat.rom_header_ok ? "true" : "false",
      compat.save_read_only_or_stubbed ? "true" : "false",
      (unsigned long)joybus.status_commands,
      (unsigned long)joybus.poll_commands,
      (unsigned long)joybus.accessory_reads,
      (unsigned long)joybus.accessory_writes,
      (unsigned long)joybus.malformed_frames,
      (unsigned long)joybus.timing_errors,
      (unsigned long)joybus.response_failures,
      (unsigned long)joybus.dropped_starts,
      (unsigned long)accessory.reads, (unsigned long)accessory.writes,
      (unsigned long)accessory.malformed);
}

bool extract_string_field(const char *json, const char *field, char *out,
                          size_t out_cap) {
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

// Extracts a (possibly signed) integer JSON field. Returns false if absent.
bool extract_int_field(const char *json, const char *field, int *value) {
  char token[32];
  snprintf(token, sizeof(token), "\"%s\"", field);
  const char *p = strstr(json, token);
  if (!p) return false;
  p = strchr(p + strlen(token), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '"') p++;
  char *end = nullptr;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  *value = (int)v;
  return true;
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
      mark_fd_dead(dead_fd, 0);
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
  // Built on the heap, not the stack: the JSON is ~3 KB and this runs in the
  // httpd task on top of handle_ws's 1 KB frame buffer, which overflows the
  // httpd task stack. malloc keeps it reentrant (also called from
  // web_portal_loop's task), unlike a shared static buffer.
  char *buf = static_cast<char *>(malloc(STATE_JSON_CAP));
  if (!buf) return;
  size_t n = build_state_json(buf, STATE_JSON_CAP);
  if (n >= STATE_JSON_CAP) n = STATE_JSON_CAP - 1;
  broadcast_text(buf, n);
  free(buf);
}

esp_err_t handle_api_state(httpd_req_t *req) {
  char *buf = static_cast<char *>(malloc(STATE_JSON_CAP));
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);
    return ESP_FAIL;
  }
  size_t n = build_state_json(buf, STATE_JSON_CAP);
  if (n >= STATE_JSON_CAP) n = STATE_JSON_CAP - 1;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_send(req, buf, n);
  free(buf);
  return err;
}

esp_err_t handle_api_save(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=\"save.srm\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(
      req, reinterpret_cast<const char *>(gb_cartridge_save_data()),
      gb_cartridge_save_size());
}

esp_err_t handle_api_save_reset(httpd_req_t *req) {
  const bool ok = save_store_reset();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, ok ? "{\"reset\":true}" : "{\"reset\":false}");
  return ESP_OK;
}

// POST /api/save — upload (load) a 32 KB savegame and persist it.
esp_err_t handle_api_save_post(httpd_req_t *req) {
  const size_t expected = gb_cartridge_save_size();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (req->content_len != expected) {
    publish_upload_status(StatusDisplayUploadKind::Save,
                          StatusDisplayUploadPhase::Rejected, 0,
                          req->content_len, "BAD SIZE");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"loaded\":false,\"error\":\"bad size\"}");
    return ESP_OK;
  }
  uint8_t *buf = static_cast<uint8_t *>(malloc(expected));
  if (!buf) {
    publish_upload_status(StatusDisplayUploadKind::Save,
                          StatusDisplayUploadPhase::Failed, 0, expected,
                          "NO MEM");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);
    return ESP_FAIL;
  }
  size_t got = 0;
  publish_upload_status(StatusDisplayUploadKind::Save,
                        StatusDisplayUploadPhase::Receiving, 0, expected,
                        "RECV");
  while (got < expected) {
    int r = httpd_req_recv(req, reinterpret_cast<char *>(buf) + got,
                           expected - got);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      break;
    }
    got += r;
    publish_upload_status(StatusDisplayUploadKind::Save,
                          StatusDisplayUploadPhase::Receiving, got, expected,
                          "RECV");
  }
  bool ok = got == expected;
  if (ok) {
    save_store_set_busy(true);
    joybus_rmt_pause();
    ok = gb_cartridge_load_save(buf, expected) && save_store_force_persist();
    joybus_rmt_resume();
    save_store_set_busy(false);
  }
  free(buf);
  publish_upload_status(StatusDisplayUploadKind::Save,
                        ok ? StatusDisplayUploadPhase::Success
                           : StatusDisplayUploadPhase::Failed,
                        got, expected, ok ? "LOADED" : "FAILED");
  httpd_resp_sendstr(req, ok ? "{\"loaded\":true}" : "{\"loaded\":false}");
  return ESP_OK;
}

// POST /api/rom — replace the active game. Streams the ROM straight to the `rom`
// flash partition in aligned 4 KB sectors (never buffers the whole 1 MB), pausing
// the Joy-Bus so the flash writes don't contend with the response path.
esp_err_t handle_api_rom_post(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const int total = req->content_len;
  if (total <= 0x150 || total > 0x100000) {
    publish_upload_status(StatusDisplayUploadKind::Rom,
                          StatusDisplayUploadPhase::Rejected, 0,
                          static_cast<size_t>(total > 0 ? total : 0),
                          "BAD SIZE");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"loaded\":false,\"error\":\"bad size\"}");
    return ESP_OK;
  }

  constexpr size_t kSector = 4096;
  uint8_t *sec = static_cast<uint8_t *>(malloc(kSector));
  uint8_t *first_sec = static_cast<uint8_t *>(malloc(kSector));
  if (!sec || !first_sec) {
    free(sec);
    free(first_sec);
    publish_upload_status(StatusDisplayUploadKind::Rom,
                          StatusDisplayUploadPhase::Failed, 0,
                          static_cast<size_t>(total), "NO MEM");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, nullptr);
    return ESP_FAIL;
  }

  save_store_set_busy(true);
  joybus_rmt_pause();
  bool ok = gb_cartridge_rom_write_begin(static_cast<size_t>(total));
  publish_upload_status(StatusDisplayUploadKind::Rom,
                        StatusDisplayUploadPhase::Receiving, 0,
                        static_cast<size_t>(total), "RECV");
  size_t flash_off = 0;
  size_t sec_fill = 0;
  int received = 0;
  bool first_sec_ready = false;
  while (ok && received < total) {
    int r = httpd_req_recv(req, reinterpret_cast<char *>(sec) + sec_fill,
                           kSector - sec_fill);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      ok = false;
      break;
    }
    sec_fill += r;
    received += r;
    publish_upload_status(StatusDisplayUploadKind::Rom,
                          StatusDisplayUploadPhase::Receiving,
                          static_cast<size_t>(received),
                          static_cast<size_t>(total), "RECV");
    if (sec_fill == kSector) {
      if (flash_off == 0) {
        memcpy(first_sec, sec, kSector);
        first_sec_ready = true;
      } else {
        ok = gb_cartridge_rom_write_chunk(flash_off, sec, kSector);
      }
      flash_off += kSector;
      sec_fill = 0;
    }
  }
  if (ok && received != total) ok = false;
  if (ok && sec_fill > 0) {  // flush final partial sector, padded with 0xFF
    memset(sec + sec_fill, 0xFF, kSector - sec_fill);
    if (flash_off == 0) {
      memcpy(first_sec, sec, kSector);
      first_sec_ready = true;
    } else {
      ok = gb_cartridge_rom_write_chunk(flash_off, sec, kSector);
    }
  }
  // Program the header sector last. If an upload is interrupted, the erased
  // header remains invalid after reboot instead of exposing a partial ROM.
  if (ok && first_sec_ready) {
    ok = gb_cartridge_rom_write_chunk(0, first_sec, kSector);
  }
  free(sec);
  free(first_sec);
  if (ok) ok = gb_cartridge_rom_write_finish();
  if (ok) {
    // New game: reset the save (a previous game's save must not be presented) and
    // re-init the Transfer Pak mapper from the new header.
    gb_cartridge_clear_save();
    save_store_reset();
    save_store_arm_power_loss_slot();
    transfer_pak_init();
  }
  joybus_rmt_resume();
  save_store_set_busy(false);
  publish_upload_status(StatusDisplayUploadKind::Rom,
                        ok ? StatusDisplayUploadPhase::Success
                           : StatusDisplayUploadPhase::Failed,
                        static_cast<size_t>(received),
                        static_cast<size_t>(total),
                        ok ? gb_cartridge_status().title : "FAILED");

  char body[96];
  int n = snprintf(body, sizeof(body),
                   "{\"loaded\":%s,\"title\":\"%s\",\"type\":%u}",
                   ok ? "true" : "false",
                   ok ? gb_cartridge_status().title : "",
                   ok ? gb_cartridge_status().cartridge_type : 0);
  if (n < 0 || n >= (int)sizeof(body)) {
    httpd_resp_sendstr(req, ok ? "{\"loaded\":true}" : "{\"loaded\":false}");
  } else {
    httpd_resp_send(req, body, n);
  }
  return ESP_OK;
}

esp_err_t handle_api_input(httpd_req_t *req) {
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
  if (control[0]) n64_controller_set_control(control, pressed);
  return handle_api_state(req);
}

// Wi-Fi helpers are defined further below (next to the Wi-Fi bring-up); declare them
// here so the /api/wifi handlers can use them.
bool save_wifi_creds(const char *ssid, const char *pass);
bool wifi_sta_connect(const char *ssid, const char *pass);

// GET /api/wifi — report provisioning / station status so a client can confirm the
// device joined the home network and learn the reachable STA IP.
esp_err_t handle_api_wifi_get(httpd_req_t *req) {
  char body[160];
  int n = snprintf(body, sizeof(body),
                   "{\"provisioned\":%s,\"connected\":%s,\"staIp\":\"%s\","
                   "\"apIp\":\"%s\"}",
                   sta_provisioned ? "true" : "false",
                   sta_connected ? "true" : "false", sta_ip_string, ip_string);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, body, n);
  return ESP_OK;
}

// POST /api/wifi — provision the home network. Accepts form-encoded
// (`ssid=..&password=..`) or JSON (`{"ssid":"..","password":".."}`). Persists the
// credentials and starts a join; the device stays reachable on the setup AP.
esp_err_t handle_api_wifi_post(httpd_req_t *req) {
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

  char ssid[33] = {};
  char pass[65] = {};
  // Try JSON first, then fall back to form-encoded.
  if (!extract_string_field(body, "ssid", ssid, sizeof(ssid))) {
    find_form_value(body, "ssid", ssid, sizeof(ssid));
  }
  if (!extract_string_field(body, "password", pass, sizeof(pass))) {
    find_form_value(body, "password", pass, sizeof(pass));
  }

  httpd_resp_set_type(req, "application/json");
  if (ssid[0] == '\0') {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"saved\":false,\"error\":\"ssid required\"}");
    return ESP_OK;
  }
  const bool saved = save_wifi_creds(ssid, pass);
  if (saved) wifi_sta_connect(ssid, pass);
  httpd_resp_sendstr(req, saved ? "{\"saved\":true,\"connecting\":true}"
                                : "{\"saved\":false}");
  return ESP_OK;
}

// GET /wifi — a self-contained Wi-Fi setup page. Embedded in the firmware so it works
// on the bare setup AP even before the SPIFFS web UI is flashed.
esp_err_t handle_wifi_page(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(
      req,
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<title>espN64 Wi-Fi setup</title>"
      "<body style='font-family:sans-serif;max-width:24rem;margin:2rem auto'>"
      "<h1>espN64 Wi-Fi setup</h1>"
      "<p>Join the device to your home network so the R36S (and other devices) "
      "can reach it.</p>"
      "<form id=f><label>Network (SSID)<br><input name=ssid required "
      "style='width:100%'></label><br><br>"
      "<label>Password<br><input name=password type=password "
      "style='width:100%'></label><br><br>"
      "<button>Save &amp; connect</button></form>"
      "<pre id=s></pre>"
      "<script>"
      "const s=document.getElementById('s');"
      "async function st(){try{const r=await fetch('/api/wifi');const j=await "
      "r.json();s.textContent='provisioned='+j.provisioned+' connected='+"
      "j.connected+' staIp='+j.staIp;}catch(e){s.textContent=e;}}"
      "document.getElementById('f').onsubmit=async(e)=>{e.preventDefault();"
      "const d=new FormData(e.target);const b=new URLSearchParams(d);"
      "s.textContent='saving...';await fetch('/api/wifi',{method:'POST',body:b});"
      "setTimeout(st,1500);};st();setInterval(st,3000);"
      "</script></body>");
  return ESP_OK;
}

esp_err_t handle_static(httpd_req_t *req) {
  if (!strncmp(req->uri, "/api/", 5)) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
    return ESP_OK;
  }
  if (stream_embedded_file(req, req->uri)) return ESP_OK;
  if (stream_spiffs_file(req, req->uri)) return ESP_OK;
  char clean_uri[128] = {};
  normalize_uri(req->uri, clean_uri, sizeof(clean_uri));
  const char *dot = strrchr(clean_uri, '.');
  const char *slash = strrchr(clean_uri, '/');
  if (!(dot && slash && dot > slash) &&
      (stream_embedded_file(req, "/index.html") ||
       stream_spiffs_file(req, "/index.html"))) {
    return ESP_OK;
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

esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t err) {
  if (err != HTTPD_404_NOT_FOUND) {
    httpd_resp_send_err(req, err, nullptr);
    return ESP_OK;
  }
  return handle_static(req);
}

esp_err_t handle_ws(httpd_req_t *req) {
  const int fd = httpd_req_to_sockfd(req);
  register_client(fd);
  if (req->method == HTTP_GET) return ESP_OK;

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) return err;
  if (frame.len >= 1024) return ESP_FAIL;
  if (frame.len == 0) return ESP_OK;

  uint8_t buf[1024] = {};
  frame.payload = buf;
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK) return err;
  buf[frame.len] = '\0';

  if (frame.type == HTTPD_WS_TYPE_TEXT) {
    char type_value[16] = {};
    if (!extract_string_field((const char *)buf, "type", type_value,
                              sizeof(type_value))) {
      return ESP_OK;
    }
    if (!strcmp(type_value, "input")) {
      char control[32] = {};
      bool pressed = false;
      if (extract_string_field((const char *)buf, "control", control,
                               sizeof(control)) &&
          extract_bool_field((const char *)buf, "pressed", &pressed)) {
        n64_controller_set_control(control, pressed);
        publish_state_throttled(0, false);
      }
    } else if (!strcmp(type_value, "stick")) {
      // Analog stick from a gamepad client (e.g. the R36S). x/y are signed and
      // clamped to the N64 controller's int8 range.
      int x = 0, y = 0;
      const bool have_x = extract_int_field((const char *)buf, "x", &x);
      const bool have_y = extract_int_field((const char *)buf, "y", &y);
      if (have_x || have_y) {
        if (x < -128) x = -128; else if (x > 127) x = 127;
        if (y < -128) y = -128; else if (y > 127) y = 127;
        n64_controller_set_stick((int8_t)x, (int8_t)y);
        publish_state_throttled(0, false);
      }
    } else if (!strcmp(type_value, "state")) {
      publish_state_throttled(0, true);
    }
  }
  return ESP_OK;
}

void on_session_close(httpd_handle_t hd, int sockfd) {
  unregister_client(sockfd);
  if (hd) close(sockfd);
}

bool mount_spiffs() {
  esp_vfs_spiffs_conf_t conf = {};
  conf.base_path = board::SPIFFS_MOUNT;
  conf.partition_label = "storage";
  conf.max_files = 6;
  // Format on mount failure so the storage partition self-heals after a chip
  // erase or a partition-table resize (otherwise the FS never exists and save
  // writes fail with "temp open failed"). The web UI also has an embedded
  // fallback, so a freshly-formatted (empty) FS still serves the portal.
  conf.format_if_mount_failed = true;
  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err == ESP_ERR_INVALID_STATE) return true;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    return false;
  }
  size_t total = 0;
  size_t used = 0;
  esp_spiffs_info(conf.partition_label, &total, &used);
  ESP_LOGI(TAG, "SPIFFS mounted at %s (used %u/%u)", conf.base_path,
           (unsigned)used, (unsigned)total);
  return true;
}

bool ensure_nvs() {
  static bool nvs_initialized = false;
  if (nvs_initialized) return true;
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
  return true;
}

// Loads stored home-network credentials. Returns true only when a non-empty SSID
// is present. `pass` may come back empty (open networks).
bool load_wifi_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
  if (!ensure_nvs()) return false;
  nvs_handle_t h;
  if (nvs_open(board::WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sl = ssid_cap, pl = pass_cap;
  ssid[0] = '\0';
  pass[0] = '\0';
  esp_err_t es = nvs_get_str(h, board::WIFI_NVS_KEY_SSID, ssid, &sl);
  nvs_get_str(h, board::WIFI_NVS_KEY_PASS, pass, &pl);
  nvs_close(h);
  return es == ESP_OK && ssid[0] != '\0';
}

bool save_wifi_creds(const char *ssid, const char *pass) {
  if (!ensure_nvs()) return false;
  nvs_handle_t h;
  if (nvs_open(board::WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_str(h, board::WIFI_NVS_KEY_SSID, ssid ? ssid : "");
  if (e == ESP_OK) e = nvs_set_str(h, board::WIFI_NVS_KEY_PASS, pass ? pass : "");
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

bool clear_wifi_creds() {
  if (!ensure_nvs()) return false;
  nvs_handle_t h;
  if (nvs_open(board::WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  nvs_erase_key(h, board::WIFI_NVS_KEY_SSID);
  nvs_erase_key(h, board::WIFI_NVS_KEY_PASS);
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

// Applies stored/given credentials to the STA interface and starts a join. Safe to
// call after Wi-Fi is started; updates sta_provisioned and resets the retry counter.
bool wifi_sta_connect(const char *ssid, const char *pass) {
  if (!ssid || ssid[0] == '\0') return false;
  wifi_config_t sta_cfg = {};
  strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
  if (pass) strncpy((char *)sta_cfg.sta.password, pass,
                    sizeof(sta_cfg.sta.password) - 1);
  sta_cfg.sta.threshold.authmode =
      (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) return false;
  sta_retry_count = 0;
  sta_provisioned = true;
  esp_wifi_disconnect();
  esp_err_t e = esp_wifi_connect();
  ESP_LOGI(TAG, "joining home network SSID=%s (%s)", ssid, esp_err_to_name(e));
  return e == ESP_OK;
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    // Only auto-connect once credentials have been applied (wifi_sta_connect sets
    // sta_provisioned); avoids a spurious join with an empty config at startup.
    if (sta_provisioned) esp_wifi_connect();
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    sta_connected = false;
    strncpy(sta_ip_string, "0.0.0.0", sizeof(sta_ip_string));
    publish_wifi_status();
    if (sta_provisioned && sta_retry_count < board::WIFI_STA_MAX_RETRIES) {
      sta_retry_count++;
      ESP_LOGW(TAG, "STA disconnected; retry %d/%d", sta_retry_count,
               board::WIFI_STA_MAX_RETRIES);
      esp_wifi_connect();
    } else if (sta_provisioned) {
      ESP_LOGW(TAG, "STA join failed after %d retries; setup AP still available",
               board::WIFI_STA_MAX_RETRIES);
    }
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    sta_connected = true;
    sta_retry_count = 0;
    snprintf(sta_ip_string, sizeof(sta_ip_string), IPSTR,
             IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "joined home network: STA ip=%s (reach the portal/WebSocket here)",
             sta_ip_string);
    publish_wifi_status();
    return;
  }
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
  if (!ensure_nvs()) return false;

  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t loop_err = esp_event_loop_create_default();
  if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_err);
  }
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  // STA netif so the device can also join a home network; the AP stays up as a
  // re-provisioning fallback (AP+STA).
  sta_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

  esp_event_handler_instance_t any_inst;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, &any_inst));
  esp_event_handler_instance_t ip_inst;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, &ip_inst));

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
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_netif_ip_info_t ip_info = {};
  if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
    snprintf(ip_string, sizeof(ip_string), IPSTR, IP2STR(&ip_info.ip));
  }
  ESP_LOGI(TAG, "AP up: SSID=%s ip=%s", config.ap_ssid, ip_string);
  publish_wifi_status();

  // If a home network was provisioned earlier, join it now (STA). The got-IP
  // event records the STA address; failures keep the setup AP available.
  char ssid[33] = {};
  char pass[65] = {};
  if (load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
    ESP_LOGI(TAG, "stored home network found; connecting as station");
    wifi_sta_connect(ssid, pass);
  } else {
    ESP_LOGI(TAG, "no home network stored; provision via the setup portal");
  }
  return true;
}

bool start_http_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = portal_config.http_port;
  cfg.lru_purge_enable = true;
  cfg.max_uri_handlers = 14;
  cfg.max_open_sockets = MAX_HTTP_CLIENTS;
  cfg.stack_size = 8192;
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

  static httpd_uri_t uri_save = {};
  uri_save.uri = "/api/save";
  uri_save.method = HTTP_GET;
  uri_save.handler = handle_api_save;

  static httpd_uri_t uri_save_reset = {};
  uri_save_reset.uri = "/api/save_reset";
  uri_save_reset.method = HTTP_POST;
  uri_save_reset.handler = handle_api_save_reset;

  static httpd_uri_t uri_save_post = {};
  uri_save_post.uri = "/api/save";
  uri_save_post.method = HTTP_POST;
  uri_save_post.handler = handle_api_save_post;

  static httpd_uri_t uri_rom_post = {};
  uri_rom_post.uri = "/api/rom";
  uri_rom_post.method = HTTP_POST;
  uri_rom_post.handler = handle_api_rom_post;

  static httpd_uri_t uri_wifi_get = {};
  uri_wifi_get.uri = "/api/wifi";
  uri_wifi_get.method = HTTP_GET;
  uri_wifi_get.handler = handle_api_wifi_get;

  static httpd_uri_t uri_wifi_post = {};
  uri_wifi_post.uri = "/api/wifi";
  uri_wifi_post.method = HTTP_POST;
  uri_wifi_post.handler = handle_api_wifi_post;

  static httpd_uri_t uri_wifi_page = {};
  uri_wifi_page.uri = "/wifi";
  uri_wifi_page.method = HTTP_GET;
  uri_wifi_page.handler = handle_wifi_page;

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
  httpd_register_uri_handler(server, &uri_save);
  httpd_register_uri_handler(server, &uri_save_reset);
  httpd_register_uri_handler(server, &uri_save_post);
  httpd_register_uri_handler(server, &uri_rom_post);
  httpd_register_uri_handler(server, &uri_wifi_get);
  httpd_register_uri_handler(server, &uri_wifi_post);
  httpd_register_uri_handler(server, &uri_wifi_page);
  httpd_register_uri_handler(server, &uri_ws);
  httpd_register_uri_handler(server, &uri_static);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_not_found);
  return true;
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
  publish_wifi_status();
  return true;
}

void web_portal_loop(uint32_t now_ms) {
  sync_ws_clients(now_ms);
  publish_state_throttled(now_ms, false);
}

const char *web_portal_ip(void) { return ip_string; }

uint8_t web_portal_client_count(void) { return active_ws_count(); }
