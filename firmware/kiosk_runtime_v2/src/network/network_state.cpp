#include "network_state.h"

namespace kiosk::network {

const char* network_state_to_string(NetworkState state) {
  switch (state) {
    case NetworkState::ONLINE: return "ONLINE";
    case NetworkState::DEGRADED: return "DEGRADED";
    case NetworkState::OFFLINE_WIFI: return "OFFLINE_WIFI";
    case NetworkState::OFFLINE_SERVER: return "OFFLINE_SERVER";
    case NetworkState::AUTH_BLOCKED: return "AUTH_BLOCKED";
  }
  return "UNKNOWN";
}

NetworkState classify_network_state(const NetworkStateInputs& in) {
  if (!in.wifi_connected) return NetworkState::OFFLINE_WIFI;
  if (in.last_http_status == 401 || in.last_http_status == 403) return NetworkState::AUTH_BLOCKED;
  if (!in.last_request_ok) {
    return in.consecutive_failures >= kOfflineServerStreakThreshold ? NetworkState::OFFLINE_SERVER
                                                                    : NetworkState::DEGRADED;
  }
  return NetworkState::ONLINE;
}

}  // namespace kiosk::network
