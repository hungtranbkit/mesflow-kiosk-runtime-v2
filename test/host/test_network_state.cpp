// Host test: plain C++, no Arduino. "Final Runtime Closure" pass, §8.
#include <cstdio>
#include <cstring>

#include "../../firmware/kiosk_runtime_v2/src/network/network_state.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::network;

  std::printf("test_network_state\n");

  check(std::strcmp(network_state_to_string(NetworkState::ONLINE), "ONLINE") == 0, "ONLINE string");
  check(std::strcmp(network_state_to_string(NetworkState::DEGRADED), "DEGRADED") == 0, "DEGRADED string");
  check(std::strcmp(network_state_to_string(NetworkState::OFFLINE_WIFI), "OFFLINE_WIFI") == 0,
        "OFFLINE_WIFI string");
  check(std::strcmp(network_state_to_string(NetworkState::OFFLINE_SERVER), "OFFLINE_SERVER") == 0,
        "OFFLINE_SERVER string");
  check(std::strcmp(network_state_to_string(NetworkState::AUTH_BLOCKED), "AUTH_BLOCKED") == 0,
        "AUTH_BLOCKED string");

  // --- Priority 1: WiFi disconnected always wins, regardless of anything else ---
  {
    NetworkStateInputs in;
    in.wifi_connected = false;
    in.last_request_ok = true;
    in.last_http_status = 200;
    check(classify_network_state(in) == NetworkState::OFFLINE_WIFI,
          "WiFi disconnected -> OFFLINE_WIFI even if last request looked fine");
  }
  {
    NetworkStateInputs in;
    in.wifi_connected = false;
    in.last_http_status = 401;  // even an auth failure doesn't outrank a real WiFi outage
    check(classify_network_state(in) == NetworkState::OFFLINE_WIFI,
          "WiFi disconnected outranks a stale 401/403 from before the drop");
  }

  // --- Priority 2: 401/403 -> AUTH_BLOCKED (WiFi connected) ---
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = false;
    in.last_http_status = 401;
    check(classify_network_state(in) == NetworkState::AUTH_BLOCKED, "401 -> AUTH_BLOCKED");
  }
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = false;
    in.last_http_status = 403;
    check(classify_network_state(in) == NetworkState::AUTH_BLOCKED, "403 -> AUTH_BLOCKED");
  }

  // --- Priority 3/4: WiFi connected + request failing -- streak decides DEGRADED vs OFFLINE_SERVER ---
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = false;
    in.last_http_status = 0;  // e.g. a timeout, no response at all
    in.consecutive_failures = 1;
    check(classify_network_state(in) == NetworkState::DEGRADED,
          "WiFi connected + server timeout, single failure -> DEGRADED (intermittent, still usable)");
  }
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = false;
    in.last_http_status = 500;
    in.consecutive_failures = kOfflineServerStreakThreshold;
    check(classify_network_state(in) == NetworkState::OFFLINE_SERVER,
          "WiFi connected + sustained 5xx streak -> OFFLINE_SERVER");
  }
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = false;
    in.consecutive_failures = kOfflineServerStreakThreshold - 1;
    check(classify_network_state(in) == NetworkState::DEGRADED,
          "one failure short of the threshold -> still DEGRADED, not yet OFFLINE_SERVER");
  }
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = false;
    in.consecutive_failures = kOfflineServerStreakThreshold + 5;
    check(classify_network_state(in) == NetworkState::OFFLINE_SERVER,
          "well past the threshold -> stays OFFLINE_SERVER");
  }

  // --- Priority 5: last request ok (or none yet, the default) -> ONLINE ---
  {
    NetworkStateInputs in;  // defaults: wifi_connected=true, last_request_ok=true
    check(classify_network_state(in) == NetworkState::ONLINE,
          "fresh boot, no request yet -- defaults classify as ONLINE");
  }
  {
    NetworkStateInputs in;
    in.wifi_connected = true;
    in.last_request_ok = true;
    in.consecutive_failures = 0;
    in.last_http_status = 200;
    check(classify_network_state(in) == NetworkState::ONLINE, "last request ok -> ONLINE");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
