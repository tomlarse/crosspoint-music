#include "MusicReaderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

#ifdef SIMULATOR
#include <cstdlib>
#endif

namespace {

constexpr int MARGIN_PX = 8;
/// Curves reaching past width + 0.15 ss cross into the next measure
/// (format layout contract, rule 3).
constexpr int32_t CROSSING_SLACK_FP = 10;  // 0.15 ss in 1/64 units
constexpr uint8_t MAX_POLY_POINTS = 255;

/// Vertical layout (viewer policy per the spec): staff top sits this many
/// staff spaces below the system origin, and systems advance by this pitch.
constexpr int SYSTEM_TOP_SS = 4;
constexpr int SYSTEM_PITCH_SS = 12;

void encodeUtf8(const uint16_t codepoint, char out[4]) {
  // SMuFL codepoints live in the BMP PUA (U+E000..): always 3 UTF-8 bytes.
  out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
  out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
  out[3] = '\0';
}

bool curveCrossesRightEdge(const cpmx::Prim& prim, const uint16_t widthFp) {
  if (prim.type != cpmx::PrimType::Curve) {
    return false;
  }
  for (uint8_t i = 0; i < prim.pointCount; i++) {
    int16_t x, y;
    prim.pointAt(i, x, y);
    if (x > static_cast<int32_t>(widthFp) + CROSSING_SLACK_FP) {
      return true;
    }
  }
  return false;
}

}  // namespace

MusicReaderActivity::MusicReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
    : Activity("MusicReader", renderer, mappedInput), filePath(std::move(filePath)) {}

void MusicReaderActivity::onEnter() {
  Activity::onEnter();

  polyX_ = makeUniqueNoThrow<int[]>(MAX_POLY_POINTS);
  polyY_ = makeUniqueNoThrow<int[]>(MAX_POLY_POINTS);
  if (!polyX_ || !polyY_) {
    LOG_ERR("MUSIC", "OOM: polygon scratch");
    showError();
    return;
  }

  if (!reader.open(filePath.c_str())) {
    showError();
    return;
  }

  // Loaded successfully: register as the open book (boot resume + recents).
  const auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  musicFontAscender_ = renderer.getFontAscenderSize(MUSIC_23_FONT_ID);
  pagePos_ = 0;
  previousPageCount_ = 0;
  // TODO(progress): persist/restore the playOrder position like the text
  // readers do (reading progress = position in playOrder, per the spec).
  if (!layoutPage(pagePos_)) {
    showError();
    return;
  }
  renderPage();
}

void MusicReaderActivity::onExit() {
  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  reader.close();
  polyX_.reset();
  polyY_.reset();
  Activity::onExit();
}

void MusicReaderActivity::showError() {
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  renderer.displayBuffer();
}

bool MusicReaderActivity::layoutPage(const uint16_t startPos) {
  systemCount_ = 0;
  const int usableWidth = renderer.getScreenWidth() - 2 * MARGIN_PX;
  const int pageHeight = renderer.getScreenHeight();
  const int pitch = SYSTEM_PITCH_SS * staffSpacePx_;
  const uint16_t playLen = reader.playOrderLength();

  int y = MARGIN_PX;
  uint16_t pos = startPos;
  cpmx::MeasureView measure;
  cpmx::HeaderBlockView block;

  while (pos < playLen && y + pitch <= pageHeight - MARGIN_PX && systemCount_ < MAX_SYSTEMS_PER_PAGE) {
    // Continuation lines start with the clef/key header block.
    int x = 0;
    if (pos != 0) {
      if (!reader.loadMeasure(reader.playOrderAt(pos), measure) || !reader.loadHeaderBlock(measure.headerIdx, block)) {
        return false;
      }
      x = fpToPx(block.advanceFp);
    }
    uint16_t count = 0;
    while (pos + count < playLen) {
      if (!reader.loadMeasure(reader.playOrderAt(pos + count), measure)) {
        return false;
      }
      const int widthPx = fpToPx(measure.widthFp);
      // Never split a measure: an oversized one gets the line to itself
      // and overflows (format layout contract, rule 5).
      if (count > 0 && x + widthPx > usableWidth) {
        break;
      }
      x += widthPx;
      count++;
    }
    systems_[systemCount_++] = {pos, count};
    pos += count;
    y += pitch;
  }
  nextPagePos_ = pos;
  return systemCount_ > 0;
}

