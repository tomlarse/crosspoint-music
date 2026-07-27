#include "MusicReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ProgressFile.h"
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

/// Vertical layout (viewer policy per the spec): systems are placed from
/// their actual per-measure yMin/yMax extents so tall content (volta
/// brackets, slurs, dynamics) neither clips at the page top nor collides
/// with the neighbouring system. Extents are glyph-anchor based, so pad.
constexpr int SYSTEM_PAD_SS = 1;             // headroom beyond yMin/yMax, each side
constexpr int SYSTEM_GAP_SS = 2;             // air between systems
constexpr int32_t MIN_Y_FLOOR_FP = -2 * 64;  // at least 2 ss above the staff
constexpr int32_t MAX_Y_FLOOR_FP = 6 * 64;   // at least 2 ss below it

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

  setlistMode_ = FsHelpers::hasSetlistExtension(filePath);
  if (setlistMode_ && !setlist_.load(filePath)) {
    showError();
    return;
  }

  // Per-piece progress lives beside the other readers' caches. Setlists get
  // none: a march order is performed from the top, so every open starts at
  // the first piece — nothing is read or written for the .cpsl itself.
  if (!setlistMode_) {
    cachePath = std::string("/.crosspoint/cpmx_") + std::to_string(std::hash<std::string>{}(filePath));
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(cachePath.c_str());
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  applyStaffSize();

  uint16_t startPos = 0;
  if (!setlistMode_) {
    loadProgress(startPos);
  }
  if (!openPiece(0)) {
    showError();
    return;
  }

  // Opened successfully: register as the open book (boot resume + recents).
  const auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  if (startPos > 0 && startPos < reader.playOrderLength()) {
    restoreToPosition(startPos);
  }
  if (!layoutPage(pagePos_)) {
    showError();
    return;
  }
  if (pagePos_ == 0) {
    showCover();  // opening at the top: start on the piece's cover
  } else {
    renderPage();  // resuming mid-piece: straight back to the music
  }
}

// Open one piece (the single file, or setlist entry `index`) and reset the
// per-piece paging state.
bool MusicReaderActivity::openPiece(const size_t index) {
  setlistIdx_ = index;
  const std::string& path = setlistMode_ ? setlist_.pieceAt(index) : filePath;
  if (!reader.open(path.c_str())) {
    return false;
  }
  pagePos_ = 0;
  nextPagePos_ = 0;
  previousPageCount_ = 0;
  atEnd_ = false;
  atCover_ = false;
  return true;
}

void MusicReaderActivity::goToNextPiece() {
  const size_t fromIdx = setlistIdx_;
  const uint16_t fromPos = pagePos_;
  if (!openPiece(setlistIdx_ + 1) || !layoutPage(0)) {
    recoverPiece(fromIdx, fromPos);
    return;
  }
  showCover();  // the cover is the boundary between setlist pieces
}

void MusicReaderActivity::goToPreviousPieceEnd() {
  const size_t fromIdx = setlistIdx_;
  const uint16_t fromPos = pagePos_;
  if (!openPiece(setlistIdx_ - 1)) {
    recoverPiece(fromIdx, fromPos);
    return;
  }
  const uint16_t playLen = reader.playOrderLength();
  restoreToPosition(playLen > 0 ? playLen - 1 : 0);
  if (!layoutPage(pagePos_)) {
    recoverPiece(fromIdx, fromPos);
    return;
  }
  renderPage();
}

// A neighbouring setlist piece failed to open (removed or corrupt since the
// setlist was written). openPiece() has already closed the current file, so
// reopen the piece the user was on and stay there instead of erroring out.
void MusicReaderActivity::recoverPiece(const size_t index, const uint16_t position) {
  LOG_ERR("MUSIC", "Setlist piece unopenable, staying on piece %u", static_cast<unsigned>(index));
  autoArmed_ = false;
  if (!openPiece(index)) {
    showError();
    return;
  }
  restoreToPosition(position);
  if (!layoutPage(pagePos_)) {
    showError();
    return;
  }
  renderPage();
}

