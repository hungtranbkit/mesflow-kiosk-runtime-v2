#include "event_journal.h"

#include <SPIFFS.h>

#include "../health/structured_log.h"
#include "../protocol/event_journal_core.h"

namespace kiosk::storage {

namespace {
constexpr const char* kJournalPath = "/journal.dat";
// Compaction working files (2026-08-24, self-recovery task) -- see
// EventJournal::compact()'s comment for the full crash-safety sequence these
// support, and init()'s boot-time recovery for how each interruption window
// is handled.
constexpr const char* kJournalTmpPath = "/journal.dat.tmp";
constexpr const char* kJournalOldPath = "/journal.dat.old";

// Thin adapter: kiosk::protocol::JournalFile's small interface, backed by a
// real fs::File. Mirrors the host test's StdFileJournalFile -- same frame
// format, same recovery logic, different I/O backend (§2/§13 of the task:
// this is exactly why event_journal_core.h was written portable).
//
// Parametrized by path (2026-08-24) so compact() can write a separate
// /journal.dat.tmp without disturbing the live /journal.dat -- defaults to
// the live path so every pre-existing call site is unchanged.
class SpiffsJournalFile : public kiosk::protocol::JournalFile {
 public:
  explicit SpiffsJournalFile(const char* path = kJournalPath) : path_(path) {}

  bool open_for_append() override {
    file_ = SPIFFS.open(path_, FILE_APPEND);
    return static_cast<bool>(file_);
  }
  bool open_for_read() override {
    if (!SPIFFS.exists(path_)) return true;  // no file yet == empty journal, not an error
    file_ = SPIFFS.open(path_, FILE_READ);
    return static_cast<bool>(file_);
  }
  void close() override {
    if (file_) file_.close();
  }
  int write(const uint8_t* data, size_t len) override {
    if (!file_) return -1;
    size_t n = file_.write(data, len);
    return static_cast<int>(n);
  }
  bool flush() override {
    if (!file_) return false;
    file_.flush();
    return true;
  }
  int read(uint8_t* buf, size_t len) override {
    if (!file_) return 0;  // no file opened (empty journal) -- clean EOF
    if (!file_.available()) return 0;
    size_t n = file_.read(buf, len);
    return static_cast<int>(n);
  }
  uint32_t size() override { return SPIFFS.exists(path_) ? static_cast<uint32_t>(SPIFFS.open(path_, FILE_READ).size()) : 0; }

