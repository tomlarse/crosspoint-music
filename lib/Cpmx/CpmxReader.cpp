#include "CpmxReader.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace cpmx {

namespace {

constexpr uint32_t FIXED_HEADER_SIZE = 16;  // magic..titleLen
constexpr uint32_t MAX_RECORD_CEILING = 32 * 1024;
constexpr uint16_t MAX_PRIMS_PER_LIST = 4096;
// RAM-budget ceilings (spec validation rules): reject before allocating,
// so a malicious header cannot drive large allocations on a 380KB target.
constexpr uint16_t MAX_MEASURES = 4096;
constexpr uint16_t MAX_PLAY_ORDER = 16384;
constexpr uint16_t MAX_TITLE_LEN = 256;

uint16_t readU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;  // format and both targets are little-endian
}

uint32_t readU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

int16_t readI16(const uint8_t* p) {
  int16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

/// Encoded byte size of the primitive at p, or 0 if malformed/unknown.
uint32_t primSize(const uint8_t* p, const uint8_t* end) {
  if (p >= end) {
    return 0;
  }
  const uint32_t avail = static_cast<uint32_t>(end - p);
  switch (static_cast<PrimType>(*p)) {
    case PrimType::Glyph:
      return 9 <= avail ? 9 : 0;
    case PrimType::Line:
      return 11 <= avail ? 11 : 0;
    case PrimType::Beam: {
      if (avail < 2) return 0;
      const uint32_t n = 2 + 4u * p[1];
      return n <= avail ? n : 0;
    }
    case PrimType::Curve:
    case PrimType::Polyline: {
      if (avail < 4) return 0;
      const uint32_t n = 4 + 4u * p[3];
      return n <= avail ? n : 0;
    }
    case PrimType::Dot:
      return 7 <= avail ? 7 : 0;
    case PrimType::Rect:
      return 11 <= avail ? 11 : 0;
    case PrimType::Text: {
      if (avail < 9) return 0;
      const uint32_t n = 9 + p[8];
      return n <= avail ? n : 0;
    }
  }
  return 0;  // unknown tag
}

/// Parse a PrimitiveList at *pos (validating every primitive's bounds) and
/// advance *pos past it. Returns false on malformed data.
bool parseList(const uint8_t** pos, const uint8_t* end, PrimList& out) {
  if (end - *pos < 2) {
    return false;
  }
  const uint16_t count = readU16(*pos);
  if (count > MAX_PRIMS_PER_LIST) {
    return false;
  }
  const uint8_t* p = *pos + 2;
  out.pos = p;
  out.count = count;
  for (uint16_t i = 0; i < count; i++) {
    const uint32_t size = primSize(p, end);
    if (size == 0) {
      return false;
    }
    p += size;
  }
  out.end = p;
  *pos = p;
  return true;
}

}  // namespace

void Prim::pointAt(const uint8_t i, int16_t& x, int16_t& y) const {
  x = readI16(points + 4u * i);
  y = readI16(points + 4u * i + 2);
}

bool PrimList::next(Prim& out) {
  if (count == 0 || pos >= end) {
    return false;
  }
  count--;
  const uint8_t* p = pos;
  pos += primSize(p, end);  // validated during parseList
  out.type = static_cast<PrimType>(*p);
  switch (out.type) {
    case PrimType::Glyph:
      out.codepoint = readU16(p + 1);
      out.x1 = readI16(p + 3);
      out.y1 = readI16(p + 5);
      out.scale = readU16(p + 7);
      return true;
    case PrimType::Line:
      out.x1 = readI16(p + 1);
      out.y1 = readI16(p + 3);
      out.x2 = readI16(p + 5);
      out.y2 = readI16(p + 7);
      out.w = readU16(p + 9);
      return true;
    case PrimType::Beam:
      out.pointCount = p[1];
      out.points = p + 2;
      out.w = 0;
      return true;
    case PrimType::Curve:
    case PrimType::Polyline:
      out.w = readU16(p + 1);
      out.pointCount = p[3];
      out.points = p + 4;
      return true;
    case PrimType::Dot:
      out.x1 = readI16(p + 1);
      out.y1 = readI16(p + 3);
      out.w = readU16(p + 5);  // radius
      return true;
    case PrimType::Rect:
      out.x1 = readI16(p + 1);
      out.y1 = readI16(p + 3);
      out.w = readU16(p + 5);
      out.h = readU16(p + 7);
      return true;
    case PrimType::Text:
      out.x1 = readI16(p + 1);
      out.y1 = readI16(p + 3);
      out.w = readU16(p + 5);  // font size
      out.textFlags = p[7];
      out.textLen = p[8];
      out.text = reinterpret_cast<const char*>(p + 9);
      return true;
  }
  return false;
}

