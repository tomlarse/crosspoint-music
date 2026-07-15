#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

/// Streaming reader for the .cpmx v1 sheet-music format.
/// Spec + validation contract: docs/music/cpmx-format-draft.md.
/// Reference implementation: converter/cpmx_render.py.
///
/// Memory model: the header tables (playOrder, offsets, title) are read once
/// on open (a few hundred bytes for a full march). Measure records are read
/// one at a time into a single reusable buffer sized to the largest record
/// in the file — the whole score never sits in RAM.
namespace cpmx {

constexpr uint8_t FORMAT_VERSION = 1;
/// Coordinates are staff spaces in i16 fixed point, 1/64 staff space.
constexpr int COORD_FP_SHIFT = 6;
/// Glyph scale fixed point (1/1024).
constexpr int SCALE_FP = 1024;

enum class PrimType : uint8_t {
  Glyph = 1,
  Line = 2,
  Beam = 3,
  Curve = 4,
  Polyline = 5,
  Dot = 6,
  Rect = 7,
  Text = 8,
};

constexpr uint8_t MEASURE_FLAG_SPLIT_START = 0x01;
constexpr uint8_t MEASURE_FLAG_SPLIT_END = 0x02;
constexpr uint8_t TEXT_FLAG_BOLD = 0x01;

/// Decoded view of one primitive. Point/text payloads stay in the record
/// buffer; accessors memcpy them out (RISC-V alignment — never cast).
struct Prim {
  PrimType type;
  // Glyph
  uint16_t codepoint;
  uint16_t scale;
  // Line endpoints / generic position (fixed-point staff spaces)
  int16_t x1, y1, x2, y2;
  uint16_t w, h;  // thickness or dimensions / text size
  // Beam/Curve/Polyline points
  uint8_t pointCount;
  const uint8_t* points;  // pointCount * (i16 x, i16 y), packed LE
  // Text
  uint8_t textFlags;
  uint8_t textLen;
  const char* text;  // NOT null-terminated; textLen bytes

  void pointAt(uint8_t i, int16_t& x, int16_t& y) const;
};

/// One primitive list inside the record buffer.
struct PrimList {
  const uint8_t* pos = nullptr;
  const uint8_t* end = nullptr;
  uint16_t count = 0;

  /// Decode the next primitive. Returns false at the end of the list or on
  /// a malformed record (distinguish via `count`, which reaches 0 on clean
  /// exhaustion).
  bool next(Prim& out);
};

struct MeasureView {
  uint16_t widthFp = 0;
  int16_t yMinFp = 0, yMaxFp = 0;
  uint8_t headerIdx = 0;
  uint8_t flags = 0;
  PrimList prims;       // default content
  PrimList splitStart;  // continuation halves (flags bit0)
  PrimList splitEnd;    // departing halves (flags bit1)
};

struct HeaderBlockView {
  uint16_t advanceFp = 0;
  PrimList prims;
};

class CpmxReader {
 public:
  CpmxReader() = default;
  ~CpmxReader() { close(); }
  CpmxReader(const CpmxReader&) = delete;
  CpmxReader& operator=(const CpmxReader&) = delete;

  /// Open and validate the file header. On any validation failure logs and
  /// returns false; the reader is then unusable until the next open().
  bool open(const char* path);
  void close();
  bool isOpen() const { return open_; }

  uint16_t measureCount() const { return measureCount_; }
  const char* title() const { return title_ ? title_.get() : ""; }

  /// Performance sequence length (repeats unrolled). Falls back to
  /// measureCount when the file has no explicit playOrder.
  uint16_t playOrderLength() const { return playOrderLen_ ? playOrderLen_ : measureCount_; }
  /// Measure index at a position in the performance sequence.
  uint16_t playOrderAt(uint16_t pos) const;

  /// Read one measure record into the shared buffer and parse it. The view
  /// (and any Prim decoded from it) is valid until the next load call.
  bool loadMeasure(uint16_t index, MeasureView& out);
  /// Same for a system header block (drawn at continuation-line starts).
  bool loadHeaderBlock(uint8_t index, HeaderBlockView& out);

 private:
  bool readRecord(uint32_t tableIndex, uint32_t& lengthOut);
  uint32_t recordOffset(uint32_t tableIndex) const;

  HalFile file_;
  bool open_ = false;
  uint16_t measureCount_ = 0;
  uint16_t playOrderLen_ = 0;
  uint8_t headerBlockCount_ = 0;
  uint32_t fileSize_ = 0;
  uint32_t maxRecordSize_ = 0;
  std::unique_ptr<char[]> title_;
  std::unique_ptr<uint8_t[]> playOrder_;  // u16 LE * playOrderLen_
  std::unique_ptr<uint8_t[]> offsets_;    // u32 LE * (blocks + measures)
  std::unique_ptr<uint8_t[]> record_;     // reusable record buffer
};

}  // namespace cpmx
