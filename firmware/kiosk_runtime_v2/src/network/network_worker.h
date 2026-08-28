#pragma once

// Persistent network worker (2026-08-26 "eliminate recurrent server
// connection failures" pass, §2-§5 of that task).
//
// Root cause this replaces: AsyncEventSender/AsyncStateFetcher/
// HeartbeatClient each spawned a BRAND NEW FreeRTOS task (xTaskCreate) per
// call, self-deleting via vTaskDelete(nullptr) when done. FreeRTOS can only
// actually reclaim a self-deleted task's stack/TCB once the IDLE task gets
// scheduled -- confirmed live (previous pass's own field report) as the
// most likely source of a residual, slow (~1KB/s) internal-SRAM decline
// under sustained load that a single vTaskDelay(1) yield did not fully
// eliminate. This class creates exactly ONE task, once, at begin() --
// never again for the life of the boot. Every HTTP operation (foreground
// business events, offline replay, state resync, bootstrap, heartbeat) now
// flows through this one task and its own local, stack-scoped HTTPClient,
// matching this task's own §5 "one HTTP transaction scope" -- nothing
// survives from one request into the next.
//
// Serialization (§3): the worker processes exactly one request at a time,
// by construction (a single task pulling from queues one item at a time).
// Two priority tiers (§3's 5-level list collapsed to what actually matters
// in practice -- see kiosk_runtime.cpp's own enqueue call sites for which
// kind maps to which tier): HIGH (foreground business events, state fetch,
// bootstrap) is always drained completely before LOW (offline replay,
// heartbeat) is touched at all.
//
// Queue contents (§4): only small, FIXED-SIZE, POD request descriptors are
// queued -- no String/std::string member, no dynamic allocation in the
// queue slot itself.
//
// A real cross-thread correctness issue was found and fixed while
// designing this (caught in review, before ever running): an EARLIER
// version of this design had the WORKER look up a business event's payload
// directly from EventJournal by event_id at send time, mirroring the
// offline-replay queue's own "don't duplicate the payload" pattern
// (kiosk_runtime.h's replay_event_ids_). That pattern is safe there because
// the LOOKUP happens on the MAIN thread. Here, the worker task runs on ITS
// OWN thread -- EventJournalIndex::records_ (a std::map) is not
// synchronized for concurrent access, and the main thread mutates it
// constantly (new appends, transitions, compaction). Instead, the payload
// is copied into this SAME fixed body[] buffer BOOTSTRAP/HEARTBEAT already
// use, on the ENQUEUING side (the main thread, where journal access is
// already safe) -- the worker never touches EventJournal at all. A real
// business-event body measured well under the 2560-byte buffer size (see
// kMaxQueuedBodyBytes' own comment), so this costs nothing extra in
// practice; it just closes a real thread-safety gap the alternative design
// would have had.
#include <Arduino.h>

#include <cstdint>
#include <string>

#include "../protocol/retry_policy.h"

