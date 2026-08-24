#include "protocol_codec.h"

#include <cstdio>

namespace kiosk::protocol {

std::string json_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (unsigned char c : input) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string encode_event_json(const KioskEvent& event) {
  char num_buf[256];
  std::string out;
  out.reserve(640);

  out += "{\"protocol_version\":";
  std::snprintf(num_buf, sizeof(num_buf), "%u", event.protocol_version);
  out += num_buf;

  // device{}
  out += ",\"device\":{";
  out += "\"device_id\":\"" + json_escape(event.device.device_id) + "\"";
  out += ",\"hardware_id\":\"" + json_escape(event.device.hardware_id) + "\"";
  out += ",\"boot_id\":\"" + json_escape(event.device.boot_id) + "\"";
  out += "}";

  // event{}
  out += ",\"event\":{";
  out += "\"event_id\":\"" + json_escape(event.event.event_id) + "\"";
  out += ",\"device_seq\":";
  std::snprintf(num_buf, sizeof(num_buf), "%llu",
                static_cast<unsigned long long>(event.event.device_seq));
  out += num_buf;
  out += ",\"type\":\"";
  out += event_type_to_string(event.event.type);
  out += "\"}";

  // time{} -- §17: never fabricate a timestamp when unsynced.
  out += ",\"time\":{";
  if (event.time.sync_status == TimeSyncStatus::SYNCED ||
      event.time.sync_status == TimeSyncStatus::STALE) {
    out += "\"timestamp_device\":\"" + json_escape(event.time.timestamp_device_iso) + "\"";
  } else {
    out += "\"timestamp_device\":null";
  }
  out += ",\"uptime_ms\":";
  std::snprintf(num_buf, sizeof(num_buf), "%llu",
                static_cast<unsigned long long>(event.time.uptime_ms));
  out += num_buf;
  out += ",\"sync_status\":\"";
  out += time_sync_status_to_string(event.time.sync_status);
  out += "\"";
  out += ",\"sync_age_s\":";
  std::snprintf(num_buf, sizeof(num_buf), "%u", event.time.sync_age_s);
  out += num_buf;
  out += "}";

  // context{}
  out += ",\"context\":{";
  out += "\"expected_state_version\":";
  std::snprintf(num_buf, sizeof(num_buf), "%llu",
                static_cast<unsigned long long>(event.context.expected_state_version));
  out += num_buf;
  out += ",\"workflow_version\":";
  std::snprintf(num_buf, sizeof(num_buf), "%u", event.context.workflow_version);
  out += num_buf;
  out += ",\"ui_bundle_version\":";
  std::snprintf(num_buf, sizeof(num_buf), "%u", event.context.ui_bundle_version);
  out += num_buf;
  out += "}";

  // payload{} -- Phase 1 only ever carries a SCAN payload.
  out += ",\"payload\":{";
  out += "\"source\":\"" + json_escape(event.payload.source) + "\"";
  out += ",\"raw\":\"" + json_escape(event.payload.raw) + "\"";
  // KIOSK-006: quantity_good must be either a real number or fully absent —
  // never null-as-a-stand-in-for-both, and never coalesced with 0's absence.
  if (event.quantity_good.present) {
    out += ",\"quantity_good\":";
    std::snprintf(num_buf, sizeof(num_buf), "%d", event.quantity_good.value);
    out += num_buf;
  }
  // Same present/absent discipline as quantity_good above (KIOSK-006) --
  // GOOD/DEFECT/REWORK quantity flow task.
  if (event.quantity_defect.present) {
    out += ",\"quantity_defect\":";
    std::snprintf(num_buf, sizeof(num_buf), "%d", event.quantity_defect.value);
    out += num_buf;
  }
  if (event.quantity_rework.present) {
    out += ",\"quantity_rework\":";
    std::snprintf(num_buf, sizeof(num_buf), "%d", event.quantity_rework.value);
    out += num_buf;
  }
  out += "}";

  out += "}";
  return out;
}

}  // namespace kiosk::protocol