void MusicReaderActivity::applyStaffSize() {
  switch (SETTINGS.musicStaffSize) {
    case CrossPointSettings::MUSIC_SIZE_XXS:
      staffSpacePx_ = 5;
      musicFontId_ = MUSIC_10_FONT_ID;
      break;
    case CrossPointSettings::MUSIC_SIZE_XS:
      staffSpacePx_ = 6;
      musicFontId_ = MUSIC_12_FONT_ID;
      break;
    case CrossPointSettings::MUSIC_SIZE_S:
      staffSpacePx_ = 8;
      musicFontId_ = MUSIC_15_FONT_ID;
      break;
    case CrossPointSettings::MUSIC_SIZE_L:
      staffSpacePx_ = 12;
      musicFontId_ = MUSIC_23_FONT_ID;
      break;
    case CrossPointSettings::MUSIC_SIZE_XL:
      staffSpacePx_ = 16;
      musicFontId_ = MUSIC_31_FONT_ID;
      break;
    case CrossPointSettings::MUSIC_SIZE_M:
    default:
      staffSpacePx_ = 10;
      musicFontId_ = MUSIC_19_FONT_ID;
      break;
  }
  musicFontAscender_ = renderer.getFontAscenderSize(musicFontId_);
}

// Read saved progress without applying it (the piece is not open yet).
// Piece records: 'M',1,pos. Setlists deliberately have no saved progress.
void MusicReaderActivity::loadProgress(uint16_t& positionOut) {
  positionOut = 0;
  HalFile f;
  if (!Storage.openFileForRead("MUSIC", cachePath + "/progress.bin", f)) {
    return;  // no saved progress
  }
  uint8_t data[4];
  if (f.read(data, sizeof(data)) != sizeof(data) || data[0] != 'M' || data[1] != 1) {
    return;
  }
  positionOut = static_cast<uint16_t>(data[2] | (data[3] << 8));
}

void MusicReaderActivity::saveProgress() {
  if (!reader.isOpen() || setlistMode_) {
    return;  // setlists start from the top every time — nothing to save
  }
  const uint8_t data[4] = {'M', 1, static_cast<uint8_t>(pagePos_ & 0xFF), static_cast<uint8_t>(pagePos_ >> 8)};
  ProgressFile::writeAtomic(cachePath, data, sizeof(data));
  pageTurnsSinceSave_ = 0;
}

void MusicReaderActivity::restoreToPosition(const uint16_t targetPos) {
  // Page boundaries are deterministic from position 0; walk forward until
  // the page containing the target, rebuilding the back-navigation stack.
  uint16_t pos = 0;
  while (pos < targetPos && previousPageCount_ < MAX_PAGE_HISTORY) {
    if (!layoutPage(pos) || nextPagePos_ <= pos) {
      return;  // corrupt layout state: fall back to the first page
    }
    if (nextPagePos_ > targetPos) {
      break;  // target lives on the page starting at pos
    }
    previousPages_[previousPageCount_++] = pos;
    pos = nextPagePos_;
  }
  pagePos_ = pos;
}

void MusicReaderActivity::onExit() {
  saveProgress();
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
  pageBeatsX8_ = 0;
  const int usableWidth = renderer.getScreenWidth() - 2 * MARGIN_PX;
  const int pageHeight = renderer.getScreenHeight();
  const int padPx = SYSTEM_PAD_SS * staffSpacePx_;
  const uint16_t playLen = reader.playOrderLength();

  int y = MARGIN_PX;
  uint16_t pos = startPos;
  cpmx::MeasureView measure;
  cpmx::HeaderBlockView block;

  while (pos < playLen && systemCount_ < MAX_SYSTEMS_PER_PAGE) {
    // Continuation lines start with the clef/key header block.
    int x = 0;
    if (pos != 0) {
      if (!reader.loadMeasure(reader.playOrderAt(pos), measure) || !reader.loadHeaderBlock(measure.headerIdx, block)) {
        return false;
      }
      x = fpToPx(block.advanceFp);
    }
    uint16_t count = 0;
    uint32_t systemBeatsX8 = 0;
    int32_t minFp = MIN_Y_FLOOR_FP;
    int32_t maxFp = MAX_Y_FLOOR_FP;
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
      systemBeatsX8 += measure.beatsX8;
      if (measure.yMinFp < minFp) {
        minFp = measure.yMinFp;
      }
      if (measure.yMaxFp > maxFp) {
        maxFp = measure.yMaxFp;
      }
    }
    const int staffTopY = y + padPx + fpToPx(-minFp);
    const int bottomY = staffTopY + fpToPx(maxFp) + padPx;
    // Page full? This system goes on the next page (but never orphan the
    // first system of a page, or an over-tall one would loop forever).
    if (systemCount_ > 0 && bottomY > pageHeight - MARGIN_PX) {
      break;
    }
    systems_[systemCount_++] = {pos, count, static_cast<int16_t>(staffTopY)};
    pageBeatsX8_ += systemBeatsX8;
    pos += count;
    y = bottomY + SYSTEM_GAP_SS * staffSpacePx_;
  }
  nextPagePos_ = pos;
  return systemCount_ > 0;
}