namespace kiosk::network {

enum class NetworkRequestKind : uint8_t {
  BUSINESS_EVENT,   // POST to the events URL; payload supplied inline (copied into the request by the caller)
  OFFLINE_REPLAY,   // same as BUSINESS_EVENT (same URL, same retry policy, same inline-payload
                    // shape) -- kept as its own enum value only so results/priority can be told
                    // apart (HIGH vs LOW tier) without a separate flag.
  STATE_FETCH,      // GET the state URL; no body -- HIGH tier (STATE_CONFLICT resync, foreground)
  BOOTSTRAP,        // POST the bootstrap URL; body supplied inline (small, fixed, built by the caller)
  HEARTBEAT,        // POST the heartbeat URL; body supplied inline (small, fixed, built by the caller)
  // 2026-08-27 "close final two runtime gaps" pass: last remaining per-call
  // xTaskCreate site (state_client.cpp's AsyncStateFetcher, used only by
  // UiSyncController for the UI-bundle download) migrated here. Deliberately
  // its OWN kind, not reusing STATE_FETCH, for two reasons: (1) priority --
  // a UI bundle check/download is background housekeeping, LOW tier, and
  // must never compete with a real STATE_CONFLICT resync (HIGH) for the
  // worker's single execution slot; (2) result routing -- KioskRuntime::poll()
  // dispatches by `kind` alone (one shared result slot, see this header's
  // NetworkResult comment), so STATE_FETCH and UI_BUNDLE_FETCH results must
  // be tellable apart even though both are bodyless GETs under the hood.
  UI_BUNDLE_FETCH,
};

// §3: PRIORITY_HIGH is always drained before PRIORITY_LOW is touched at
// all. Not named plain HIGH/LOW -- Arduino.h #define's those as digitalWrite
// pin-level macros (HIGH=0x1, LOW=0x0), which silently mangles an
// `enum class` member of that name at the token level (confirmed live: a
// real build failure, not a hypothetical -- "expected identifier before
// numeric constant" pointing at this exact line, before this rename).
enum class NetworkPriorityTier : uint8_t { PRIORITY_HIGH, PRIORITY_LOW };

constexpr size_t kMaxQueuedUrlBytes = 160;
// Sized against status_snapshot.cpp's actual build_status_json() output
// (the heartbeat body, and structurally close to bootstrap's own body) --
// a real captured /debug/device-state dump (a superset of the heartbeat
// body, adding only the recovery-history block) measured 2595 bytes.
// 2560 leaves reasonable headroom without being an unbounded guess. Only 2
// of the 5 request kinds (HEARTBEAT/BOOTSTRAP) ever populate this field --
// BUSINESS_EVENT/OFFLINE_REPLAY/STATE_FETCH leave it empty (0 bytes used,
// looked up from the journal or not needed at all) -- but every queue slot
// is this same fixed size regardless of kind, by design (§4: "use bounded
// structs/fixed storage"). Queue depths are kept small (see .cpp) so the
// total static cost stays bounded and small in absolute terms.
constexpr size_t kMaxQueuedBodyBytes = 2560;
constexpr size_t kMaxQueuedEventIdBytes = 40;  // event_ids are 32 hex chars + margin
// secrets.token_urlsafe(32) (the backend's actual generator, see
// KioskRepository.approve()/bind_legacy() in mesflow's execution.py) yields
// ~43 base64url characters -- 64 leaves real margin without being an
// unbounded guess, matching this struct's own "bounded, fixed" convention.
constexpr size_t kMaxQueuedTokenBytes = 64;

// Fixed-size, POD (no String/std::string members) -- safe to memcpy into a
// FreeRTOS queue slot. §4: "do not put full JSON payloads or large Strings
// into the queue" -- "large" is the operative word: this is a small, fixed,
// BOUNDED size (2560 bytes, see kMaxQueuedBodyBytes), not the unbounded/
// duplicated-per-pending-item pattern this whole task set out to eliminate.
struct NetworkRequest {
  NetworkRequestKind kind = NetworkRequestKind::BUSINESS_EVENT;
  char url[kMaxQueuedUrlBytes] = {0};
  char event_id[kMaxQueuedEventIdBytes] = {0};  // BUSINESS_EVENT/OFFLINE_REPLAY only (for result correlation)
  uint64_t device_seq = 0;                      // BUSINESS_EVENT/OFFLINE_REPLAY only
  char body[kMaxQueuedBodyBytes] = {0};         // POST body for all kinds except STATE_FETCH (a GET)
  uint16_t body_len = 0;
  // BUSINESS_EVENT/OFFLINE_REPLAY/STATE_FETCH only (2026-08-28 P0 auth fix)
  // -- sent as X-Kiosk-Token. Copied in on the calling thread, same
  // thread-safety reasoning as `body`/`url` above (this struct's own top
  // comment). Empty for BOOTSTRAP/HEARTBEAT/UI_BUNDLE_FETCH, which never
  // attach this header -- see DeviceIdentity::kiosk_token()'s own comment
  // for why.
  char kiosk_token[kMaxQueuedTokenBytes] = {0};
};

// Result of ONE completed request. This is NOT queued (only one request is
// ever in flight at a time by construction) -- a single mutex-protected
// slot, the same shape AsyncEventSender/AsyncStateFetcher/HeartbeatClient
// already used individually before this class unified them. response_body
// is a std::string here (not a fixed buffer) because it's genuinely
// variable-length up to RUNTIME_MAX_RESPONSE_BODY_BYTES and there is only ever
// ONE such buffer alive at a time (overwritten each result, never
// accumulated) -- structurally identical to the pre-existing SendOutcome/
// StateFetchOutcome contract this replaces, so every existing caller's
// parsing code (event_response.h, state_projection.h) is untouched.
struct NetworkResult {
  NetworkRequestKind kind = NetworkRequestKind::BUSINESS_EVENT;
  kiosk::protocol::HttpOutcome outcome;
  uint32_t total_latency_ms = 0;
  int attempts = 0;
  std::string event_id;
  uint64_t device_seq = 0;
  std::string response_body;
};

// One persistent task, created once by begin(). Never destroyed/recreated
// for the life of the boot.
class NetworkWorker {
 public:
  NetworkWorker() = default;

