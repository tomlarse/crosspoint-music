#pragma once

#include <CpmxReader.h>

#include <memory>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/// Sheet-music viewer for .cpmx files (see docs/music/cpmx-format-draft.md).
///
/// Layout per the format's firmware contract: measures are packed into
/// systems ("word wrap" with measures as words), systems into pages. Page
/// turns follow the file's playOrder — repeats and endings ("hus") are
/// pre-unrolled by the converter, so paging forward always shows what the
/// player performs next.
class MusicReaderActivity final : public Activity {
 public:
  MusicReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct SystemLayout {
    uint16_t startPos;  // playOrder position of the system's first measure
    uint16_t count;     // measures on this system
  };

  bool layoutPage(uint16_t startPos);
  void renderPage();
  void drawSystem(const SystemLayout& system, int oy);
  void drawMeasure(const cpmx::MeasureView& measure, int ox, int oy, bool brokenBefore, bool brokenAfter);
  void drawPrimList(cpmx::PrimList list, int ox, int oy, bool skipCrossing, uint16_t widthFp);
  void drawPrim(const cpmx::Prim& prim, int ox, int oy);
  void drawStaffLines(int ox, int oy, int widthPx);
  void showError();

  void pageForward();
  void pageBack();

  /// Fixed-point staff spaces (1/64) -> screen px at the current staff size.
  int fpToPx(int32_t fp) const { return static_cast<int>((fp * staffSpacePx_) >> cpmx::COORD_FP_SHIFT); }

  std::string filePath;
  cpmx::CpmxReader reader;
  int staffSpacePx_ = 12;
  int musicFontAscender_ = 0;
  uint16_t pagePos_ = 0;      // playOrder position at the top of this page
  uint16_t nextPagePos_ = 0;  // where the next page starts
  std::vector<uint16_t> previousPages_;
  std::vector<SystemLayout> systems_;
  // fillPolygon scratch for curve/beam points (heap: 2 KB would be too much
  // stack; allocated once in onEnter, freed automatically on destruction)
  std::unique_ptr<int[]> polyX_;
  std::unique_ptr<int[]> polyY_;
};