 private:
  const char* path_;
  File file_;
};
}  // namespace

void EventJournal::init(uint32_t capacity_bytes) {
  // Idempotent/safe even if UiBundleStore already mounted SPIFFS -- the
  // ESP32 SPIFFS library treats a second begin() as a cheap no-op when
  // already mounted rather than re-formatting.
  if (!SPIFFS.begin(true)) {
    kiosk::health::log_structured("ERROR", "JOURNAL_INIT", "event_journal",
                                  "SPIFFS.begin() failed -- journal unavailable this boot");
    init_ok_ = false;
    return;
  }

  index_ = kiosk::protocol::EventJournalIndex(capacity_bytes);

  // --- Boot-time compaction-orphan recovery (2026-08-24, self-recovery
  // task) -- must run BEFORE the normal scan below, since it decides which
  // file /journal.dat even is if compact() was interrupted mid-swap. Every
  // window compact()'s rename sequence can be interrupted in:
  //   * only .tmp exists (crash during/after writing it, before any rename):
  //     stray, unvalidated by this boot -- remove it, live file untouched.
  //   * .dat and .old both exist (crash between the two renames, i.e. the
  //     live file IS already the new one): the swap's last step (removing
  //     .old) just didn't run -- remove the now-redundant .old.
  //   * .dat missing but .old present (crash between renaming .dat->.old
  //     and renaming .tmp->.dat): the live file was moved out but the new
  //     one never arrived -- rename .old back to .dat to restore it.
  //   * .tmp AND .old both present with .dat missing: same case as above,
  //     the .tmp never made it to .dat -- restore .old, then also remove
  //     the stray .tmp (never risk swapping in a file this boot never
  //     validated itself).
  bool has_dat = SPIFFS.exists(kJournalPath);
  bool has_old = SPIFFS.exists(kJournalOldPath);
  bool has_tmp = SPIFFS.exists(kJournalTmpPath);
  if (has_tmp) {
    kiosk::health::log_structured("WARN", "JOURNAL_COMPACT_ORPHAN_TMP", "event_journal",
                                  "removing unvalidated compaction temp file left from a prior crash");
    SPIFFS.remove(kJournalTmpPath);
  }
  if (!has_dat && has_old) {
    kiosk::health::log_structured("WARN", "JOURNAL_COMPACT_ORPHAN_RESTORE", "event_journal",
                                  "restoring /journal.dat from .old -- prior compaction crashed mid-swap");
    SPIFFS.rename(kJournalOldPath, kJournalPath);
  } else if (has_dat && has_old) {
    kiosk::health::log_structured("WARN", "JOURNAL_COMPACT_ORPHAN_CLEANUP", "event_journal",
                                  "removing redundant .old -- prior compaction's swap completed but cleanup didn't");
    SPIFFS.remove(kJournalOldPath);
  }

  kiosk::health::log_structured("INFO", "JOURNAL_RECOVERY_BEGIN", "event_journal", kJournalPath);
  SpiffsJournalFile file;
  int incomplete = 0, crc_fail = 0, bad_marker = 0, duplicates_seen = 0, unknown_kind = 0;
  int ok = kiosk::protocol::scan_journal(file, [&](const kiosk::protocol::JournalScanFrame& f) {
    switch (f.outcome) {
      case kiosk::protocol::JournalFrameOutcome::OK: {
        bool was_new = false;
        auto kind = kiosk::protocol::journal_body_kind(f.body_json);
        if (kind == kiosk::protocol::JournalBodyKind::EVENT) {
          kiosk::protocol::JournalRecord r;
          if (kiosk::protocol::decode_journal_event(f.body_json, r)) {
            was_new = !index_.has_event(r.event_id);
            if (!was_new) ++duplicates_seen;
          } else {
            ++unknown_kind;
          }
        } else if (kind == kiosk::protocol::JournalBodyKind::UNKNOWN) {
          ++unknown_kind;
        }
        index_.apply_scanned_frame(f.body_json, f.frame_size);
        if (!was_new && kind == kiosk::protocol::JournalBodyKind::EVENT) {
          kiosk::health::log_structured("WARN", "JOURNAL_DUPLICATE_EVENT", "event_journal",
                                        ("offset=" + std::to_string(f.offset)).c_str());
        }
        break;
      }
      case kiosk::protocol::JournalFrameOutcome::INCOMPLETE:
        ++incomplete;
        kiosk::health::log_structured("WARN", "JOURNAL_INCOMPLETE_RECORD", "event_journal",
                                      ("offset=" + std::to_string(f.offset)).c_str());
        break;
      case kiosk::protocol::JournalFrameOutcome::CRC_FAIL:
        ++crc_fail;
        kiosk::health::log_structured("ERROR", "JOURNAL_CRC_FAIL", "event_journal",
                                      ("offset=" + std::to_string(f.offset)).c_str());
        break;
      case kiosk::protocol::JournalFrameOutcome::BAD_COMMIT_MARKER:
        ++bad_marker;
        kiosk::health::log_structured("ERROR", "JOURNAL_CORRUPT", "event_journal",
                                      ("offset=" + std::to_string(f.offset)).c_str());
        break;
    }
  });

  char msg[192];
  snprintf(msg, sizeof(msg),
           "recovered=%d incomplete=%d crc_fail=%d bad_marker=%d duplicates=%d unknown=%d used_bytes=%u",
           ok, incomplete, crc_fail, bad_marker, duplicates_seen, unknown_kind, index_.used_bytes());
  kiosk::health::log_structured("INFO", "JOURNAL_RECOVERY_END", "event_journal", msg);
  init_ok_ = true;
}

bool EventJournal::append_event(const kiosk::protocol::JournalRecord& record) {
  if (!init_ok_) return false;
  std::string body = kiosk::protocol::encode_journal_event(record);
  uint32_t frame_size = kiosk::protocol::kJournalFrameOverheadBytes + static_cast<uint32_t>(body.size());

  auto decision = index_.decide_append(record.event_id, frame_size);
  if (decision == kiosk::protocol::JournalAppendDecision::DUPLICATE_EVENT) {
    kiosk::health::log_structured("WARN", "JOURNAL_DUPLICATE_EVENT", "event_journal", record.event_id.c_str());
    return false;
  }
  if (decision == kiosk::protocol::JournalAppendDecision::FULL) {
    kiosk::health::log_structured("ERROR", "JOURNAL_FULL", "event_journal",
                                  (std::string("event_id=") + record.event_id +
                                   " used=" + std::to_string(index_.used_bytes()) +
                                   " capacity=" + std::to_string(index_.capacity_bytes()))
                                      .c_str());
    return false;
  }

  kiosk::health::log_structured("INFO", "JOURNAL_APPEND_BEGIN", "event_journal", record.event_id.c_str());
  SpiffsJournalFile file;
  if (!kiosk::protocol::append_journal_frame(file, body)) {
    kiosk::health::log_structured("ERROR", "JOURNAL_APPEND_FAIL", "event_journal", record.event_id.c_str());
    return false;
  }
  index_.record_appended_event(record, frame_size);

  double pct = index_.usage_pct();
  if (pct >= 70.0) {
    char msg[96];
    snprintf(msg, sizeof(msg), "usage_pct=%.1f pressure=%d", pct, static_cast<int>(index_.pressure()));
    kiosk::health::log_structured("WARN", "JOURNAL_CAPACITY_WARNING", "event_journal", msg);
  }
  kiosk::health::log_structured("INFO", "JOURNAL_APPEND_OK", "event_journal", record.event_id.c_str());
  return true;
}

bool EventJournal::append_transition(const kiosk::protocol::JournalTransition& transition) {
  if (!init_ok_) return false;
  if (!index_.has_event(transition.event_id)) {
    kiosk::health::log_structured("WARN", "JOURNAL_APPEND_FAIL", "event_journal",
                                  ("transition for unknown event_id=" + transition.event_id).c_str());
    return false;
  }
  std::string body = kiosk::protocol::encode_journal_transition(transition);
  uint32_t frame_size = kiosk::protocol::kJournalFrameOverheadBytes + static_cast<uint32_t>(body.size());

  SpiffsJournalFile file;
  if (!kiosk::protocol::append_journal_frame(file, body)) {
    kiosk::health::log_structured("ERROR", "JOURNAL_APPEND_FAIL", "event_journal", transition.event_id.c_str());
    return false;
  }
  index_.record_appended_transition(transition, frame_size);
  return true;
}

bool EventJournal::compact(const kiosk::protocol::EventJournalIndex::CompactionPolicy& policy) {
  if (!init_ok_) return false;

  std::vector<std::string> keep = index_.select_records_to_keep(policy);
  uint32_t before_count = index_.record_count();
  if (keep.size() == before_count) {
    // Nothing to drop -- every terminal record is already within its
    // retention count. Not an error, just a no-op worth a quiet log so
    // "compaction ran but changed nothing" is distinguishable from "never
    // ran at all" in the field.
    kiosk::health::log_structured("INFO", "JOURNAL_COMPACTION_SKIP", "event_journal",
                                  "nothing to drop -- all terminal records already within retention");
    return true;
  }

  char begin_msg[96];
  snprintf(begin_msg, sizeof(begin_msg), "before=%u keep=%u", before_count,
           static_cast<unsigned>(keep.size()));
  kiosk::health::log_structured("INFO", "JOURNAL_COMPACTION_START", "event_journal", begin_msg);

  // Step 1: write every surviving record as a fresh EVENT frame to the temp
  // file. Always start from a clean temp file -- a leftover from an
  // interrupted PRIOR attempt must never be appended onto.
  SPIFFS.remove(kJournalTmpPath);
  {
    SpiffsJournalFile tmp(kJournalTmpPath);
    for (const auto& event_id : keep) {
      const kiosk::protocol::JournalRecord* r = index_.find(event_id);
      if (!r) continue;  // defensive -- select_records_to_keep() only returns ids that exist in this same index
      std::string body = kiosk::protocol::encode_journal_event(*r);
      if (!kiosk::protocol::append_journal_frame(tmp, body)) {
        kiosk::health::log_structured("ERROR", "JOURNAL_COMPACTION_FAIL", "event_journal",
                                      "write to temp file failed -- live journal left untouched");
        SPIFFS.remove(kJournalTmpPath);
        return false;
      }
    }
  }

  return finish_compaction_validate_and_swap(keep);
}

bool EventJournal::finish_compaction_validate_and_swap(const std::vector<std::string>& keep) {
  // VALIDATE by re-scanning the temp file exactly like a real boot would --
  // every frame must be OK and the count must match what was written.
  // Never swap in a file this process hasn't itself confirmed readable.
  {
    SpiffsJournalFile check(kJournalTmpPath);
    int bad = 0;
    int seen = 0;
    int ok = kiosk::protocol::scan_journal(check, [&](const kiosk::protocol::JournalScanFrame& f) {
      ++seen;
      if (f.outcome != kiosk::protocol::JournalFrameOutcome::OK) ++bad;
    });
    if (bad > 0 || seen != static_cast<int>(keep.size()) || ok != static_cast<int>(keep.size())) {
      char msg[128];
      snprintf(msg, sizeof(msg), "validation failed: seen=%d ok=%d bad=%d expected=%u", seen, ok, bad,
               static_cast<unsigned>(keep.size()));
      kiosk::health::log_structured("ERROR", "JOURNAL_COMPACTION_FAIL", "event_journal", msg);
      SPIFFS.remove(kJournalTmpPath);
      return false;
    }
  }

  // Atomic-as-possible swap. If the rename to .old fails outright (e.g.
  // .dat vanished from under us), abort rather than guess.
  if (SPIFFS.exists(kJournalPath)) {
    if (!SPIFFS.rename(kJournalPath, kJournalOldPath)) {
      kiosk::health::log_structured("ERROR", "JOURNAL_COMPACTION_FAIL", "event_journal",
                                    "rename .dat -> .old failed -- live journal left untouched");
      SPIFFS.remove(kJournalTmpPath);
      return false;
    }
  }
  if (!SPIFFS.rename(kJournalTmpPath, kJournalPath)) {
    // Worst case of this step: .dat is now missing and .tmp still holds the
    // validated data. Restore .old back to .dat so the device isn't left
    // without ANY journal file, then report failure -- init()'s own
    // orphan-recovery logic would also catch this on a reboot, but don't
    // rely on a reboot happening.
    kiosk::health::log_structured("ERROR", "JOURNAL_COMPACTION_FAIL", "event_journal",
                                  "rename .tmp -> .dat failed -- restoring .old");
    if (SPIFFS.exists(kJournalOldPath)) SPIFFS.rename(kJournalOldPath, kJournalPath);
    return false;
  }
  SPIFFS.remove(kJournalOldPath);

  // Only now, with the on-disk swap fully committed, does the in-memory
  // index change to match.
  index_.rebuild_after_compaction(keep);

  char end_msg[96];
  snprintf(end_msg, sizeof(end_msg), "after=%u used_bytes=%u", index_.record_count(), index_.used_bytes());
  kiosk::health::log_structured("INFO", "JOURNAL_COMPACTION_OK", "event_journal", end_msg);
  return true;
}

bool EventJournal::begin_compaction(const kiosk::protocol::EventJournalIndex::CompactionPolicy& policy) {
  if (!init_ok_ || compaction_active_) return false;

  compaction_keep_ = index_.select_records_to_keep(policy);
  uint32_t before_count = index_.record_count();
  if (compaction_keep_.size() == before_count) {
    kiosk::health::log_structured("INFO", "JOURNAL_COMPACTION_SKIP", "event_journal",
                                  "nothing to drop -- all terminal records already within retention");
    compaction_keep_.clear();
    return false;
  }

  char begin_msg[112];
  snprintf(begin_msg, sizeof(begin_msg), "before=%u keep=%u incremental=1 chunk=%u", before_count,
           static_cast<unsigned>(compaction_keep_.size()), static_cast<unsigned>(kCompactionRecordsPerTick));
  kiosk::health::log_structured("INFO", "JOURNAL_COMPACTION_START", "event_journal", begin_msg);

  // Same "always start from a clean temp file" rule as compact() -- a
  // leftover from an interrupted PRIOR attempt must never be appended onto.
  SPIFFS.remove(kJournalTmpPath);
  compaction_write_index_ = 0;
  compaction_last_progress_ms_ = millis();
  compaction_active_ = true;
  return true;
}

void EventJournal::compact_tick() {
  if (!compaction_active_) return;

  // append_journal_frame() opens_for_append()+closes the file itself on
  // EVERY call (see event_journal_core.cpp) -- there is no cross-call state
  // worth holding onto here, so a fresh SpiffsJournalFile per compact_tick()
  // call is exactly equivalent to compact()'s single reused instance, just
  // without an extra pointer/lifetime to manage across calls.
  SpiffsJournalFile tmp(kJournalTmpPath);

  uint32_t end = static_cast<uint32_t>(compaction_write_index_) + kCompactionRecordsPerTick;
  if (end > compaction_keep_.size()) end = static_cast<uint32_t>(compaction_keep_.size());

  bool failed = false;
  for (uint32_t i = static_cast<uint32_t>(compaction_write_index_); i < end; ++i) {
    const kiosk::protocol::JournalRecord* r = index_.find(compaction_keep_[i]);
    if (!r) continue;  // defensive -- keep-list only ever names ids that exist in this same index
    std::string body = kiosk::protocol::encode_journal_event(*r);
    if (!kiosk::protocol::append_journal_frame(tmp, body)) {
      kiosk::health::log_structured("ERROR", "JOURNAL_COMPACTION_FAIL", "event_journal",
                                    "write to temp file failed mid-chunk -- live journal left untouched");
      failed = true;
      break;
    }
    compaction_write_index_ = i + 1;
  }
  compaction_last_progress_ms_ = millis();

  if (failed) {
    SPIFFS.remove(kJournalTmpPath);
    compaction_active_ = false;
    compaction_keep_.clear();
    return;
  }

  if (compaction_write_index_ < compaction_keep_.size()) return;  // more chunks still to go

  // Last chunk just finished writing -- run the same synchronous
  // validate+swap+rebuild tail compact() uses. This part is fast (reads +
  // renames, not per-record encode+flash-write), so doing it in one call
  // here is a reasonable, bounded exception to "chunk everything".
  finish_compaction_validate_and_swap(compaction_keep_);
  compaction_active_ = false;
  compaction_keep_.clear();
  compaction_write_index_ = 0;
}

#if MESFLOW_DEBUG_API
void EventJournal::simulate_compaction_crash_for_test(int scenario) {
  switch (scenario) {
    case 1: {
      // Incomplete tmp: a frame header claiming a body this "file" doesn't
      // actually have -- exactly what a crash mid-write of one frame would
      // leave. scan_journal() must report this INCOMPLETE, never OK.
      SPIFFS.remove(kJournalTmpPath);
      File f = SPIFFS.open(kJournalTmpPath, FILE_APPEND);
      if (f) {
        uint8_t garbage[6] = {0x64, 0x00, 0x00, 0x00, 0xAA, 0xBB};  // claims a 100-byte body_len, only 2 bytes follow
        f.write(garbage, sizeof(garbage));
        f.close();
      }
      break;
    }
    case 2: {
      // Complete, VALID tmp -- a real compaction that finished writing but
      // crashed before validate+swap ever ran. Reuses the real keep-list
      // logic so this is a genuinely valid file, not hand-crafted bytes.
      auto keep = index_.select_records_to_keep(kiosk::protocol::EventJournalIndex::CompactionPolicy());
      SPIFFS.remove(kJournalTmpPath);
      SpiffsJournalFile tmp(kJournalTmpPath);
      for (const auto& id : keep) {
        const kiosk::protocol::JournalRecord* r = index_.find(id);
        if (!r) continue;
        kiosk::protocol::append_journal_frame(tmp, kiosk::protocol::encode_journal_event(*r));
      }
      break;
    }
    case 3: {
      // Mid-swap: .dat renamed to .old, the new file never arrived.
      SPIFFS.remove(kJournalTmpPath);
      if (SPIFFS.exists(kJournalPath)) SPIFFS.rename(kJournalPath, kJournalOldPath);
      break;
    }
    case 4: {
      // Swap completed, final cleanup (removing .old) didn't run -- both
      // present. .old is a byte-for-byte copy of .dat, standing in for
      // "the same content the completed swap actually left".
      SPIFFS.remove(kJournalTmpPath);
      SPIFFS.remove(kJournalOldPath);
      if (SPIFFS.exists(kJournalPath)) {
        File src = SPIFFS.open(kJournalPath, FILE_READ);
        File dst = SPIFFS.open(kJournalOldPath, FILE_APPEND);
        if (src && dst) {
          uint8_t buf[512];
          int n;
          while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, n);
        }
        if (src) src.close();
        if (dst) dst.close();
      }
      break;
    }
    default:
      kiosk::health::log_structured("WARN", "JOURNAL_TEST_HOOK", "event_journal",
                                    "simulate_compaction_crash_for_test: unknown scenario");
      break;
  }
}
#endif

}  // namespace kiosk::storage