  // Creates the queues + the one persistent FreeRTOS task. Call exactly
  // once, from setup().
  void begin();

  // Enqueue calls -- all non-blocking, return false if the relevant queue
  // is full (§4: "queue full behavior must be explicit"). The caller's
  // business event/state/etc is ALREADY durably journaled (for
  // BUSINESS_EVENT/OFFLINE_REPLAY) before this is ever called -- a false
  // return here means "not dispatched yet", never "lost": the periodic
  // offline-replay fallback (kiosk_runtime.cpp) or the next foreground
  // interaction will pick it up again later. `payload`/`body` are copied
  // into the request's fixed buffer here, on the CALLING thread -- see
  // this header's own top comment for why (thread safety, not a style
  // choice).
  // `kiosk_token` (2026-08-28 P0 auth fix): default "" preserves source
  // compatibility for any caller that predates this change, but in
  // practice every real caller now passes DeviceIdentity::kiosk_token() --
  // an empty token here just means the server will correctly 401 the
  // request, not a silent bypass.
  bool enqueue_business_event(const std::string& event_id, uint64_t device_seq, const String& url,
                              const std::string& payload, const String& kiosk_token = "");
  bool enqueue_offline_replay(const std::string& event_id, uint64_t device_seq, const String& url,
                             const std::string& payload, const String& kiosk_token = "");
  bool enqueue_state_fetch(const String& url, const String& kiosk_token = "");
  // LOW tier -- see NetworkRequestKind::UI_BUNDLE_FETCH's own comment for why
  // this is a distinct kind/priority from enqueue_state_fetch() above.
  bool enqueue_ui_bundle_fetch(const String& url);
  bool enqueue_bootstrap(const String& url, const std::string& body);
  bool enqueue_heartbeat(const String& url, const std::string& body);

  // True if the worker is currently executing a HIGH-priority request OR
  // one is queued -- used by callers that need to know "would enqueuing a
  // LOW-priority request have to wait" (offline replay/heartbeat's own
  // foreground-priority checks, §3's "if foreground traffic exists,
  // heartbeat/replay wait").
  bool high_priority_busy() const;

  // True if EITHER queue is non-empty or a request is currently executing
  // -- "is the network subsystem doing anything at all right now".
  bool busy() const;

  // Call every loop() iteration. Returns true (and fills `out`) exactly
  // once per completed request. Never blocks.
  bool poll(NetworkResult& out);

 private:
  void* high_queue_ = nullptr;  // QueueHandle_t, opaque here to avoid pulling FreeRTOS headers into this .h
  void* low_queue_ = nullptr;
  void* result_mutex_ = nullptr;  // SemaphoreHandle_t
  volatile bool executing_ = false;
  bool result_ready_ = false;
  NetworkResult pending_result_;

  bool enqueue(NetworkPriorityTier tier, const NetworkRequest& req);
  void deposit_result(const NetworkResult& result);
  NetworkResult execute(const NetworkRequest& req);

  static void task_entry(void* arg);
};

}  // namespace kiosk::network
