#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

struct WebPortalConfig {
  const char *ap_ssid;
  const char *ap_password;
  uint16_t http_port = 80;
  uint16_t websocket_port = 80;
  uint16_t stream_interval_ms = 100;
};

// Mount the SPIFFS storage partition. Re-calling is a no-op.
bool web_portal_mount_storage(void);

bool web_portal_begin(const WebPortalConfig &config);
void web_portal_loop(uint32_t now_ms);
const char *web_portal_ip(void);
uint8_t web_portal_client_count(void);

#endif
