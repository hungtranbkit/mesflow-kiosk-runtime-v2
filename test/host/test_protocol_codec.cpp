// Host test: plain C++, no Arduino, no ESP toolchain. Build/run via
// scripts/run_host_tests.sh. Exercises the protocol codec directly against
// docs/PROTOCOL.md's documented Envelope v1 wire format.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../firmware/kiosk_runtime_v2/src/protocol/crc32.h"
#include "../../firmware/kiosk_runtime_v2/src/protocol/event_types.h"
#include "../../firmware/kiosk_runtime_v2/src/protocol/protocol_codec.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
  if (condition) {
    std::printf("  PASS: %s\n", description);
  } else {
    std::printf("  FAIL: %s\n", description);
    ++g_failures;
  }
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  using namespace kiosk::protocol;

  std::printf("test_protocol_codec\n");

  // --- Envelope v1 shape (nested device/event/time/context/payload) ---
  {
    KioskEvent event;
    event.device.device_id = "KIOSK-DEV-0001";
    event.device.hardware_id = "esp32s3-abcdef012345";
    event.device.boot_id = "abc123";
    event.event.event_id = "e-001";
    event.event.device_seq = 42;
    event.event.type = EventType::SCAN;
    event.time.timestamp_device_iso = "2026-08-23T08:00:00Z";
    event.time.uptime_ms = 1000;
    event.time.sync_status = TimeSyncStatus::SYNCED;
    event.time.sync_age_s = 5;
    event.payload.source = "GM65";
    event.payload.raw = "WF|EMP|00152";

    std::string json = encode_event_json(event);

    check(contains(json, "\"protocol_version\":1"), "protocol_version present");
    check(contains(json, "\"device\":{"), "device{} nested object present");
    check(contains(json, "\"device_id\":\"KIOSK-DEV-0001\""), "device.device_id present");
    check(contains(json, "\"hardware_id\":\"esp32s3-abcdef012345\""), "device.hardware_id present");
    check(contains(json, "\"boot_id\":\"abc123\""), "device.boot_id present");
    check(contains(json, "\"event\":{"), "event{} nested object present");
    check(contains(json, "\"event_id\":\"e-001\""), "event.event_id present");
    check(contains(json, "\"device_seq\":42"), "event.device_seq present");
    check(contains(json, "\"type\":\"SCAN\""), "event.type present");
    check(contains(json, "\"time\":{"), "time{} nested object present");
    check(contains(json, "\"timestamp_device\":\"2026-08-23T08:00:00Z\""),
          "time.timestamp_device present when SYNCED");
    check(contains(json, "\"sync_status\":\"SYNCED\""), "time.sync_status present");
    check(contains(json, "\"sync_age_s\":5"), "time.sync_age_s present");
    check(contains(json, "\"context\":{"), "context{} nested object present");
    check(contains(json, "\"expected_state_version\":0"), "context.expected_state_version present (§7)");
    check(contains(json, "\"raw\":\"WF|EMP|00152\""), "payload.raw present");
    check(!contains(json, "quantity_good"),
          "quantity_good absent when not set (no field, not a stray null)");
  }

  // --- §17: UNSYNCED time must never fabricate a timestamp ---
  {
    KioskEvent event;
    event.payload.source = "GM65";
    event.payload.raw = "X";
    event.time.sync_status = TimeSyncStatus::UNSYNCED;
    event.time.uptime_ms = 102233;
    event.time.timestamp_device_iso = "";  // never set when unsynced

    std::string json = encode_event_json(event);
    check(contains(json, "\"timestamp_device\":null"),
          "UNSYNCED -> timestamp_device is JSON null, not a fabricated epoch string");
    check(!contains(json, "1970"), "no 1970-01-01 epoch placeholder anywhere in the encoded event");
    check(contains(json, "\"sync_status\":\"UNSYNCED\""), "sync_status correctly UNSYNCED");
    check(contains(json, "\"uptime_ms\":102233"),
          "uptime_ms still serializes fine even though time is unsynced (event remains valid, KIOSK-070)");
  }

  // --- KIOSK-006: quantity_good = 0 must differ from absent ---
  {
    KioskEvent with_zero;
    with_zero.payload.source = "GM65";
    with_zero.payload.raw = "Q";
    with_zero.quantity_good.present = true;
    with_zero.quantity_good.value = 0;
    std::string json_zero = encode_event_json(with_zero);

    KioskEvent without;
    without.payload.source = "GM65";
    without.payload.raw = "Q";
    std::string json_absent = encode_event_json(without);

    check(contains(json_zero, "\"quantity_good\":0"),
          "quantity_good=0 encodes as an explicit 0");
    check(!contains(json_absent, "quantity_good"),
          "quantity_good absent encodes with no field at all");
    check(json_zero != json_absent, "0-case and absent-case are distinguishable strings");
  }

  // --- GOOD/DEFECT/REWORK quantity flow: defect/rework same KIOSK-006 discipline ---
  {
    KioskEvent full;
    full.payload.source = "KEYPAD";
    full.payload.raw = "";
    full.quantity_good.present = true;
    full.quantity_good.value = 35;
    full.quantity_defect.present = true;
    full.quantity_defect.value = 4;
    full.quantity_rework.present = true;
    full.quantity_rework.value = 3;
    std::string json_full = encode_event_json(full);

    check(contains(json_full, "\"quantity_good\":35"), "quantity_good present and correct");
    check(contains(json_full, "\"quantity_defect\":4"), "quantity_defect present and correct");
    check(contains(json_full, "\"quantity_rework\":3"), "quantity_rework present and correct");

    KioskEvent good_only;
    good_only.payload.source = "KEYPAD";
    good_only.payload.raw = "";
    good_only.quantity_good.present = true;
    good_only.quantity_good.value = 20;
    std::string json_good_only = encode_event_json(good_only);
    check(!contains(json_good_only, "quantity_defect"),
          "quantity_defect absent when not set (older-style single-quantity submit)");
    check(!contains(json_good_only, "quantity_rework"),
          "quantity_rework absent when not set");

    KioskEvent defect_zero;
    defect_zero.payload.source = "KEYPAD";
    defect_zero.payload.raw = "";
    defect_zero.quantity_good.present = true;
    defect_zero.quantity_good.value = 20;
    defect_zero.quantity_defect.present = true;
    defect_zero.quantity_defect.value = 0;
    defect_zero.quantity_rework.present = true;
    defect_zero.quantity_rework.value = 0;
    std::string json_defect_zero = encode_event_json(defect_zero);
    check(contains(json_defect_zero, "\"quantity_defect\":0"),
          "quantity_defect=0 (DEFECT==0 finish-immediately path) encodes as explicit 0, not absent");
    check(contains(json_defect_zero, "\"quantity_rework\":0"),
          "quantity_rework=0 (not repairable / no defect) encodes as explicit 0, not absent");
  }

  // --- JSON escaping ---
  {
    std::string escaped = json_escape("line1\nline2\"quoted\"\\backslash");
    check(contains(escaped, "\\n"), "newline escaped");
    check(contains(escaped, "\\\""), "quote escaped");
    check(contains(escaped, "\\\\"), "backslash escaped");
  }

  // --- CRC32 sanity (groundwork for Phase 3 journal records) ---
  {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint32_t crc = crc32(data, sizeof(data));
    // Well-known CRC-32/ISO-HDLC check value for ASCII "123456789".
    check(crc == 0xCBF43926u, "CRC32(\"123456789\") matches known-good check value");

    uint32_t crc_empty = crc32(nullptr, 0);
    check(crc_empty == 0x00000000u, "CRC32 of empty buffer is 0");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