void MusicReaderActivity::renderPage() {
  renderer.clearScreen();
  for (size_t i = 0; i < systemCount_; i++) {
    const int oy = MARGIN_PX + SYSTEM_TOP_SS * staffSpacePx_ + static_cast<int>(i) * SYSTEM_PITCH_SS * staffSpacePx_;
    drawSystem(systems_[i], oy);
  }
  renderer.displayBuffer();
#ifdef SIMULATOR
  // Desktop verification hook: dump each rendered page to /screenshots/.
  if (std::getenv("CROSSPOINT_MUSIC_SHOT") != nullptr) {
    ScreenshotUtil::takeScreenshot(renderer);
  }
#endif
}

void MusicReaderActivity::drawSystem(const SystemLayout& system, const int oy) {
  const uint16_t playLen = reader.playOrderLength();
  int x = MARGIN_PX;
  cpmx::MeasureView measure;

  if (system.startPos != 0) {
    // Draw the header block first: it shares the record buffer with
    // measures, so its primitives must be consumed before the next load.
    if (!reader.loadMeasure(reader.playOrderAt(system.startPos), measure)) {
      return;
    }
    cpmx::HeaderBlockView block;
    if (!reader.loadHeaderBlock(measure.headerIdx, block)) {
      return;
    }
    const int advancePx = fpToPx(block.advanceFp);
    drawStaffLines(x, oy, advancePx);
    cpmx::Prim prim;
    while (block.prims.next(prim)) {
      drawPrim(prim, x, oy);
    }
    x += advancePx;
  }

  for (uint16_t j = 0; j < system.count; j++) {
    const uint16_t pos = system.startPos + j;
    const uint16_t idx = reader.playOrderAt(pos);
    if (!reader.loadMeasure(idx, measure)) {
      return;
    }
    // Split curves only connect measures that are score-adjacent. At a
    // repeat/volta jump the neighbour in playOrder is a different measure,
    // so a crossing curve must be stubbed (splitAtEnd) even mid-line, and
    // no continuation half may be drawn after the jump.
    const bool prevAdjacent = pos > 0 && reader.playOrderAt(pos - 1) + 1 == idx;
    const bool nextAdjacent = pos + 1 < playLen && reader.playOrderAt(pos + 1) == idx + 1;
    const bool lineBreakAfter = (j == system.count - 1) && pos + 1 < playLen;
    const bool brokenBefore = (j == 0) && prevAdjacent;
    const bool brokenAfter = (pos + 1 < playLen) && (lineBreakAfter || !nextAdjacent);
    drawMeasure(measure, x, oy, brokenBefore, brokenAfter);
    x += fpToPx(measure.widthFp);
  }
}

void MusicReaderActivity::drawMeasure(const cpmx::MeasureView& measure, const int ox, const int oy,
                                      const bool brokenBefore, const bool brokenAfter) {
  drawStaffLines(ox, oy, fpToPx(measure.widthFp));
  // At a line break, whole curves that reach past the barline are replaced
  // by their pre-engraved split halves (format layout contract, rule 3).
  const bool substitute = brokenAfter && measure.splitEnd.count > 0;
  drawPrimList(measure.prims, ox, oy, substitute, measure.widthFp);
  if (substitute) {
    drawPrimList(measure.splitEnd, ox, oy, false, 0);
  }
  if (brokenBefore) {
    drawPrimList(measure.splitStart, ox, oy, false, 0);
  }
}

void MusicReaderActivity::drawPrimList(cpmx::PrimList list, const int ox, const int oy, const bool skipCrossing,
                                       const uint16_t widthFp) {
  cpmx::Prim prim;
  while (list.next(prim)) {
    if (skipCrossing && curveCrossesRightEdge(prim, widthFp)) {
      continue;
    }
    drawPrim(prim, ox, oy);
  }
}

void MusicReaderActivity::drawStaffLines(const int ox, const int oy, const int widthPx) {
  const int thickness = staffSpacePx_ >= 14 ? 2 : 1;
  for (int line = 0; line < 5; line++) {
    const int y = oy + line * staffSpacePx_;
    renderer.drawLine(ox, y, ox + widthPx, y, thickness, true);
  }
}

