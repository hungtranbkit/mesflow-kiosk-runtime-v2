#include "status_snapshot.h"

#include <WiFi.h>

#include "../config/build_info.h"
#include "../health/recovery_supervisor.h"
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

  // §2/§3/§4 (2026-08-26 UX-hardening pass): server identity/environment,
  // for the exact real-hardware confirmation loop found necessary the
  // first time this shipped -- there was no way to see WHY a device
  // reported UNKNOWN (backend not yet redeployed with the environment
  // fields vs. a real parsing bug) without this in /debug/device-state.
  json += "\"server\":{";
  json += std::string("\"environment\":\"") +
          kiosk::protocol::environment_to_string(runtime.server_environment()) + "\",";
  json += "\"version\":\"" + kiosk::protocol::json_escape(runtime.server_version().c_str()) + "\",";
  json += "\"expected_environment\":\"" +
          kiosk::protocol::json_escape(runtime.configured_expected_environment().c_str()) + "\",";
  json += std::string("\"mismatch\":") + (runtime.env_mismatch() ? "true" : "false") + ",";
  json += "\"last_sync\":" +
          (runtime.last_sync_iso().length() > 0
               ? ("\"" + kiosk::protocol::json_escape(runtime.last_sync_iso().c_str()) + "\"")
               : std::string("null"));
  json += ",\"offline_queue\":" + std::to_string(runtime.offline_queue_size());
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
  json += "},";

  // Phase 3A durable journal (shadow mode) -- §12 of the task. No
  // event_id/payload contents here by design ("No secrets/payload contents
  // needed by default").
  {
    const auto& j = runtime.journal();
    auto counts = j.counts();
    uint32_t now_ms = static_cast<uint32_t>(millis());
    uint32_t oldest_age = j.oldest_pending_age_ms(now_ms);
    json += "\"journal\":{";
    json += std::string("\"init_ok\":") + (j.init_ok() ? "true" : "false") + ",";
    json += "\"capacity_bytes\":" + std::to_string(j.capacity_bytes()) + ",";
    json += "\"used_bytes\":" + std::to_string(j.used_bytes()) + ",";
    json += "\"free_bytes\":" + std::to_string(j.capacity_bytes() > j.used_bytes()
                                                    ? j.capacity_bytes() - j.used_bytes() : 0) + ",";
    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), "%.2f", j.usage_pct());
    json += std::string("\"usage_pct\":") + pct_buf + ",";
    const char* pressure_str = "NORMAL";
    switch (j.pressure()) {
      case kiosk::protocol::JournalPressure::NORMAL: pressure_str = "NORMAL"; break;
      case kiosk::protocol::JournalPressure::WARNING: pressure_str = "WARNING"; break;
      case kiosk::protocol::JournalPressure::RESTRICTED: pressure_str = "RESTRICTED"; break;
      case kiosk::protocol::JournalPressure::FULL: pressure_str = "FULL"; break;
    }
    json += std::string("\"pressure\":\"") + pressure_str + "\",";
    json += "\"records\":" + std::to_string(j.record_count()) + ",";
    json += "\"pending\":" + std::to_string(counts.pending) + ",";
    json += "\"inflight\":" + std::to_string(counts.in_flight) + ",";
    json += "\"acked\":" + std::to_string(counts.acked) + ",";
    json += "\"rejected\":" + std::to_string(counts.rejected) + ",";
    json += "\"conflicts\":" + std::to_string(counts.conflict) + ",";
    json += "\"human_review\":" + std::to_string(counts.human_review) + ",";
    if (oldest_age == 0xFFFFFFFFu) {
      json += "\"oldest_pending_age_s\":null";
    } else {
      json += "\"oldest_pending_age_s\":" + std::to_string(oldest_age / 1000);
    }
    // §1 of the 2026-08-25 follow-up: incremental compaction progress, so
    // QA tooling/an operator can see a multi-tick compaction actually
    // making progress rather than just a silent pause.
    json += std::string(",\"compaction_active\":") + (j.compaction_active() ? "true" : "false") + ",";
    json += "\"compaction_records_processed\":" + std::to_string(j.compaction_records_processed()) + ",";
    json += "\"compaction_records_total\":" + std::to_string(j.compaction_records_total()) + ",";
    json += "\"compaction_last_progress_ms\":" + std::to_string(j.compaction_last_progress_ms());
    json += "},";
  }

  // §22/§23 of the self-recovery task -- structured recovery codes + the
  // bounded recovery history ring, plus reboot-loop protection's current
  // SAFE_MODE flag. Never any business/payload data here, same "coarse
  // operational status only" rule the rest of this function already
  // follows.
  {
    json += "\"recovery\":{";
    json += std::string("\"safe_mode\":") + (kiosk::health::is_safe_mode() ? "true" : "false") + ",";
    json += "\"same_fault_streak\":" + std::to_string(kiosk::health::same_fault_streak()) + ",";
    json += "\"history\":[";
    int count = kiosk::health::recovery_history_count();
    for (int i = 0; i < count; ++i) {
      if (i > 0) json += ",";
      const auto& e = kiosk::health::recovery_history_at(i);
      json += "{";
      json += std::string("\"code\":\"") + kiosk::health::recovery_code_to_string(e.code) + "\",";
      json += "\"detail\":\"" + kiosk::protocol::json_escape(e.detail) + "\",";
      json += "\"uptime_ms\":" + std::to_string(e.uptime_ms) + ",";
      json += "\"journal_pressure\":" + std::to_string(e.journal_pressure) + ",";
      json += "\"memory_free_bytes\":" + std::to_string(e.memory_free_bytes) + ",";
      json += "\"memory_largest_block_bytes\":" + std::to_string(e.memory_largest_block_bytes);
      json += "}";
    }
    json += "]}";
  }

  json += "}";
  return json;
}

}  // namespace kiosk::runtime
