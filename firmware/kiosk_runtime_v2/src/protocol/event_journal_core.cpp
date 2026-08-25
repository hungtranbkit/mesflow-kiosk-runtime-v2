#include "event_journal_core.h"

#include "crc32.h"

namespace kiosk::protocol {

namespace {
// Reads exactly `len` bytes or returns false if the file runs out first
// (an incomplete-frame signature, never a hard error to the caller).
bool read_exact(JournalFile& file, uint8_t* buf, size_t len, size_t* out_read) {
  size_t got = 0;
  while (got < len) {
    int n = file.read(buf + got, len - got);
    if (n <= 0) break;  // EOF or error -- either way, incomplete
    got += static_cast<size_t>(n);
  }
  if (out_read) *out_read = got;
  return got == len;
}

uint32_t read_u32_le(const uint8_t* b) {
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

void write_u32_le(uint8_t* b, uint32_t v) {
  b[0] = static_cast<uint8_t>(v & 0xFF);
  b[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  b[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  b[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
}  // namespace

int scan_journal(JournalFile& file, const std::function<void(const JournalScanFrame&)>& on_frame) {
  if (!file.open_for_read()) return 0;
  int ok_count = 0;
  uint32_t offset = 0;

  for (;;) {
    uint8_t len_bytes[4];
    size_t got = 0;
    bool have_len = read_exact(file, len_bytes, 4, &got);
    if (!have_len) {
      if (got > 0) {
        // Partial length prefix -- a real incomplete tail, not a clean EOF.
        JournalScanFrame f;
        f.offset = offset;
        f.outcome = JournalFrameOutcome::INCOMPLETE;
        if (on_frame) on_frame(f);
      }
      break;  // clean EOF (got==0) or incomplete tail -- either way, scan ends here
    }
    uint32_t body_len = read_u32_le(len_bytes);

    std::string body;
    body.resize(body_len);
    size_t body_got = 0;
    bool have_body = body_len == 0 || read_exact(file, reinterpret_cast<uint8_t*>(&body[0]), body_len, &body_got);
    if (!have_body) {
      JournalScanFrame f;
      f.offset = offset;
      f.outcome = JournalFrameOutcome::INCOMPLETE;
      if (on_frame) on_frame(f);
      break;
    }

    uint8_t crc_bytes[4];
    if (!read_exact(file, crc_bytes, 4, nullptr)) {
      JournalScanFrame f;
      f.offset = offset;
      f.outcome = JournalFrameOutcome::INCOMPLETE;
      if (on_frame) on_frame(f);
      break;
    }
    uint32_t stored_crc = read_u32_le(crc_bytes);
    uint32_t actual_crc = crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    if (stored_crc != actual_crc) {
      JournalScanFrame f;
      f.offset = offset;
      f.outcome = JournalFrameOutcome::CRC_FAIL;
      if (on_frame) on_frame(f);
      break;  // conservative: an append-only log written by this code never has a
              // valid frame AFTER a corrupt one, so stop rather than guess past it
    }

    uint8_t marker;
    if (!read_exact(file, &marker, 1, nullptr)) {
      JournalScanFrame f;
      f.offset = offset;
      f.outcome = JournalFrameOutcome::INCOMPLETE;
      if (on_frame) on_frame(f);
      break;
    }
    if (marker != kJournalCommitMarker) {
      JournalScanFrame f;
      f.offset = offset;
      f.outcome = JournalFrameOutcome::BAD_COMMIT_MARKER;
      if (on_frame) on_frame(f);
      break;
    }

    JournalScanFrame f;
    f.offset = offset;
    f.frame_size = 4 + body_len + 4 + 1;
    f.body_json = body;
    f.outcome = JournalFrameOutcome::OK;
    if (on_frame) on_frame(f);
    ++ok_count;
    offset += f.frame_size;
  }

  file.close();
  return ok_count;
}

bool append_journal_frame(JournalFile& file, const std::string& body_json) {
  if (!file.open_for_append()) return false;

  uint8_t len_bytes[4];
  write_u32_le(len_bytes, static_cast<uint32_t>(body_json.size()));
  if (file.write(len_bytes, 4) != 4) { file.close(); return false; }
  if (!body_json.empty()) {
    if (file.write(reinterpret_cast<const uint8_t*>(body_json.data()), body_json.size()) !=
        static_cast<int>(body_json.size())) {
      file.close();
      return false;
    }
  }
  if (!file.flush()) { file.close(); return false; }

  uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(body_json.data()), body_json.size());
  uint8_t crc_bytes[4];
  write_u32_le(crc_bytes, crc);
  if (file.write(crc_bytes, 4) != 4) { file.close(); return false; }
  if (!file.flush()) { file.close(); return false; }

  uint8_t marker = kJournalCommitMarker;
  if (file.write(&marker, 1) != 1) { file.close(); return false; }
  bool ok = file.flush();
  file.close();
  return ok;
}

}  // namespace kiosk::protocol
