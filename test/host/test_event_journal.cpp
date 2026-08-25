// Host test: plain C++, no Arduino. Exercises the Phase 3A durable journal's
// framing (event_journal_core.h), record encode/decode (journal_record.h),
// and in-memory index/capacity logic (event_journal_index.h) against a REAL
// temp file on the host filesystem (StdFileJournalFile below) -- the exact
// same byte-level frame format storage/event_journal.cpp writes to SPIFFS
// on-device, just backed by <cstdio> instead of fs::File.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../../firmware/kiosk_runtime_v2/src/protocol/event_journal_core.h"
#include "../../firmware/kiosk_runtime_v2/src/protocol/event_journal_index.h"
#include "../../firmware/kiosk_runtime_v2/src/protocol/journal_record.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}

using namespace kiosk::protocol;

class StdFileJournalFile : public JournalFile {
 public:
  explicit StdFileJournalFile(std::string path) : path_(std::move(path)) {}
  ~StdFileJournalFile() override { close(); }

  bool open_for_append() override {
    close();
    fp_ = std::fopen(path_.c_str(), "ab");
    return fp_ != nullptr;
  }
  bool open_for_read() override {
    close();
    fp_ = std::fopen(path_.c_str(), "rb");
    not_found_ = (fp_ == nullptr);
    return true;  // a missing file scans as empty, not an error
  }
  void close() override {
    if (fp_) { std::fclose(fp_); fp_ = nullptr; }
  }
  int write(const uint8_t* data, size_t len) override {
    if (!fp_) return -1;
    return static_cast<int>(std::fwrite(data, 1, len, fp_));
  }
  bool flush() override { return fp_ && std::fflush(fp_) == 0; }
  int read(uint8_t* buf, size_t len) override {
    if (not_found_) return 0;
    if (!fp_) return -1;
    return static_cast<int>(std::fread(buf, 1, len, fp_));
  }
  uint32_t size() override {
    struct stat st;
    if (::stat(path_.c_str(), &st) != 0) return 0;
    return static_cast<uint32_t>(st.st_size);
  }

 private:
  std::string path_;
  FILE* fp_ = nullptr;
  bool not_found_ = false;
};

std::string tmp_path(const char* name) {
  return std::string("/tmp/") + name + "_" + std::to_string(::getpid()) + ".journal";
}

// Truncates the file at `path` to exactly `n` bytes -- simulates a crash
// mid-write at a precise byte offset.
void truncate_to(const std::string& path, long n) {
  FILE* f = std::fopen(path.c_str(), "r+b");
  if (!f) return;
#ifdef _WIN32
  std::fclose(f);
#else
  ::ftruncate(fileno(f), n);
  std::fclose(f);
#endif
}

JournalRecord make_record(const std::string& event_id, JournalSyncStatus status = JournalSyncStatus::PENDING) {
  JournalRecord r;
  r.record_version = 1;
  r.event_id = event_id;
  r.device_seq = 42;
  r.boot_id = "boot-abc";
  r.event_type = "QUANTITY_SUBMITTED";
  r.payload = "{\"quantity_good\":10}";
  r.payload_hash = "deadbeef";
  r.expected_state_version = 7;
  r.created_uptime_ms = 1000;
  r.time_sync_status = "SYNCED";
  r.retry_count = 0;
  r.last_attempt_uptime_ms = 0;
  r.last_error_code = "";
  r.sync_status = status;
  return r;
}
}  // namespace

