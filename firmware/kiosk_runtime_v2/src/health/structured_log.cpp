#include "structured_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

#include "../protocol/protocol_codec.h"  // reuse json_escape, avoid a second escaper

namespace kiosk::health {

namespace {
// A real bug found on real hardware this session (docs/RETRY_POLICY.md
// "Known gap"): the background send task (api_client.cpp) and the main
// loopTask both call this function, and multiple separate Serial.print()
// calls per line are not atomic against each other across tasks -- their
// output can interleave mid-line. Fixed by building the whole line in one
// buffer and making exactly one Serial.print() call, guarded by a mutex so
// two concurrent callers can't even interleave THAT single call's
// underlying byte writes.
SemaphoreHandle_t log_mutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}
}  // namespace

void log_structured(const char* level, const char* code, const char* module,
                     const char* message) {
  std::string line = "{\"level\":\"";
  line += level;
  line += "\",\"code\":\"";
  line += code;
  line += "\",\"module\":\"";
  line += module;
  line += "\",\"message\":\"";
  line += kiosk::protocol::json_escape(message);
  line += "\",\"uptime_ms\":";
  line += std::to_string(millis());
  line += "}\n";

  xSemaphoreTake(log_mutex(), portMAX_DELAY);
  Serial.print(line.c_str());
  xSemaphoreGive(log_mutex());
}

}  // namespace kiosk::health
