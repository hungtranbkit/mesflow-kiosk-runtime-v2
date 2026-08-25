// Plain C++, no Arduino.h — host-testable. Phase 3A durable journal: the
// on-disk FRAME format and scan/append/recovery logic, operating over the
// abstract JournalFile interface below rather than SPIFFS directly -- so
// this exact byte-level logic can be exercised on host (test/host/, using a
// real temp file via <cstdio>) and reused unchanged on-device
// (storage/event_journal.cpp, wrapping fs::File). Same protocol/-vs-
// storage/ split ui_bundle.h/ui_bundle_store.h already use.
//
// Frame layout (§3/§4 of the Phase 3A task -- "write header, write payload,
// flush, write CRC, flush, write commit marker LAST, flush"):
//
//   [4 bytes LE: body_len]   \__ one write()+flush() group ("header"+"payload")
//   [body_len bytes: body]   /
//   [4 bytes LE: crc32(body)]  -- separate write()+flush()
//   [1 byte: kCommitMarker]    -- separate write()+flush(), LAST
//
// A crash between any two flush() calls leaves the frame incomplete;
// recovery (scan_journal()) never treats an incomplete frame as committed.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace kiosk::protocol {

constexpr uint8_t kJournalCommitMarker = 0xC3;

// Minimal file abstraction -- deliberately NOT fs::File/FILE* directly, so
// this header stays portable. Semantics match ordinary sequential
// read/write/append file I/O.
class JournalFile {
 public:
  virtual ~JournalFile() = default;
  virtual bool open_for_append() = 0;  // create if missing; position at end
  virtual bool open_for_read() = 0;    // position at start; ok to call even if file doesn't exist (treated as empty)
  virtual void close() = 0;
  virtual int write(const uint8_t* data, size_t len) = 0;  // returns bytes written, -1 on error
  virtual bool flush() = 0;
  virtual int read(uint8_t* buf, size_t len) = 0;  // returns bytes read (0..len), 0 = EOF, never -1 for a clean EOF
  virtual uint32_t size() = 0;                     // total bytes currently in the file
};

enum class JournalFrameOutcome {
  OK,
  INCOMPLETE,           // truncated mid-frame (power loss signature) -- §5 JOURNAL_INCOMPLETE_RECORD
  CRC_FAIL,              // body present in full, but crc32 mismatch -- §5 JOURNAL_CRC_FAIL
  BAD_COMMIT_MARKER,     // crc ok, but the marker byte isn't kJournalCommitMarker -- §5 JOURNAL_CORRUPT
};

struct JournalScanFrame {
  uint32_t offset = 0;       // byte offset this frame started at
  uint32_t frame_size = 0;   // total on-disk size of this frame (4+body_len+4+1), 0 if outcome != OK
  std::string body_json;     // only meaningful when outcome == OK
  JournalFrameOutcome outcome = JournalFrameOutcome::OK;
};

// Scans `file` sequentially from the start, invoking `on_frame` once per
// frame encountered. Stops at the first non-OK frame (an incomplete/corrupt
// tail always means "nothing valid follows" for an append-only log written
// by this same code) OR at a clean EOF between frames. Returns the number
// of OK frames found. `on_frame` is called even for the one non-OK frame
// (if any) so the caller can log it -- see docs/OFFLINE.md "no silent
// fallback".
int scan_journal(JournalFile& file, const std::function<void(const JournalScanFrame&)>& on_frame);

// Appends one frame for `body_json`, in the exact flush-ordered sequence
// documented above. Returns false if any write/flush step fails (caller
// must treat the append as NOT durable in that case -- see
// storage/event_journal.cpp for what happens to used_bytes accounting).
bool append_journal_frame(JournalFile& file, const std::string& body_json);

}  // namespace kiosk::protocol