int main() {
  std::printf("test_event_journal\n");

  // --- journal_record encode/decode round-trip ---
  {
    JournalRecord r = make_record("evt-1", JournalSyncStatus::ACKED);
    std::string body = encode_journal_event(r);
    check(journal_body_kind(body) == JournalBodyKind::EVENT, "encoded event body has kind E");
    JournalRecord decoded;
    check(decode_journal_event(body, decoded), "event round-trips through decode");
    check(decoded.event_id == "evt-1", "event_id preserved");
    check(decoded.device_seq == 42, "device_seq preserved");
    check(decoded.sync_status == JournalSyncStatus::ACKED, "sync_status preserved");
    check(decoded.payload == r.payload, "payload preserved exactly (incl. special JSON chars via escaping)");

    JournalTransition t;
    t.event_id = "evt-1";
    t.sync_status = JournalSyncStatus::CONFLICT;
    t.retry_count = 3;
    t.last_error_code = "STATE_CONFLICT";
    std::string tbody = encode_journal_transition(t);
    check(journal_body_kind(tbody) == JournalBodyKind::TRANSITION, "encoded transition body has kind T");
    JournalTransition tdec;
    check(decode_journal_transition(tbody, tdec), "transition round-trips through decode");
    check(tdec.sync_status == JournalSyncStatus::CONFLICT, "transition sync_status preserved");
  }

  // --- unknown record version ---
  {
    std::string bad = "{\"k\":\"E\",\"record_version\":99,\"event_id\":\"x\",\"event_type\":\"SCAN\","
                      "\"sync_status\":\"PENDING\"}";
    JournalRecord out;
    check(!decode_journal_event(bad, out), "unknown record_version (99) rejected, not guessed through");
  }

  // --- empty journal scans as zero records, no error ---
  {
    std::string path = tmp_path("empty");
    std::remove(path.c_str());
    StdFileJournalFile file(path);
    int n = scan_journal(file, [](const JournalScanFrame&) {});
    check(n == 0, "scanning a nonexistent/empty journal file yields 0 records, not an error");
  }

  // --- append one record, scan finds it ---
  {
    std::string path = tmp_path("one");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    std::string body = encode_journal_event(make_record("evt-one"));
    check(append_journal_frame(w, body), "append_journal_frame succeeds");

    StdFileJournalFile r(path);
    std::vector<JournalScanFrame> frames;
    int n = scan_journal(r, [&](const JournalScanFrame& f) { frames.push_back(f); });
    check(n == 1, "one appended record scans back as exactly one OK frame");
    check(frames.size() == 1 && frames[0].outcome == JournalFrameOutcome::OK, "the one frame has outcome OK");
    JournalRecord decoded;
    check(decode_journal_event(frames[0].body_json, decoded) && decoded.event_id == "evt-one",
          "scanned frame body decodes back to the same event_id");
  }

  // --- append many records (including a transition), reload/reboot recovery ---
  {
    std::string path = tmp_path("many");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    for (int i = 0; i < 20; ++i) {
      std::string body = encode_journal_event(make_record("evt-" + std::to_string(i)));
      check(append_journal_frame(w, body), "append many: record appended");
    }
    JournalTransition t;
    t.event_id = "evt-5";
    t.sync_status = JournalSyncStatus::ACKED;
    check(append_journal_frame(w, encode_journal_transition(t)), "append many: transition appended");

    // "reload/reboot" == a fresh scan over the same file, as if freshly booted.
    StdFileJournalFile r(path);
    EventJournalIndex idx(1u << 20);
    int n = scan_journal(r, [&](const JournalScanFrame& f) {
      if (f.outcome == JournalFrameOutcome::OK) idx.apply_scanned_frame(f.body_json, f.frame_size);
    });
    check(n == 21, "20 events + 1 transition scan back as 21 OK frames");
    check(idx.record_count() == 20, "index has exactly 20 distinct events (transition doesn't add a new one)");
    const JournalRecord* rec = idx.find("evt-5");
    check(rec != nullptr && rec->sync_status == JournalSyncStatus::ACKED,
          "the transitioned event's status reflects the LATEST transition after recovery");
    const JournalRecord* rec0 = idx.find("evt-0");
    check(rec0 != nullptr && rec0->sync_status == JournalSyncStatus::PENDING,
          "an untransitioned event keeps its original PENDING status after recovery");
  }

  // --- duplicate event_id: log-level (first-seen wins) and index-level (decide_append) ---
  {
    std::string path = tmp_path("dup");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    JournalRecord first = make_record("evt-dup");
    first.payload = "{\"quantity_good\":1}";
    check(append_journal_frame(w, encode_journal_event(first)), "first evt-dup appended");
    JournalRecord second = make_record("evt-dup");
    second.payload = "{\"quantity_good\":999}";  // different payload -- must NOT win
    check(append_journal_frame(w, encode_journal_event(second)), "second (duplicate id) evt-dup appended to log");

    StdFileJournalFile r(path);
    EventJournalIndex idx(1u << 20);
    scan_journal(r, [&](const JournalScanFrame& f) {
      if (f.outcome == JournalFrameOutcome::OK) idx.apply_scanned_frame(f.body_json, f.frame_size);
    });
    check(idx.record_count() == 1, "duplicate event_id in the log collapses to exactly one index entry");
    const JournalRecord* rec = idx.find("evt-dup");
    check(rec != nullptr && rec->payload == first.payload,
          "first-seen record wins on a log-level duplicate, not the later one (§7)");

    // Index-level: decide_append() BEFORE writing must refuse a duplicate.
    EventJournalIndex idx2(1u << 20);
    idx2.record_appended_event(make_record("evt-x"), 100);
    check(idx2.decide_append("evt-x", 50) == JournalAppendDecision::DUPLICATE_EVENT,
          "decide_append() refuses a duplicate event_id before ever writing");
    check(idx2.decide_append("evt-y", 50) == JournalAppendDecision::OK,
          "decide_append() allows a genuinely new event_id");
  }

  // --- truncated header (crash before the 4-byte length prefix completes) ---
  {
    std::string path = tmp_path("trunc_header");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    check(append_journal_frame(w, encode_journal_event(make_record("evt-good"))), "one good record first");
    long good_size = static_cast<long>(w.size());
    // Append a second frame, then truncate mid-length-prefix (2 of 4 bytes).
    StdFileJournalFile w2(path);
    append_journal_frame(w2, encode_journal_event(make_record("evt-truncated")));
    truncate_to(path, good_size + 2);

    StdFileJournalFile r(path);
    std::vector<JournalScanFrame> frames;
    int n = scan_journal(r, [&](const JournalScanFrame& f) { frames.push_back(f); });
    check(n == 1, "truncated header: only the first complete record recovers");
    check(frames.back().outcome == JournalFrameOutcome::INCOMPLETE,
          "truncated header: the partial tail frame is reported INCOMPLETE, not silently dropped");
  }

  // --- truncated payload (crash mid-body) ---
  {
    std::string path = tmp_path("trunc_payload");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    check(append_journal_frame(w, encode_journal_event(make_record("evt-good2"))), "one good record first");
    long good_size = static_cast<long>(w.size());
    StdFileJournalFile w2(path);
    append_journal_frame(w2, encode_journal_event(make_record("evt-truncated2")));
    // Cut somewhere inside the body (past the 4-byte length prefix).
    truncate_to(path, good_size + 4 + 5);

    StdFileJournalFile r(path);
    std::vector<JournalScanFrame> frames;
    int n = scan_journal(r, [&](const JournalScanFrame& f) { frames.push_back(f); });
    check(n == 1, "truncated payload: only the first complete record recovers");
    check(frames.back().outcome == JournalFrameOutcome::INCOMPLETE, "truncated payload reported INCOMPLETE");
  }

  // --- missing commit marker (crash after CRC, before the marker byte) ---
  {
    std::string path = tmp_path("no_marker");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    check(append_journal_frame(w, encode_journal_event(make_record("evt-good3"))), "one good record first");
    long good_size = static_cast<long>(w.size());
    StdFileJournalFile w2(path);
    std::string body = encode_journal_event(make_record("evt-nomarker"));
    append_journal_frame(w2, body);
    long full_size = static_cast<long>(w2.size());
    check(full_size == good_size + 4 + static_cast<long>(body.size()) + 4 + 1, "sanity: frame size matches 4+body+4+1");
    truncate_to(path, full_size - 1);  // remove exactly the 1-byte commit marker

    StdFileJournalFile r(path);
    std::vector<JournalScanFrame> frames;
    int n = scan_journal(r, [&](const JournalScanFrame& f) { frames.push_back(f); });
    check(n == 1, "missing commit marker: only the first complete record recovers");
    check(frames.back().outcome == JournalFrameOutcome::INCOMPLETE,
          "a frame with body+CRC present but NO commit marker byte at all is INCOMPLETE (not BAD_COMMIT_MARKER)");
  }

  // --- bad CRC (body corrupted after the fact, commit marker still present) ---
  {
    std::string path = tmp_path("bad_crc");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    std::string body = encode_journal_event(make_record("evt-corrupt"));
    check(append_journal_frame(w, body), "one record appended");
    // Flip one byte inside the body region (offset 4 = right after the
    // 4-byte length prefix) to simulate flash-level bit corruption.
    FILE* f = std::fopen(path.c_str(), "r+b");
    check(f != nullptr, "reopen for corruption injection");
    std::fseek(f, 4, SEEK_SET);
    int c = std::fgetc(f);
    std::fseek(f, 4, SEEK_SET);
    std::fputc(c ^ 0xFF, f);
    std::fclose(f);

    StdFileJournalFile r(path);
    std::vector<JournalScanFrame> frames;
    int n = scan_journal(r, [&](const JournalScanFrame& f) { frames.push_back(f); });
    check(n == 0, "bad CRC: the corrupted record does not recover as valid");
    check(!frames.empty() && frames.back().outcome == JournalFrameOutcome::CRC_FAIL,
          "bad CRC: reported as CRC_FAIL specifically, not INCOMPLETE or silently ignored");
  }

  // --- recovery after partial tail record (2 good + 1 truncated) ---
  {
    std::string path = tmp_path("partial_tail");
    std::remove(path.c_str());
    StdFileJournalFile w(path);
    append_journal_frame(w, encode_journal_event(make_record("evt-a")));
    append_journal_frame(w, encode_journal_event(make_record("evt-b")));
    long two_good_size = static_cast<long>(w.size());
    StdFileJournalFile w2(path);
    append_journal_frame(w2, encode_journal_event(make_record("evt-c-partial")));
    truncate_to(path, two_good_size + 3);  // chop the third record's length prefix short

    StdFileJournalFile r(path);
    EventJournalIndex idx(1u << 20);
    int n = scan_journal(r, [&](const JournalScanFrame& f) {
      if (f.outcome == JournalFrameOutcome::OK) idx.apply_scanned_frame(f.body_json, f.frame_size);
    });
    check(n == 2, "partial tail: exactly the 2 complete leading records recover");
    check(idx.has_event("evt-a") && idx.has_event("evt-b"), "both complete records present in the index");
    check(!idx.has_event("evt-c-partial"), "the partial tail record is NOT present -- never a fake committed event");
  }

  // --- capacity / pressure thresholds ---
  {
    check(journal_pressure_for_usage(0.0) == JournalPressure::NORMAL, "0% -> NORMAL");
    check(journal_pressure_for_usage(69.9) == JournalPressure::NORMAL, "69.9% -> NORMAL");
    check(journal_pressure_for_usage(70.0) == JournalPressure::WARNING, "70% -> WARNING");
    check(journal_pressure_for_usage(89.9) == JournalPressure::WARNING, "89.9% -> WARNING");
    check(journal_pressure_for_usage(90.0) == JournalPressure::RESTRICTED, "90% -> RESTRICTED");
    check(journal_pressure_for_usage(99.9) == JournalPressure::RESTRICTED, "99.9% -> RESTRICTED");
    check(journal_pressure_for_usage(100.0) == JournalPressure::FULL, "100% -> FULL");

    // A tiny capacity that fills up after one record -- proves FULL
    // actually refuses further writes rather than overwriting/silently
    // dropping (docs/OFFLINE.md "no silent fallback"). A real encoded
    // JournalRecord body is ~350 bytes (all the required fields, see
    // journal_record.h) -- 500 leaves room for exactly one.
    EventJournalIndex idx(500);
    JournalRecord r1 = make_record("evt-cap-1");
    std::string b1 = encode_journal_event(r1);
    uint32_t frame1 = kJournalFrameOverheadBytes + static_cast<uint32_t>(b1.size());
    check(idx.decide_append("evt-cap-1", frame1) == JournalAppendDecision::OK, "first small record fits");
    idx.record_appended_event(r1, frame1);

    JournalRecord r2 = make_record("evt-cap-2-with-a-longer-payload-to-force-overflow");
    r2.payload = std::string(200, 'x');  // deliberately oversized to force FULL
    std::string b2 = encode_journal_event(r2);
    uint32_t frame2 = kJournalFrameOverheadBytes + static_cast<uint32_t>(b2.size());
    check(idx.decide_append("evt-cap-2-with-a-longer-payload-to-force-overflow", frame2) == JournalAppendDecision::FULL,
          "an oversized-for-remaining-capacity record is refused as FULL, not silently written over old data");
  }

  // --- compaction: select_records_to_keep() / rebuild_after_compaction() ---
  // (2026-08-24, self-recovery task -- real bug: records_ never shrank,
  // eventually exhausting internal SRAM via xTaskCreate() failures.)
  {
    EventJournalIndex idx(1000000);  // plenty of capacity -- this block tests selection logic, not FULL handling

    // Real frame size (matches what rebuild_after_compaction() itself
    // recomputes via encode_journal_event()) -- using a realistic size here,
    // not an arbitrary placeholder, so the "used_bytes shrinks" assertion
    // below reflects an actual size reduction, not a test artifact.
    auto real_frame_size = [](const JournalRecord& r) {
      std::string body = encode_journal_event(r);
      return kJournalFrameOverheadBytes + static_cast<uint32_t>(body.size());
    };

    // Non-terminal records: always kept, regardless of age.
    JournalRecord pending = make_record("evt-pending", JournalSyncStatus::PENDING);
    pending.created_uptime_ms = 100;
    JournalRecord inflight = make_record("evt-inflight", JournalSyncStatus::IN_FLIGHT);
    inflight.created_uptime_ms = 200;
    JournalRecord conflict = make_record("evt-conflict", JournalSyncStatus::CONFLICT);
    conflict.created_uptime_ms = 300;
    JournalRecord human_review = make_record("evt-human-review", JournalSyncStatus::HUMAN_REVIEW);
    human_review.created_uptime_ms = 400;
    idx.record_appended_event(pending, real_frame_size(pending));
    idx.record_appended_event(inflight, real_frame_size(inflight));
    idx.record_appended_event(conflict, real_frame_size(conflict));
    idx.record_appended_event(human_review, real_frame_size(human_review));

    // 5 ACKED records at increasing created_uptime_ms -- only the 2 newest
    // should survive a retention count of 2.
    for (int i = 0; i < 5; ++i) {
      JournalRecord r = make_record("evt-acked-" + std::to_string(i), JournalSyncStatus::ACKED);
      r.created_uptime_ms = 1000 + i;  // evt-acked-4 is newest
      idx.record_appended_event(r, real_frame_size(r));
    }
    // 3 REJECTED records -- only the 2 newest should survive.
    for (int i = 0; i < 3; ++i) {
      JournalRecord r = make_record("evt-rejected-" + std::to_string(i), JournalSyncStatus::REJECTED);
      r.created_uptime_ms = 2000 + i;  // evt-rejected-2 is newest
      idx.record_appended_event(r, real_frame_size(r));
    }

    check(idx.record_count() == 12, "12 records appended before compaction (4 non-terminal + 5 acked + 3 rejected)");

    EventJournalIndex::CompactionPolicy policy;
    policy.acked_retention_count = 2;
    policy.rejected_retention_count = 2;
    std::vector<std::string> keep = idx.select_records_to_keep(policy);
    auto contains = [&](const std::string& id) {
      return std::find(keep.begin(), keep.end(), id) != keep.end();
    };

    check(keep.size() == 8, "4 non-terminal + 2 kept acked + 2 kept rejected = 8 survivors");
    check(contains("evt-pending") && contains("evt-inflight") && contains("evt-conflict") && contains("evt-human-review"),
          "all 4 non-terminal records survive selection regardless of age");
    check(contains("evt-acked-4") && contains("evt-acked-3"), "the 2 NEWEST acked records survive");
    check(!contains("evt-acked-0") && !contains("evt-acked-1") && !contains("evt-acked-2"),
          "the 3 OLDEST acked records are dropped from the keep-list");
    check(contains("evt-rejected-2") && contains("evt-rejected-1"), "the 2 newest rejected records survive");
    check(!contains("evt-rejected-0"), "the oldest rejected record is dropped from the keep-list");

    uint32_t used_before = idx.used_bytes();
    idx.rebuild_after_compaction(keep);
    check(idx.record_count() == 8, "rebuild_after_compaction() shrinks record_count() to exactly the keep-list size");
    check(idx.used_bytes() < used_before, "rebuild_after_compaction() shrinks used_bytes() (fewer surviving frames)");
    check(idx.find("evt-acked-0") == nullptr,
          "a dropped record is genuinely gone from find() after rebuild, not just absent from the keep-list");
    check(idx.find("evt-pending") != nullptr, "a kept non-terminal record is still findable after rebuild");
    check(idx.find("evt-acked-4") != nullptr, "the newest kept acked record is still findable after rebuild");

    // A retention count larger than the available bucket keeps everything in
    // that bucket -- never pad/fabricate, never crash.
    EventJournalIndex::CompactionPolicy generous_policy;
    generous_policy.acked_retention_count = 100;
    generous_policy.rejected_retention_count = 100;
    std::vector<std::string> keep_all = idx.select_records_to_keep(generous_policy);
    check(keep_all.size() == 8, "a retention count larger than the bucket keeps every remaining record, no crash/overrun");
  }

  // --- should_consider_compaction() matches the 70% WARNING boundary ---
  {
    EventJournalIndex idx(1000);
    JournalRecord r = make_record("evt-a");
    idx.record_appended_event(r, 699);  // 69.9% used
    check(!idx.should_consider_compaction(), "69.9% usage does not yet urge compaction");
    JournalRecord r2 = make_record("evt-b");
    idx.record_appended_event(r2, 1);  // 70.0% used
    check(idx.should_consider_compaction(), "70.0% usage crosses the WARNING boundary and urges compaction");
  }

  std::printf("\n%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