// Beats on the page x tempo -> a millis() deadline for the automatic turn.
// The turn fires AUTO_PAGE_LEAD_BEATS_X8 early: the player has already read
// the last measure when playing it, and the e-ink refresh finishes before
// the music runs out.
void MusicReaderActivity::armAutoPage() {
  constexpr uint32_t AUTO_PAGE_LEAD_BEATS_X8 = 16;  // 2 metronome beats
  if (!SETTINGS.musicAutoPage || SETTINGS.musicTempoBpm == 0 || pageBeatsX8_ == 0) {
    autoArmed_ = false;
    return;
  }
  const uint32_t beatsX8 =
      pageBeatsX8_ > AUTO_PAGE_LEAD_BEATS_X8 ? pageBeatsX8_ - AUTO_PAGE_LEAD_BEATS_X8 : pageBeatsX8_ / 2;
  // ms = (beatsX8 / 8) * 60000 / bpm = beatsX8 * 7500 / bpm. 64-bit and
  // clamped: a pathological file could overflow 32 bits (7500 * beatsX8).
  constexpr uint32_t MAX_AUTO_MS = 60u * 60u * 1000u;  // one hour
  const uint64_t ms64 = static_cast<uint64_t>(beatsX8) * 7500u / SETTINGS.musicTempoBpm;
  const uint32_t ms = ms64 > MAX_AUTO_MS ? MAX_AUTO_MS : static_cast<uint32_t>(ms64);
  LOG_DBG("MUSIC", "Auto page armed: %u beatsX8 on page, turn in %u ms", static_cast<unsigned>(pageBeatsX8_),
          static_cast<unsigned>(ms));
  autoDeadline_ = millis() + ms;
  autoArmed_ = true;
#ifdef SIMULATOR
  // Flow testing without waiting out real page durations.
  if (const char* fastMs = std::getenv("CROSSPOINT_MUSIC_AUTO_MS")) {
    autoDeadline_ = millis() + static_cast<unsigned long>(atoi(fastMs));
  }
#endif
}