void MusicReaderActivity::drawPrim(const cpmx::Prim& prim, const int ox, const int oy) {
  switch (prim.type) {
    case cpmx::PrimType::Glyph: {
      char utf8[4];
      encodeUtf8(prim.codepoint, utf8);
      // drawText's y is the top of the line box (it adds the ascender);
      // glyph anchors in .cpmx are baseline positions.
      renderer.drawText(MUSIC_23_FONT_ID, ox + fpToPx(prim.x1), oy + fpToPx(prim.y1) - musicFontAscender_, utf8, true);
      return;
    }
    case cpmx::PrimType::Line: {
      const int w = fpToPx(prim.w);
      renderer.drawLine(ox + fpToPx(prim.x1), oy + fpToPx(prim.y1), ox + fpToPx(prim.x2), oy + fpToPx(prim.y2),
                        w > 0 ? w : 1, true);
      return;
    }
    case cpmx::PrimType::Beam:
    case cpmx::PrimType::Curve: {
      const uint8_t n = prim.pointCount;
      if (n < 3) {
        return;
      }
      for (uint8_t i = 0; i < n; i++) {
        int16_t x, y;
        prim.pointAt(i, x, y);
        polyX_[i] = ox + fpToPx(x);
        polyY_[i] = oy + fpToPx(y);
      }
      renderer.fillPolygon(polyX_.get(), polyY_.get(), n, true);
      return;
    }
    case cpmx::PrimType::Polyline: {
      if (prim.pointCount == 0) {
        return;
      }
      const int w = fpToPx(prim.w);
      int16_t x1, y1;
      prim.pointAt(0, x1, y1);
      for (uint8_t i = 1; i < prim.pointCount; i++) {
        int16_t x2, y2;
        prim.pointAt(i, x2, y2);
        renderer.drawLine(ox + fpToPx(x1), oy + fpToPx(y1), ox + fpToPx(x2), oy + fpToPx(y2), w > 0 ? w : 1, true);
        x1 = x2;
        y1 = y2;
      }
      return;
    }
    case cpmx::PrimType::Dot: {
      const int r = fpToPx(prim.w) > 0 ? fpToPx(prim.w) : 1;
      renderer.fillRect(ox + fpToPx(prim.x1) - r, oy + fpToPx(prim.y1) - r, 2 * r, 2 * r, true);
      return;
    }
    case cpmx::PrimType::Rect:
      renderer.fillRect(ox + fpToPx(prim.x1), oy + fpToPx(prim.y1), fpToPx(prim.w), fpToPx(prim.h), true);
      return;
    case cpmx::PrimType::Text: {
      // Volta numbers etc. — short labels; render with the UI font.
      char buf[64];
      const size_t len = prim.textLen < sizeof(buf) - 1 ? prim.textLen : sizeof(buf) - 1;
      memcpy(buf, prim.text, len);
      buf[len] = '\0';
      const auto style = (prim.textFlags & cpmx::TEXT_FLAG_BOLD) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      renderer.drawText(UI_10_FONT_ID, ox + fpToPx(prim.x1),
                        oy + fpToPx(prim.y1) - renderer.getFontAscenderSize(UI_10_FONT_ID), buf, true, style);
      return;
    }
  }
}

void MusicReaderActivity::pageForward() {
  if (nextPagePos_ >= reader.playOrderLength() || previousPageCount_ >= MAX_PAGE_HISTORY) {
    return;  // last page (or history exhausted — pathological page counts)
  }
  const uint16_t newPos = nextPagePos_;
  if (!layoutPage(newPos)) {
    LOG_ERR("MUSIC", "Layout failed at position %u", newPos);
    layoutPage(pagePos_);  // restore the current page's layout state
    return;
  }
  previousPages_[previousPageCount_++] = pagePos_;
  pagePos_ = newPos;
  renderPage();
}

void MusicReaderActivity::pageBack() {
  if (previousPageCount_ == 0) {
    return;
  }
  const uint16_t newPos = previousPages_[previousPageCount_ - 1];
  if (!layoutPage(newPos)) {
    LOG_ERR("MUSIC", "Layout failed at position %u", newPos);
    layoutPage(pagePos_);
    return;
  }
  previousPageCount_--;
  pagePos_ = newPos;
  renderPage();
}

void MusicReaderActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    pageForward();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    pageBack();
    return;
  }
}
