#include "net_diag.h"

#if MESFLOW_DEBUG_API

#include <WiFi.h>
#include <esp_system.h>

#include "../protocol/protocol_codec.h"

namespace kiosk::network {

namespace {

constexpr uint16_t kHttpPort = 80;
constexpr unsigned long kTcpConnectTimeoutMs = 5000;
constexpr unsigned long kHttpReadTimeoutMs = 5000;

const char* wl_status_name(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "UNKNOWN";
  }
}

// Raw TCP connect to an already-resolved IP:port -- no DNS, no HTTP layer
// above it. Returns connect duration in *out_ms regardless of outcome.
bool tcp_connect(const IPAddress& ip, uint16_t port, unsigned long* out_ms) {
  WiFiClient client;
  client.setTimeout(kTcpConnectTimeoutMs);
  unsigned long t0 = millis();
  bool ok = client.connect(ip, port);
  *out_ms = millis() - t0;
  if (ok) client.stop();
  return ok;
}

// Plain HTTP/1.1 GET over an already-open WiFiClient -- no HTTPClient
// wrapper, so this exercises exactly the same socket layer HTTPClient
// would but with nothing else in between. Captures the status line only
// (not the full body -- this is a connectivity probe, not a protocol
// client) plus total bytes read before EOF/timeout.
struct HttpProbeResult {
  bool connected = false;
  unsigned long connect_ms = 0;
  bool got_response = false;
  String status_line;
  size_t bytes_read = 0;
  unsigned long total_ms = 0;
};

HttpProbeResult http_probe(const char* host, const char* path) {
  HttpProbeResult result;
  IPAddress resolved;
  if (!WiFi.hostByName(host, resolved)) return result;  // caller already ran/logged the DNS step separately

  WiFiClient client;
  client.setTimeout(kTcpConnectTimeoutMs);
  unsigned long t0 = millis();
  result.connected = client.connect(resolved, kHttpPort);
  result.connect_ms = millis() - t0;
  if (!result.connected) return result;

  String request = String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
                    "\r\nConnection: close\r\n\r\n";
  client.print(request);

  unsigned long read_t0 = millis();
  bool got_first_line = false;
  while (client.connected() || client.available()) {
    if (millis() - read_t0 > kHttpReadTimeoutMs) break;
    if (client.available()) {
      if (!got_first_line) {
        result.status_line = client.readStringUntil('\n');
        got_first_line = true;
        result.got_response = true;
        result.bytes_read += result.status_line.length();
      } else {
        // Drain the rest without keeping it -- only the status line and a
        // byte count matter for this probe.
        int c = client.read();
        if (c < 0) break;
        ++result.bytes_read;
      }
    }
  }
  result.total_ms = millis() - t0;
  client.stop();
  return result;
}

void print_target(Stream& out, const char* label, const char* host) {
  unsigned long dns_ms = 0;
  IPAddress resolved;
  unsigned long dns_t0 = millis();
  bool dns_ok = WiFi.hostByName(host, resolved);
  dns_ms = millis() - dns_t0;

  out.printf("\"%s\":{\"host\":\"%s\",\"dns\":{\"ok\":%s,\"ip\":\"%s\",\"ms\":%lu}", label, host,
             dns_ok ? "true" : "false", dns_ok ? resolved.toString().c_str() : "", dns_ms);

  if (!dns_ok) {
    out.print(",\"tcp\":null,\"http\":null}");
    return;
  }

  unsigned long tcp_ms = 0;
  uint32_t heap_before = ESP.getFreeHeap();
  bool tcp_ok = tcp_connect(resolved, kHttpPort, &tcp_ms);
  uint32_t heap_after = ESP.getFreeHeap();
  out.printf(",\"tcp\":{\"ok\":%s,\"ms\":%lu,\"heap_before\":%u,\"heap_after\":%u}",
             tcp_ok ? "true" : "false", tcp_ms, heap_before, heap_after);

  if (!tcp_ok) {
    out.print(",\"http\":null}");
    return;
  }

  HttpProbeResult http = http_probe(host, "/api/kiosk/v2/health");
  std::string escaped_status = kiosk::protocol::json_escape(http.status_line.c_str());
  out.printf(",\"http\":{\"got_response\":%s,\"status_line\":\"%s\",\"bytes\":%u,\"ms\":%lu}}",
             http.got_response ? "true" : "false", escaped_status.c_str(),
             static_cast<unsigned>(http.bytes_read), http.total_ms);
}

}  // namespace

void run_net_diag(Stream& out, uint32_t wifi_reconnect_count) {
  out.println("##MFDBG-BEGIN net-diag##");
  out.print("{");

  // --- Layer 1: WiFi association state (already-connected accessors only
  // -- this diagnostic does not itself reconnect/disconnect WiFi) ---
  wl_status_t status = WiFi.status();
  out.printf(
      "\"wifi\":{\"status\":\"%s\",\"connected\":%s,\"ssid\":\"%s\",\"bssid\":\"%s\","
      "\"channel\":%d,\"rssi\":%d,\"local_ip\":\"%s\",\"gateway_ip\":\"%s\","
      "\"subnet_mask\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\",\"reconnect_count\":%u},",
      wl_status_name(status), status == WL_CONNECTED ? "true" : "false", WiFi.SSID().c_str(),
      WiFi.BSSIDstr().c_str(), WiFi.channel(), WiFi.RSSI(), WiFi.localIP().toString().c_str(),
      WiFi.gatewayIP().toString().c_str(), WiFi.subnetMask().toString().c_str(),
      WiFi.dnsIP(0).toString().c_str(), WiFi.dnsIP(1).toString().c_str(), wifi_reconnect_count);

  // --- Layer 2: gateway reachability, raw TCP (no DNS involved -- the
  // gateway IP is already known from DHCP) ---
  unsigned long gw_ms = 0;
  bool gw_ok = tcp_connect(WiFi.gatewayIP(), kHttpPort, &gw_ms);
  out.printf("\"gateway_tcp80\":{\"ok\":%s,\"ms\":%lu},", gw_ok ? "true" : "false", gw_ms);

  // --- Layers 3-5: DNS -> raw TCP -> plain HTTP, per target ---
  print_target(out, "dev", "dev.mesflow.net");
  out.print(",");
  print_target(out, "prod", "prod.mesflow.net");

  out.print("}\n");
  out.println("##MFDBG-END##");
}

}  // namespace kiosk::network

#endif  // MESFLOW_DEBUG_API