void MusicReaderActivity::renderPage() {
  renderer.clearScreen();
  for (size_t i = 0; i < systemCount_; i++) {
    drawSystem(systems_[i], systems_[i].staffTopY);
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
      renderer.drawText(musicFontId_, ox + fpToPx(prim.x1), oy + fpToPx(prim.y1) - musicFontAscender_, utf8, true);
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
      char buf[64];
      const size_t len = prim.textLen < sizeof(buf) - 1 ? prim.textLen : sizeof(buf) - 1;
      memcpy(buf, prim.text, len);
      buf[len] = '\0';

      // Digit-only labels (volta numbers) render with the music font's
      // time-signature digits: they exist at every staff size, so they
      // scale exactly with the notation. UI fonts bottom out around 17 px
      // and collide with the bracket at small staff sizes.
      bool digitsOnly = len > 0;
      for (size_t i = 0; i < len; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
          digitsOnly = false;
          break;
        }
      }
      if (digitsOnly && len <= 4) {
        char glyphs[4 * 3 + 1];
        char* p = glyphs;
        for (size_t i = 0; i < len; i++) {
          const uint16_t cp = 0xE080 + (buf[i] - '0');
          *p++ = static_cast<char>(0xE0 | (cp >> 12));
          *p++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          *p++ = static_cast<char>(0x80 | (cp & 0x3F));
        }
        *p = '\0';
        // Time-signature digits are ~2 ss tall with the glyph origin at
        // their vertical CENTER; nudge the center above the text baseline
        // so the digit sits between the bracket line and the staff.
        const int centerY = oy + fpToPx(prim.y1) - (staffSpacePx_ * 3) / 5;
        renderer.drawText(musicFontId_, ox + fpToPx(prim.x1), centerY - musicFontAscender_, glyphs, true);
        return;
      }

      // Other labels (future drill cues etc.): closest UI font.
      const auto style = (prim.textFlags & cpmx::TEXT_FLAG_BOLD) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int targetPx = fpToPx(prim.w);
      int fontId = UI_12_FONT_ID;  // ~25 px em
      if (targetPx < 14) {
        fontId = SMALL_FONT_ID;  // ~17 px em
      } else if (targetPx < 23) {
        fontId = UI_10_FONT_ID;  // ~21 px em
      }
      renderer.drawText(fontId, ox + fpToPx(prim.x1), oy + fpToPx(prim.y1) - renderer.getFontAscenderSize(fontId), buf,
                        true, style);
      return;
    }
  }
}