bool CpmxReader::open(const char* path) {
  close();
  if (!openInternal(path)) {
    close();  // release the file handle and any partial allocations
    return false;
  }
  open_ = true;
  LOG_INF("CPMX", "Opened '%s': %u measures, %u blocks, playOrder %u, maxRecord %u B", title_.get(), measureCount_,
          headerBlockCount_, playOrderLen_, maxRecordSize_);
  return true;
}

bool CpmxReader::openInternal(const char* path) {
  if (!Storage.openFileForRead("CPMX", path, file_)) {
    LOG_ERR("CPMX", "Cannot open %s", path);
    return false;
  }
  fileSize_ = file_.size();

  uint8_t header[FIXED_HEADER_SIZE];
  if (file_.read(header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("CPMX", "Truncated header");
    return false;
  }
  if (memcmp(header, "CPMX", 4) != 0) {
    LOG_ERR("CPMX", "Bad magic");
    return false;
  }
  if (header[4] != FORMAT_VERSION) {
    LOG_ERR("CPMX", "Unsupported version %u", header[4]);
    return false;
  }
  if (header[5] != 0) {
    LOG_ERR("CPMX", "Nonzero reserved flags 0x%02x", header[5]);
    return false;
  }
  measureCount_ = readU16(header + 6);
  playOrderLen_ = readU16(header + 8);
  const uint16_t blockCount = readU16(header + 10);
  // header + 12: unitsPerStaffSpace — converter-side glyph scale reference,
  // unused on device (glyphs are pre-rasterized per size class).
  const uint16_t titleLen = readU16(header + 14);
  if (measureCount_ == 0 || measureCount_ > MAX_MEASURES || blockCount == 0 || blockCount > 255 ||
      playOrderLen_ > MAX_PLAY_ORDER || titleLen > MAX_TITLE_LEN) {
    LOG_ERR("CPMX", "Invalid counts: %u measures, %u blocks, playOrder %u, title %u", measureCount_, blockCount,
            playOrderLen_, titleLen);
    return false;
  }
  headerBlockCount_ = static_cast<uint8_t>(blockCount);

  title_ = makeUniqueNoThrow<char[]>(titleLen + 1u);
  if (!title_) {
    LOG_ERR("CPMX", "OOM: title (%u bytes)", titleLen + 1);
    return false;
  }
  if (file_.read(title_.get(), titleLen) != static_cast<int>(titleLen)) {
    LOG_ERR("CPMX", "Truncated title");
    return false;
  }
  title_[titleLen] = '\0';

  if (playOrderLen_ > 0) {
    const uint32_t bytes = 2u * playOrderLen_;
    playOrder_ = makeUniqueNoThrow<uint8_t[]>(bytes);
    if (!playOrder_) {
      LOG_ERR("CPMX", "OOM: playOrder (%u bytes)", bytes);
      return false;
    }
    if (file_.read(playOrder_.get(), bytes) != static_cast<int>(bytes)) {
      LOG_ERR("CPMX", "Truncated playOrder");
      return false;
    }
    for (uint16_t i = 0; i < playOrderLen_; i++) {
      if (readU16(playOrder_.get() + 2u * i) >= measureCount_) {
        LOG_ERR("CPMX", "playOrder[%u] out of range", i);
        return false;
      }
    }
  }

  const uint32_t tableCount = static_cast<uint32_t>(headerBlockCount_) + measureCount_;
  const uint32_t tableBytes = 4u * tableCount;
  offsets_ = makeUniqueNoThrow<uint8_t[]>(tableBytes);
  if (!offsets_) {
    LOG_ERR("CPMX", "OOM: offset table (%u bytes)", tableBytes);
    return false;
  }
  if (file_.read(offsets_.get(), tableBytes) != static_cast<int>(tableBytes)) {
    LOG_ERR("CPMX", "Truncated offset table");
    return false;
  }

  // Offsets must ascend strictly and stay inside the file; a record is
  // bounded by the next offset (EOF for the last). Derive the largest
  // record so one buffer serves every load.
  uint32_t prev = FIXED_HEADER_SIZE + titleLen + 2u * playOrderLen_ + tableBytes - 1;
  maxRecordSize_ = 0;
  for (uint32_t i = 0; i < tableCount; i++) {
    const uint32_t off = readU32(offsets_.get() + 4u * i);
    if (off <= prev || off >= fileSize_) {
      LOG_ERR("CPMX", "Offset table corrupt at %u", i);
      return false;
    }
    const uint32_t next = (i + 1 < tableCount) ? readU32(offsets_.get() + 4u * (i + 1)) : fileSize_;
    if (next <= off) {
      LOG_ERR("CPMX", "Offset table corrupt at %u", i + 1);
      return false;
    }
    if (next - off > maxRecordSize_) {
      maxRecordSize_ = next - off;
    }
    prev = off;
  }
  if (maxRecordSize_ > MAX_RECORD_CEILING) {
    LOG_ERR("CPMX", "Record of %u bytes exceeds ceiling", maxRecordSize_);
    return false;
  }
  record_ = makeUniqueNoThrow<uint8_t[]>(maxRecordSize_);
  if (!record_) {
    LOG_ERR("CPMX", "OOM: record buffer (%u bytes)", maxRecordSize_);
    return false;
  }

  return true;
}

void CpmxReader::close() {
  if (file_) {
    file_.close();
  }
  open_ = false;
  title_.reset();
  playOrder_.reset();
  offsets_.reset();
  record_.reset();
  measureCount_ = 0;
  playOrderLen_ = 0;
  headerBlockCount_ = 0;
}

uint16_t CpmxReader::playOrderAt(const uint16_t pos) const {
  if (playOrderLen_ == 0) {
    return pos < measureCount_ ? pos : 0;
  }
  if (pos >= playOrderLen_) {
    return 0;
  }
  return readU16(playOrder_.get() + 2u * pos);
}

uint32_t CpmxReader::recordOffset(const uint32_t tableIndex) const { return readU32(offsets_.get() + 4u * tableIndex); }

bool CpmxReader::readRecord(const uint32_t tableIndex, uint32_t& lengthOut) {
  const uint32_t off = recordOffset(tableIndex);
  const uint32_t tableCount = static_cast<uint32_t>(headerBlockCount_) + measureCount_;
  const uint32_t next = (tableIndex + 1 < tableCount) ? recordOffset(tableIndex + 1) : fileSize_;
  lengthOut = next - off;
  if (!file_.seek(off)) {
    LOG_ERR("CPMX", "Seek to %u failed", off);
    return false;
  }
  if (file_.read(record_.get(), lengthOut) != static_cast<int>(lengthOut)) {
    LOG_ERR("CPMX", "Short read at %u", off);
    return false;
  }
  return true;
}

bool CpmxReader::loadMeasure(const uint16_t index, MeasureView& out) {
  if (!open_ || index >= measureCount_) {
    return false;
  }
  uint32_t length = 0;
  if (!readRecord(headerBlockCount_ + index, length)) {
    return false;
  }
  const uint8_t* p = record_.get();
  const uint8_t* end = p + length;
  if (length < 8) {
    LOG_ERR("CPMX", "Measure %u record too short", index);
    return false;
  }
  out.widthFp = readU16(p);
  out.yMinFp = readI16(p + 2);
  out.yMaxFp = readI16(p + 4);
  out.headerIdx = p[6];
  out.flags = p[7];
  if (out.headerIdx >= headerBlockCount_) {
    LOG_ERR("CPMX", "Measure %u headerIdx %u out of range", index, out.headerIdx);
    return false;
  }
  if (out.flags & ~(MEASURE_FLAG_SPLIT_START | MEASURE_FLAG_SPLIT_END)) {
    LOG_ERR("CPMX", "Measure %u unknown flags 0x%02x", index, out.flags);
    return false;
  }
  p += 8;
  out.splitStart = PrimList{};
  out.splitEnd = PrimList{};
  if (!parseList(&p, end, out.prims)) {
    LOG_ERR("CPMX", "Measure %u malformed primitive list", index);
    return false;
  }
  if ((out.flags & MEASURE_FLAG_SPLIT_START) && !parseList(&p, end, out.splitStart)) {
    LOG_ERR("CPMX", "Measure %u malformed splitAtStart list", index);
    return false;
  }
  if ((out.flags & MEASURE_FLAG_SPLIT_END) && !parseList(&p, end, out.splitEnd)) {
    LOG_ERR("CPMX", "Measure %u malformed splitAtEnd list", index);
    return false;
  }
  return true;
}

bool CpmxReader::loadHeaderBlock(const uint8_t index, HeaderBlockView& out) {
  if (!open_ || index >= headerBlockCount_) {
    return false;
  }
  uint32_t length = 0;
  if (!readRecord(index, length)) {
    return false;
  }
  const uint8_t* p = record_.get();
  const uint8_t* end = p + length;
  if (length < 2) {
    LOG_ERR("CPMX", "Header block %u record too short", index);
    return false;
  }
  out.advanceFp = readU16(p);
  p += 2;
  if (!parseList(&p, end, out.prims)) {
    LOG_ERR("CPMX", "Header block %u malformed primitive list", index);
    return false;
  }
  return true;
}

}  // namespace cpmx
