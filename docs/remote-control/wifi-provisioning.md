---
id: wifi-provisioning
title: Wi-Fi Provisioning
sidebar_label: Wi-Fi Provisioning
sidebar_position: 1
description: How the ESP32 joins your home Wi-Fi (setup AP → credentials in NVS → station join) so devices like the R36S can reach it on the LAN.
---

# Wi-Fi Provisioning

Out of the box the ESP32 runs a setup access point. Once you give it your home
network credentials it also joins that network as a **station (STA)**, so any device
on the LAN — including the R36S handheld — can reach the portal and the input
WebSocket.

## Flow

```text
boot ──► SoftAP "GameBoy-Link" + portal
            │  user submits home SSID/password
            ▼
       credentials saved to NVS  ──►  join home network (STA)
            │                              │ got IP
            ▼                              ▼
   AP stays up (fallback)          reachable at the STA IP (e.g. 192.168.5.171)
```

The device runs **AP+STA**: the setup AP never goes away, so you can always
re-provision even if the home network is wrong or unavailable.

## Provisioning steps

1. Power the ESP32 (in the N64 port or over USB). Join the **`GameBoy-Link`** Wi-Fi.
2. Open the setup page: **`http://192.168.4.1/wifi`** (a self-contained page embedded
   in the firmware — works even before the SPIFFS web UI is flashed).
3. Enter your home network SSID + password and submit. The page polls status and
   shows the **STA IP** once connected.
4. Note that STA IP — you'll type it into the R36S app. See
   [R36S Client](./r36s-client).

## HTTP API

| Method | Path | Body | Purpose |
| --- | --- | --- | --- |
| GET | `/api/wifi` | — | `{"provisioned":bool,"connected":bool,"staIp":"…","apIp":"…"}` |
| POST | `/api/wifi` | form `ssid=..&password=..` **or** JSON `{"ssid":"..","password":".."}` | Save credentials + join |

```bash
# Provision over curl (from a device on the setup AP)
curl --data 'ssid=MyHome&password=secret' http://192.168.4.1/api/wifi
curl http://192.168.4.1/api/wifi     # poll until "connected":true, read staIp
```

The STA IP also appears in `/api/state` (`network.staIp` / `staConnected` /
`provisioned`) and in the serial boot log.

## Behavior details

- **Persistence:** credentials live in NVS (`wifi` namespace) and are reused on every
  boot — no re-provisioning needed.
- **Fallback:** after `WIFI_STA_MAX_RETRIES` (8) failed join attempts the device keeps
  the setup AP available and logs the failure; fix the credentials via `/wifi`.
- **No regression:** with no stored credentials the device behaves exactly as before —
  SoftAP portal only.

:::note mDNS deferred
`espn64.local` discovery is **not** implemented yet — the `espressif/mdns` managed
component is intentionally absent to keep the PlatformIO/ESP-IDF 5.4 build resolution
working. Use the STA IP directly (it's stable on most home routers via DHCP
reservation). Manual address entry is the supported path on the R36S.
:::
