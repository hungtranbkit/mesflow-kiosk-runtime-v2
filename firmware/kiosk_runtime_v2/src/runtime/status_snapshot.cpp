#include "status_snapshot.h"

#include <WiFi.h>

#include "../config/build_info.h"
#include "../protocol/protocol_codec.h"  // json_escape

namespace kiosk::runtime {

std::string build_status_json(kiosk::security::DeviceIdentity& identity,
                              kiosk::network::TimeSync& time_sync, KioskRuntime& runtime,
                              kiosk::hardware::KeypadPcf8574& keypad,
                              const kiosk::hardware::SelfTestResult& selftest,
                              BootDiagnostics& diagnostics,
                              const kiosk::network::BootstrapClient& bootstrap) {
  refresh_memory_fields(diagnostics);

  bool wifi_connected = WiFi.status() == WL_CONNECTED;
  auto prov_state = identity.state();

  std::string json = "{";
  json += "\"device_id\":\"" + kiosk::protocol::json_escape(runtime.device_id().c_str()) + "\",";
  json += "\"hardware_id\":\"" + kiosk::protocol::json_escape(runtime.hardware_id().c_str()) + "\",";
  json += "\"boot_id\":\"" + kiosk::protocol::json_escape(runtime.boot_id().c_str()) + "\",";
  json += "\"provisioning_state\":\"";
  json += kiosk::security::provisioning_state_to_string(prov_state);
  json += "\",";
  json += "\"firmware_version\":\"" KIOSK_RUNTIME_VERSION "\",";
  json += std::string("\"build_id\":\"") + KIOSK_BUILD_ID + "\",";
  json += std::string("\"profile\":\"") + MESFLOW_PROFILE_NAME + "\",";
  json += "\"uptime_ms\":" + std::to_string(millis()) + ",";

  // §34: never expose Wi-Fi password/secrets here -- SSID and connection
  // quality only.
  json += "\"network\":{";
  json += std::string("\"wifi_connected\":") + (wifi_connected ? "true" : "false") + ",";
  json += "\"ssid\":\"" + kiosk::protocol::json_escape(WiFi.SSID().c_str()) + "\",";
  json += "\"rssi\":" + std::to_string(wifi_connected ? WiFi.RSSI() : 0) + ",";
  json += "\"ip\":\"" + std::string(WiFi.localIP().toString().c_str()) + "\",";
  json += std::string("\"backend_reachable\":") +
          (runtime.has_scanned() ? (runtime.last_backend_reachable() ? "true" : "false") : "null");
  json += "},";

  json += "\"memory\":{";
  json += "\"heap_free\":" + std::to_string(diagnostics.free_heap_bytes) + ",";
  json += "\"largest_heap_block\":" + std::to_string(diagnostics.largest_free_block_bytes) + ",";
  json += "\"psram_free\":" + std::to_string(diagnostics.psram_free_bytes);
  json += "},";

  json += "\"hardware\":{";
  json += std::string("\"display\":\"") + (selftest.display_ok ? "OK" : "FAIL") + "\",";
  json += std::string("\"scanner\":\"") + (selftest.scanner_ok ? "OK" : "FAIL") + "\",";
  json += std::string("\"keypad\":\"") +
          (!keypad.found() ? "NOT_FOUND" : (keypad.calibrated() ? "OK" : "UNCALIBRATED")) + "\"";
  json += "},";

  json += "\"input\":{";
  char last_key_str[2] = {runtime.last_key(), '\0'};
  json += "\"last_key\":\"" + kiosk::protocol::json_escape(last_key_str) + "\",";
  json += "\"last_scan\":" +
          (runtime.last_scan().length() > 0
               ? ("\"" + kiosk::protocol::json_escape(runtime.last_scan().c_str()) + "\"")
               : std::string("null"));
  json += "},";

  // §28/§39: protocol + time + security observability.
  json += "\"protocol\":{";
  json += "\"protocol_version\":1,";
  json += "\"device_seq\":" + std::to_string(runtime.device_seq()) + ",";
  json += "\"last_event_id\":\"" + kiosk::protocol::json_escape(runtime.last_event_id().c_str()) + "\",";
  json += "\"last_event_type\":\"" + std::string(runtime.last_event_type()) + "\",";
  json += "\"last_http_status\":" + std::to_string(runtime.last_http_status()) + ",";
  json += "\"last_latency_ms\":" + std::to_string(runtime.last_latency_ms()) + ",";
  json += "\"last_error\":" +
          (runtime.last_error_code().length() > 0
               ? ("\"" + kiosk::protocol::json_escape(runtime.last_error_code().c_str()) + "\"")
               : std::string("null"));
  json += ",\"retry_count\":" + std::to_string(runtime.last_retry_count()) + ",";
  json += "\"bootstrap\":{\"status\":\"";
  json += kiosk::network::bootstrap_status_to_string(bootstrap.last_result().status);
  json += "\",\"last_ok_uptime_ms\":" + std::to_string(bootstrap.last_result().last_ok_uptime_ms) + "}";
  json += "},";

  // Phase 2 (§44-46): the SERVER-authoritative business state this device
  // is currently rendering, per kiosk::protocol::StateProjection -- never
  // decided locally, only ever what the last-applied server snapshot said.
  json += "\"state\":{";
  if (runtime.has_state_snapshot()) {
    const auto& snap = runtime.current_state();
    json += "\"business\":\"";
    json += kiosk::protocol::business_state_to_string(snap.state);
    json += "\",";
    json += "\"state_version\":" + std::to_string(snap.state_version) + ",";
    json += "\"workflow_version\":" + std::to_string(snap.workflow_version) + ",";
    json += "\"resyncing\":" + std::string(runtime.resyncing() ? "true" : "false") + ",";
    json += "\"last_server_seq\":" + std::to_string(runtime.last_server_seq()) + ",";
    json += "\"source\":\"" + std::string(runtime.resyncing() ? "RESYNC_PENDING" : "SERVER") + "\",";
    // Sanitized view: only the same operational fields already shown on
    // screen -- no PII beyond what an operator standing at the kiosk
    // already sees.
    json += "\"view\":{";
    bool first_view = true;
    auto add_view_str = [&](const char* key, bool has, const std::string& value) {
      if (!has) return;
      if (!first_view) json += ",";
      first_view = false;
      json += std::string("\"") + key + "\":\"" + kiosk::protocol::json_escape(value) + "\"";
    };
    auto add_view_int = [&](const char* key, bool has, int32_t value) {
      if (!has) return;
      if (!first_view) json += ",";
      first_view = false;
      json += std::string("\"") + key + "\":" + std::to_string(value);
    };
    add_view_str("employee_name", snap.view.has_employee_name, snap.view.employee_name);
    add_view_str("operation_code", snap.view.has_operation_code, snap.view.operation_code);
    add_view_str("operation_name", snap.view.has_operation_name, snap.view.operation_name);
    add_view_str("session_id", snap.view.has_session_id, snap.view.session_id);
    add_view_str("started_at", snap.view.has_started_at, snap.view.started_at);
    add_view_int("target_qty", snap.view.has_target_qty, snap.view.target_qty);
    add_view_int("produced_qty", snap.view.has_produced_qty, snap.view.produced_qty);
    json += "},";
    json += "\"local_quantity_buffer\":\"" +
            kiosk::protocol::json_escape(runtime.local_quantity_buffer().c_str()) + "\"";
  } else {
    json += "\"business\":null,\"state_version\":null,\"workflow_version\":null,";
    json += "\"resyncing\":false,\"last_server_seq\":" + std::to_string(runtime.last_server_seq()) + ",";
    json += "\"source\":\"NONE_YET\",\"view\":{},\"local_quantity_buffer\":\"\"";
  }
  json += "},";

  json += "\"time\":{";
  json += "\"sync_status\":\"";
  json += kiosk::protocol::time_sync_status_to_string(time_sync.status());
  json += "\",\"sync_age_s\":" + std::to_string(time_sync.sync_age_s());
  json += "},";

  // §39: coarse status only, never private key/cert material. Phase 1 has
  // no mTLS wired into the live HTTP client yet (docs/SECURITY.md) -- report
  // that honestly rather than claiming ENABLED.
  json += "\"security\":{";
  json += std::string("\"profile\":\"") + MESFLOW_PROFILE_NAME + "\",";
  json += "\"provisioning_state\":\"";
  json += kiosk::security::provisioning_state_to_string(prov_state);
  json += "\",";
  json += "\"mtls\":\"DESIGNED_NOT_ENABLED\",";
  json += "\"server_ca_loaded\":false,";
  json += "\"client_cert_loaded\":false,";
  json += "\"certificate_status\":\"NONE\"";
  json += "}";

  json += "}";
  return json;
}

}  // namespace kiosk::runtime