void MusicReaderActivity::pageForward() {
  if (atCover_) {
    // Leave the cover onto the first page (layout is already current).
    // The auto-page clock starts here, so the first turn is on tempo.
    atCover_ = false;
    renderPage();
    armAutoPage();
    return;
  }
  if (nextPagePos_ >= reader.playOrderLength()) {
    // Past the last page: mid-setlist flow to the next piece's cover — the
    // end screen belongs only after the last one.
    if (setlistMode_ && setlistIdx_ + 1 < setlist_.count()) {
      goToNextPiece();
      return;
    }
    atEnd_ = true;
    autoArmed_ = false;
    endOfBookOptions_.loadOnce(filePath);
    renderEndScreen();
    return;
  }
  if (previousPageCount_ >= MAX_PAGE_HISTORY) {
    return;  // history exhausted — pathological page counts
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
  armAutoPage();  // every forward entry restarts the page clock
  // Debounced progress save (SD wear): every 8 turns and on exit.
  if (++pageTurnsSinceSave_ >= 8) {
    saveProgress();
  }
}

void MusicReaderActivity::pageBack() {
  // Backing up means something is off — the timer never fights the player.
  autoArmed_ = false;
  if (atCover_) {
    // Back over the cover crosses into the previous setlist piece.
    if (setlistMode_ && setlistIdx_ > 0) {
      goToPreviousPieceEnd();
    }
    return;
  }
  if (previousPageCount_ == 0) {
    showCover();  // back from the first page lands on the cover
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
  if (++pageTurnsSinceSave_ >= 8) {
    saveProgress();
  }
}

void MusicReaderActivity::showCover() {
  atCover_ = true;
  // A cover never auto-advances: between pieces the band marches on and the
  // drum major decides when the next march starts.
  autoArmed_ = false;
  renderCover();
}

void MusicReaderActivity::renderCover() {
  renderer.clearScreen();
  // Center within the physically viewable area (bezel margins vary with
  // orientation), with breathing room on top of the TRBL insets.
  int vTop = 0, vRight = 0, vBottom = 0, vLeft = 0;
  renderer.getOrientedViewableTRBL(&vTop, &vRight, &vBottom, &vLeft);
  constexpr int COVER_MARGIN_PX = 24;
  const int left = vLeft + COVER_MARGIN_PX;
  const int usableW = renderer.getScreenWidth() - vLeft - vRight - 2 * COVER_MARGIN_PX;
  const int screenH = renderer.getScreenHeight();

  // Title from the piece metadata; a file without one shows its filename.
  const char* title = reader.title();
  std::string fallback;
  if (title[0] == '\0') {
    const std::string& path = setlistMode_ ? setlist_.pieceAt(setlistIdx_) : filePath;
    const auto slash = path.find_last_of('/');
    const auto start = (slash == std::string::npos) ? 0 : slash + 1;
    const auto dot = path.find_last_of('.');
    fallback = path.substr(start, (dot != std::string::npos && dot > start) ? dot - start : std::string::npos);
    title = fallback.c_str();
  }

  // Largest serif size whose title still fits the width.
  static constexpr int TITLE_FONTS[] = {NOTOSERIF_18_FONT_ID, NOTOSERIF_16_FONT_ID, NOTOSERIF_14_FONT_ID,
                                        NOTOSERIF_12_FONT_ID};
  int titleFont = TITLE_FONTS[3];
  for (const int fontId : TITLE_FONTS) {
    // cppcheck-suppress useStlAlgorithm
    if (renderer.getTextWidth(fontId, title, EpdFontFamily::BOLD) <= usableW) {
      titleFont = fontId;
      break;
    }
  }

  int y = screenH / 3;
  const int titleW = renderer.getTextWidth(titleFont, title, EpdFontFamily::BOLD);
  renderer.drawText(titleFont, left + (usableW - titleW) / 2, y, title, true, EpdFontFamily::BOLD);
  y += 2 * renderer.getFontAscenderSize(titleFont);

  if (reader.composer()[0] != '\0') {
    const int w = renderer.getTextWidth(NOTOSERIF_14_FONT_ID, reader.composer());
    renderer.drawText(NOTOSERIF_14_FONT_ID, left + (usableW - w) / 2, y, reader.composer(), true);
    y += 2 * renderer.getFontAscenderSize(NOTOSERIF_14_FONT_ID);
  }
  if (reader.arranger()[0] != '\0') {
    // Draw "arr." and the name as two adjacent runs: the arranger string can
    // be up to 256 bytes (format ceiling), too large to compose on the stack.
    const char* abbr = tr(STR_MUSIC_ARR_ABBR);
    const char* name = reader.arranger();
    const int spaceW = renderer.getSpaceWidth(NOTOSERIF_12_FONT_ID, EpdFontFamily::ITALIC);
    const int abbrW = renderer.getTextWidth(NOTOSERIF_12_FONT_ID, abbr, EpdFontFamily::ITALIC);
    const int nameW = renderer.getTextWidth(NOTOSERIF_12_FONT_ID, name, EpdFontFamily::ITALIC);
    const int x = left + (usableW - (abbrW + spaceW + nameW)) / 2;
    renderer.drawText(NOTOSERIF_12_FONT_ID, x, y, abbr, true, EpdFontFamily::ITALIC);
    renderer.drawText(NOTOSERIF_12_FONT_ID, x + abbrW + spaceW, y, name, true, EpdFontFamily::ITALIC);
  }

  // Bottom lines, small: setlist position ("where in the gig"), and the
  // armed tempo when auto page turn is on — visible before stepping off.
  int bottomY = screenH - vBottom - COVER_MARGIN_PX - renderer.getFontAscenderSize(UI_10_FONT_ID);
  if (setlistMode_) {
    char pos[24];
    snprintf(pos, sizeof(pos), "%u / %u", static_cast<unsigned>(setlistIdx_ + 1),
             static_cast<unsigned>(setlist_.count()));
    const int w = renderer.getTextWidth(UI_10_FONT_ID, pos);
    renderer.drawText(UI_10_FONT_ID, left + (usableW - w) / 2, bottomY, pos, true);
    bottomY -= 2 * renderer.getFontAscenderSize(UI_10_FONT_ID);
  }
  if (SETTINGS.musicAutoPage) {
    char tempo[40];
    snprintf(tempo, sizeof(tempo), "%s · %u", tr(STR_MUSIC_AUTO_PAGE), static_cast<unsigned>(SETTINGS.musicTempoBpm));
    const int w = renderer.getTextWidth(UI_10_FONT_ID, tempo);
    renderer.drawText(UI_10_FONT_ID, left + (usableW - w) / 2, bottomY, tempo, true);
  }

  renderer.displayBuffer();
#ifdef SIMULATOR
  if (std::getenv("CROSSPOINT_MUSIC_SHOT") != nullptr) {
    ScreenshotUtil::takeScreenshot(renderer);
  }
#endif
}

void MusicReaderActivity::renderEndScreen() {
  renderer.clearScreen();
  endOfBookOptions_.render(renderer, mappedInput);
  renderer.displayBuffer();
#ifdef SIMULATOR
  if (std::getenv("CROSSPOINT_MUSIC_SHOT") != nullptr) {
    ScreenshotUtil::takeScreenshot(renderer);
  }
#endif
}

// Returns true when the event was consumed by the end screen.
bool MusicReaderActivity::handleEndScreenInput() {
  if (endOfBookOptions_.menuActive()) {
    std::string openPath;
    switch (endOfBookOptions_.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return true;
      case EndOfBookOptions::Action::GoHome:
        activityManager.goHome();
        return true;
      case EndOfBookOptions::Action::LastPage:
        atEnd_ = false;
        renderPage();
        return true;
      case EndOfBookOptions::Action::Redraw:
        renderEndScreen();
        return true;
      case EndOfBookOptions::Action::None:
        break;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return true;
  }
  if (!endOfBookOptions_.menuActive()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      activityManager.goHome();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      atEnd_ = false;
      renderPage();
      return true;
    }
  }
  return true;  // the end screen owns all input while showing
}

void MusicReaderActivity::cycleStaffSize() {
  SETTINGS.musicStaffSize = (SETTINGS.musicStaffSize + 1) % CrossPointSettings::MUSIC_STAFF_SIZE_COUNT;
  SETTINGS.saveToFile();
  applyStaffSize();
  // Page boundaries shift with the size; re-layout from the current
  // position (stacked back-positions remain valid playOrder anchors).
  if (layoutPage(pagePos_)) {
    if (atCover_) {
      renderCover();  // size applies once the music shows; stay on the cover
    } else {
      renderPage();
      if (autoArmed_) {
        armAutoPage();  // page boundaries moved: restart this page's clock
      }
    }
  }
}

void MusicReaderActivity::loop() {
  Activity::loop();

#ifdef SIMULATOR
  // Desktop verification hook: with CROSSPOINT_MUSIC_AUTOPAGE set, page
  // through the whole piece automatically (pairs with the page-dump hook).
  if (std::getenv("CROSSPOINT_MUSIC_AUTOPAGE") != nullptr && reader.isOpen() &&
      nextPagePos_ < reader.playOrderLength()) {
    pageForward();
    return;
  }
  // Scripted input for headless testing: CROSSPOINT_MUSIC_SCRIPT is a
  // comma-separated action list (fwd/back/size), one action per ~700 ms.
  static const char* script = std::getenv("CROSSPOINT_MUSIC_SCRIPT");
  static size_t scriptPos = 0;
  static unsigned long lastAction = 0;
  if (script != nullptr && reader.isOpen() && millis() - lastAction > 700) {
    while (script[scriptPos] == ',') {
      scriptPos++;
    }
    if (script[scriptPos] != '\0') {
      lastAction = millis();
      if (strncmp(script + scriptPos, "fwd", 3) == 0) {
        scriptPos += 3;
        pageForward();
      } else if (strncmp(script + scriptPos, "back", 4) == 0) {
        scriptPos += 4;
        if (atEnd_) {
          atEnd_ = false;
          renderPage();
        } else {
          pageBack();
        }
      } else if (strncmp(script + scriptPos, "size", 4) == 0) {
        scriptPos += 4;
        cycleStaffSize();
      } else {
        scriptPos++;  // unknown byte: skip
      }
      return;
    }
  }
#endif

  // Signed-difference comparison survives millis() wraparound (49.7 days).
  if (autoArmed_ && static_cast<long>(millis() - autoDeadline_) >= 0) {
    autoArmed_ = false;
    pageForward();  // re-arms for the new page; covers and the end disarm
    return;
  }

  if (atEnd_) {
    handleEndScreenInput();
    return;
  }

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
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    cycleStaffSize();
    return;
  }
}
