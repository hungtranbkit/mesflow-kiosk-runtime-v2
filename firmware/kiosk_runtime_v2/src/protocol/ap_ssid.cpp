#include "ap_ssid.h"

#include <cctype>

namespace kiosk::protocol {

std::string compute_setup_ap_ssid(const std::string& identity_name) {
  // Strip punctuation first (device_id values like "KIOSK-LASER-01" are
  // dash-heavy) so the suffix reads as a clean 4-character code rather than
  // e.g. "R-01" (which would make the full SSID "MesflowKiosk-R-01" --
  // technically valid but confusing with two dashes back to back).
  std::string alnum;
  alnum.reserve(identity_name.length());
  for (char c : identity_name) {
    if (std::isalnum(static_cast<unsigned char>(c))) alnum += c;
  }

  std::string suffix;
  if (alnum.length() >= 4) {
    suffix = alnum.substr(alnum.length() - 4);
  } else {
    // Left-pad with '0' -- never seen in practice (device_id/hardware_id
    // are always longer), but the format must stay exactly 4 characters
    // regardless.
    suffix = std::string(4 - alnum.length(), '0') + alnum;
  }
  for (char& c : suffix) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return "MesflowKiosk-" + suffix;
}

}  // namespace kiosk::protocol
