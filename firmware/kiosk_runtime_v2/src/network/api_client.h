#pragma once

#include <Arduino.h>

#include <string>

#include "../config/runtime_config.h"  // MESFLOW_DEBUG_API
#include "../protocol/retry_policy.h"

namespace kiosk::network {

// Result of a completed send (possibly after several retries). Deliberately
// NOT a bare bool/int (§57 of the original task spec, §25 of Phase 1):
// carries the full classified outcome plus enough to correlate back to
// which event this was.
struct SendOutcome {
  kiosk::protocol::HttpOutcome outcome;
  uint32_t total_latency_ms = 0;
  int attempts = 0;
  std::string event_id;
  uint64_t device_seq = 0;
  // Body of the LAST attempt's response, whatever its HTTP status -- Phase 2
  // needs this to parse the /events response shape (accepted/state/error/
  // action/current_state_version, see event_response.h). Every business
  // outcome (accept/reject/conflict) is HTTP 200 per §83, so `outcome.ok`
  // alone can't distinguish them; the caller must parse this body.
  std::string response_body;
};

// §23: retries must never block display/keypad/scanner. AsyncEventSender
// runs the whole retry loop (classify -> backoff+jitter -> retry, per
// docs/RETRY_POLICY.md) on its own FreeRTOS task. The caller's loop()
// polls for completion instead of blocking on a semaphore -- this is a
// deliberate change from Phase 0's synchronous-with-hard-deadline
// api_client, which DID block the caller for up to ~8s per scan.
//
// Single-in-flight by design: while one send is running, calling send()
// again is refused (returns false) rather than silently queued -- there is
// no durable journal yet (Phase 3), so queuing here would be pretending to
// have reliability this codebase doesn't have. The caller must decide what
// "busy" means for the operator (Phase 1: show an honest "previous event
// still sending" message rather than dropping silently).
//
// Thread safety: the shared result slot is guarded by a small FreeRTOS
// mutex, held only for the brief copy in/out -- poll() and send() are both
// meant to be called from the main loop() task only; the retry loop itself
// runs on the spawned task and never touches the EventBus or Display
// directly (see kiosk_runtime.cpp, which is what actually publishes the
// result on the main task once poll() reports completion -- keeping the
// "only loopTask touches Display" invariant from docs/VISUAL_DEBUG.md intact).
class AsyncEventSender {
 public:
  AsyncEventSender();

  // Starts sending in the background. Returns false (does NOT start a new
  // send) if a previous send is still in flight.
  bool send(const String& url, const String& json_body, const std::string& event_id,
            uint64_t device_seq);

  bool busy() const;

#if MESFLOW_DEBUG_API
  // DEV-only fault injection (2026-08-25, §8 of the finish-anti-stuck-
  // recovery follow-up): makes the NEXT send() call skip xTaskCreate()
  // entirely and fail exactly the way a genuine API_ERR_TASK_CREATE_FAILED
  // does, without needing to actually exhaust real memory to prove the
  // recovery path works. One-shot -- cleared as soon as it's consumed.
  void force_next_task_create_failure() { force_next_task_create_failure_ = true; }
#endif

  // Call every loop() iteration. Returns true (and fills `out`) exactly
  // once per completed send. Never blocks.
  bool poll(SendOutcome& out);

  // INTERNAL: called only by the background send task (api_client.cpp) to
  // hand back its result. Not part of the public contract for normal
  // callers -- public only because a free function (the FreeRTOS task
  // entry point) needs to call it, and a friend declaration would have to
  // cross an anonymous-namespace boundary awkwardly.
  void internal_deposit_result(const SendOutcome& result);

 private:
  void* mutex_;  // SemaphoreHandle_t, opaque here to avoid pulling FreeRTOS headers into this .h
  bool busy_ = false;
  bool result_ready_ = false;
  SendOutcome pending_result_;
#if MESFLOW_DEBUG_API
  bool force_next_task_create_failure_ = false;
#endif
};

}  // namespace kiosk::network
