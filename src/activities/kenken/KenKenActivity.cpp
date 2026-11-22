#include "KenKenActivity.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// 5x7 bitmap digits, one byte per row, bit 4 = leftmost column. Drawing these as
// scaled blocks keeps the big cell digits crisp without shipping another font.
const uint8_t kDigits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

const char* kLevelNames[] = {"4x4", "5x5", "6x6"};
constexpr int kLevelCount = 3;

}  // namespace

int KenKenActivity::levelSize(const int levelIndex) { return 4 + levelIndex; }

void KenKenActivity::computeLayout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  toolbarY_ = metrics.topPadding + metrics.headerHeight + 8;
  toolbarH_ = 34;

  const int top = toolbarY_ + toolbarH_ + 12;
  const int bottomLimit = pageH - metrics.buttonHintsHeight - 46;
  const int available = std::max(120, bottomLimit - top);
  const int maxWidth = pageW - 2 * metrics.contentSidePadding;

  const int n = levelSize(levelIndex_);
  int size = std::min(available, maxWidth);
  cellSize_ = size / n;
  size = cellSize_ * n;
  gridX_ = (pageW - size) / 2;
  gridY_ = top + (available - size) / 2;
}

void KenKenActivity::onEnter() {
  Activity::onEnter();
  computeLayout();
  newPuzzle();
}

void KenKenActivity::markAllDirty() {
  fullRedraw_ = true;
  memset(dirty_, 0, sizeof(dirty_));
}

void KenKenActivity::markDirty(const int r, const int c) {
  if (r < 0 || r >= puzzle_.n() || c < 0 || c >= puzzle_.n()) return;
  dirty_[r][c] = true;
}

void KenKenActivity::newPuzzle() {
  const int n = levelSize(levelIndex_);
  // Seed from the clock plus the level so two boards in a row differ. Generation
  // almost always succeeds first time; retry rather than show an empty grid.
  for (int attempt = 0; attempt < 3; ++attempt) {
    puzzle_.generate(n, static_cast<uint32_t>(millis() * 2654435761u + n * 7919u + attempt * 104729u));
    if (puzzle_.ready()) break;
  }
  memset(values_, 0, sizeof(values_));
  memset(notes_, 0, sizeof(notes_));
  memset(wrong_, 0, sizeof(wrong_));
  filled_ = 0;
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c)
      if (puzzle_.isClue(r, c)) ++filled_;
  solved_ = false;
  checked_ = false;
  editing_ = false;
  curRow_ = 0;
  curCol_ = 0;
  toolIndex_ = 0;
  partialsSinceFull_ = 0;
  computeLayout();
  markAllDirty();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void KenKenActivity::loop() {
  const int n = puzzle_.n();
  if (n <= 0) {
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (editing_) {
      editing_ = false;
      markDirty(curRow_, curCol_);
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (editing_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      candidate_ = (candidate_ - 1 + (n + 1)) % (n + 1);
      markDirty(curRow_, curCol_);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      candidate_ = (candidate_ + 1) % (n + 1);
      markDirty(curRow_, curCol_);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const int before = values_[curRow_][curCol_];
      const bool wasChecked = checked_;
      if (notesMode_) {
        // Pencil marks only mean anything on an empty cell; on a filled cell
        // they are invisible (the value is drawn over them) so ignore them.
        if (candidate_ > 0 && before == 0) notes_[curRow_][curCol_] ^= static_cast<uint8_t>(1u << candidate_);
      } else {
        values_[curRow_][curCol_] = static_cast<int8_t>(candidate_);
        if (candidate_ != 0) notes_[curRow_][curCol_] = 0;
        // filled_ tracks *values*, so only adjust it when a value was actually
        // committed (not when a notes-mode bit was toggled, which used to
        // corrupt the count and could trigger a bogus "solved").
        if (before == 0 && candidate_ != 0) ++filled_;
        if (before != 0 && candidate_ == 0) --filled_;
      }
      wrong_[curRow_][curCol_] = false;
      if (wasChecked) {
        // Leaving check mode removes every underline, so repaint the grid.
        for (int r = 0; r < n; ++r)
          for (int c = 0; c < n; ++c) dirty_[r][c] = true;
      }
      checked_ = false;
      statusDirty_ = true;
      editing_ = false;
      if (filled_ == n * n) {
        bool complete = true;
        for (int r = 0; r < n && complete; ++r)
          for (int c = 0; c < n && complete; ++c)
            if (cellValue(r, c) != puzzle_.solutionAt(r, c)) complete = false;
        if (complete) {
          solved_ = true;
          markAllDirty();
        }
      }
      markDirty(curRow_, curCol_);
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (curRow_ == 0) {
      curRow_ = -1;
      markAllDirty();  // toolbar highlight moves
    } else if (curRow_ > 0) {
      markDirty(curRow_, curCol_);
      --curRow_;
      markDirty(curRow_, curCol_);
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (curRow_ == -1) {
      curRow_ = 0;
      markAllDirty();
    } else if (curRow_ < n - 1) {
      markDirty(curRow_, curCol_);
      ++curRow_;
      markDirty(curRow_, curCol_);
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (curRow_ == -1) {
      toolIndex_ = (toolIndex_ - 1 + TOOL_COUNT) % TOOL_COUNT;
      markAllDirty();
    } else {
      markDirty(curRow_, curCol_);
      curCol_ = (curCol_ - 1 + n) % n;
      markDirty(curRow_, curCol_);
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (curRow_ == -1) {
      toolIndex_ = (toolIndex_ + 1) % TOOL_COUNT;
      markAllDirty();
    } else {
      markDirty(curRow_, curCol_);
      curCol_ = (curCol_ + 1) % n;
      markDirty(curRow_, curCol_);
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (curRow_ == -1) {
      switch (toolIndex_) {
        case TOOL_LEVEL:
          levelIndex_ = (levelIndex_ + 1) % kLevelCount;
          fullRedraw_ = true;
          newPuzzle();
          return;
        case TOOL_NOTES:
          notesMode_ = !notesMode_;
          markAllDirty();
          break;
        case TOOL_CHECK: {
          checked_ = true;
          int bad = 0;
          for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
              if (!puzzle_.isClue(r, c) && values_[r][c] != 0 && values_[r][c] != puzzle_.solutionAt(r, c)) {
                wrong_[r][c] = true;
                ++bad;
              } else {
                wrong_[r][c] = false;
              }
            }
          if (bad == 0 && filled_ == n * n) solved_ = true;
          markAllDirty();
          break;
        }
        case TOOL_NEW:
          newPuzzle();
          return;
      }
      requestUpdate();
      return;
    }

    if (solved_) {
      // Nothing left to edit.
      return;
    }
    if (!cellEditable(curRow_, curCol_)) return;
    editing_ = true;
    candidate_ = values_[curRow_][curCol_] > 0 ? values_[curRow_][curCol_] : 1;
    markDirty(curRow_, curCol_);
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void KenKenActivity::drawBigDigit(const int value, const int x, const int y, const int scale, const bool black) {
  if (value < 0 || value > 9) return;
  const uint8_t* glyph = kDigits[value];
  for (int row = 0; row < 7; ++row) {
    const uint8_t bits = glyph[row];
    for (int col = 0; col < 5; ++col) {
      if (bits & (1u << (4 - col))) {
        renderer.fillRect(x + col * scale, y + row * scale, scale, scale, black);
      }
    }
  }
}

void KenKenActivity::drawClue(const int r, const int c, const int cellX, const int cellY, const bool black) {
  const int ci = puzzle_.cageOf(r, c);
  if (ci < 0) return;
  const auto& cage = puzzle_.cage(ci);
  if (cage.op == '=') return;  // the single-cell value is drawn large already
  if (cage.labelCell != static_cast<int8_t>(r * puzzle_.n() + c)) return;

  char label[8];
  snprintf(label, sizeof(label), "%d%c", cage.target, cage.op);
  renderer.drawText(NOTOSANS_14_FONT_ID, cellX + 4, cellY + 2, label, black, EpdFontFamily::BOLD);
}

void KenKenActivity::drawNotes(const int r, const int c, const int cellX, const int cellY, const bool black) {
  const uint8_t mask = notes_[r][c];
  if (mask == 0) return;
  const int n = puzzle_.n();
  const int scale = 1;
  const int digitW = 5 * scale;
  const int digitH = 7 * scale;
  const int stepX = digitW + 4;
  const int stepY = digitH + 3;
  const int startX = cellX + 6;
  const int rowsNeeded = (n + 2) / 3;
  const int startY = cellY + cellSize_ - 6 - rowsNeeded * stepY;
  int index = 0;
  for (int v = 1; v <= n; ++v) {
    if (!(mask & (1u << v))) continue;
    const int col = index % 3;
    const int row = index / 3;
    drawBigDigit(v, startX + col * stepX, startY + row * stepY, scale, black);
    ++index;
  }
}

void KenKenActivity::drawCell(const int r, const int c) {
  const int n = puzzle_.n();
  const int x = gridX_ + c * cellSize_;
  const int y = gridY_ + r * cellSize_;
  const bool cursor = isCursor(r, c) || (editing_ && curRow_ == r && curCol_ == c);

  renderer.fillRect(x, y, cellSize_, cellSize_, false);

  if (cursor) {
    if (editing_) {
      // Editing: a solid frame keeps the candidate digit readable.
      renderer.fillRect(x, y, cellSize_, 3, true);
      renderer.fillRect(x, y + cellSize_ - 3, cellSize_, 3, true);
      renderer.fillRect(x, y, 3, cellSize_, true);
      renderer.fillRect(x + cellSize_ - 3, y, 3, cellSize_, true);
    } else {
      renderer.fillRect(x + 1, y + 1, cellSize_ - 2, cellSize_ - 2, true);
    }
  }

  // Cage borders: 3px between different cages, 1px inside a cage.
  // Each cell owns its top/left edge; an interior cage boundary is therefore
  // drawn exactly once, by the cell below/right of it. Drawing the opposite
  // edges of both neighbours too made every inter-cage boundary 6px thick.
  // The bottom/right edge is only drawn for the outer frame.
  const int ci = puzzle_.cageOf(r, c);
  auto thick = [&](const int bx, const int by, const int bw, const int bh) { renderer.fillRect(bx, by, bw, bh, true); };
  const bool topCage = (r == 0) || puzzle_.cageOf(r - 1, c) != ci;
  const bool leftCage = (c == 0) || puzzle_.cageOf(r, c - 1) != ci;
  const int cageT = 3;
  const int innerT = 1;
  if (topCage) {
    thick(x, y, cellSize_, cageT);
  } else {
    thick(x, y, cellSize_, innerT);
  }
  if (leftCage) {
    thick(x, y, cageT, cellSize_);
  } else {
    thick(x, y, innerT, cellSize_);
  }
  if (r == n - 1) thick(x, y + cellSize_ - cageT, cellSize_, cageT);
  if (c == n - 1) thick(x + cellSize_ - cageT, y, cageT, cellSize_);

  const bool black = !(cursor && !editing_);  // an inverted cursor cell needs white ink
  // Cells that carry a cage label draw their digit a little smaller and lower so
  // the clue text in the top-left corner stays readable.
  const int labelCage = puzzle_.cageOf(r, c);
  const bool hasLabel =
      labelCage >= 0 && puzzle_.cage(labelCage).op != '=' && puzzle_.cage(labelCage).labelCell == r * n + c;
  if (hasLabel) drawClue(r, c, x, y, black);

  const int digitScale = std::max(2, cellSize_ / (hasLabel ? 15 : 12));
  const int digitW = 5 * digitScale;
  const int digitH = 7 * digitScale;
  const int digitX = x + (cellSize_ - digitW) / 2;
  const int digitY = y + (cellSize_ - digitH) / 2 + (hasLabel ? 6 : 0);

  if (puzzle_.isClue(r, c)) {
    drawBigDigit(puzzle_.clueAt(r, c), digitX, digitY, digitScale, black);
    return;
  }

  if (values_[r][c] > 0 && !editing_) {
    drawBigDigit(values_[r][c], digitX, digitY, digitScale, black);
    if (checked_ && wrong_[r][c]) {
      renderer.fillRect(x + cellSize_ / 4, y + cellSize_ - 12, cellSize_ / 2, 2, black);
    }
    return;
  }

  if (editing_) {
    if (candidate_ > 0) drawBigDigit(candidate_, digitX, digitY, digitScale, true);
    return;
  }

  drawNotes(r, c, x, y, black);
}

void KenKenActivity::drawToolbar() {
  const int pageW = renderer.getScreenWidth();
  const int margin = 12;
  const int gap = 8;
  const int pillW = (pageW - 2 * margin - (TOOL_COUNT - 1) * gap) / TOOL_COUNT;
  char labels[TOOL_COUNT][16];
  snprintf(labels[TOOL_LEVEL], sizeof(labels[0]), "%s", kLevelNames[levelIndex_]);
  snprintf(labels[TOOL_NOTES], sizeof(labels[0]), "%s", notesMode_ ? "Notes:on" : "Notes:off");
  snprintf(labels[TOOL_CHECK], sizeof(labels[0]), "Check");
  snprintf(labels[TOOL_NEW], sizeof(labels[0]), "New");

  for (int i = 0; i < TOOL_COUNT; ++i) {
    const int x = margin + i * (pillW + gap);
    const bool selected = (curRow_ == -1 && toolIndex_ == i);
    // Clear first: a partial repaint reuses this without a full clearScreen.
    renderer.fillRect(x, toolbarY_, pillW, toolbarH_, false);
    renderer.drawRoundedRect(x, toolbarY_, pillW, toolbarH_, 1, 8, true);
    if (selected) renderer.fillRoundedRect(x, toolbarY_, pillW, toolbarH_, 8, Color::Black);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, labels[i], EpdFontFamily::BOLD);
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, x + (pillW - textW) / 2, toolbarY_ + (toolbarH_ - textH) / 2, labels[i],
                      !selected, EpdFontFamily::BOLD);
  }
}

Rect KenKenActivity::drawStatusLine() {
  const int pageW = renderer.getScreenWidth();
  const int n = puzzle_.n();
  const int y = gridY_ + cellSize_ * n + 18;
  char status[48];
  if (solved_) {
    snprintf(status, sizeof(status), "Solved!");
  } else if (checked_) {
    int wrong = 0;
    for (int r = 0; r < n; ++r)
      for (int c = 0; c < n; ++c)
        if (wrong_[r][c]) ++wrong;
    if (wrong == 0) {
      snprintf(status, sizeof(status), "%d of %d filled", filled_, n * n);
    } else {
      snprintf(status, sizeof(status), "%d wrong", wrong);
    }
  } else {
    snprintf(status, sizeof(status), "%d/%d filled", filled_, n * n);
  }
  const int textW = renderer.getTextWidth(UI_12_FONT_ID, status, EpdFontFamily::BOLD);
  const int textX = (pageW - textW) / 2;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  // The string is centred and its width changes as the fill count does, so
  // clear the union of the previous and the new text. Otherwise a shorter
  // string leaves fragments of the old one on screen.
  const int clearX = (statusW_ > 0) ? std::min(statusX_, textX) : textX;
  const int clearR = (statusW_ > 0) ? std::max(statusX_ + statusW_ - 1, textX + textW - 1) : textX + textW - 1;
  const int clearW = clearR - clearX + 1;
  renderer.fillRect(clearX, y, clearW, lineH, false);
  renderer.drawText(UI_12_FONT_ID, textX, y, status, true, EpdFontFamily::BOLD);
  statusX_ = textX;
  statusW_ = textW;
  return Rect{clearX, y, clearW, lineH};
}

void KenKenActivity::drawAll() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int n = puzzle_.n();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, "KenKen");
  drawToolbar();
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) drawCell(r, c);
  drawStatusLine();

  // Keep the hint bar independent of editing_: partial repaints never touch it,
  // so a mode-dependent bar would be left showing the previous mode's labels.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void KenKenActivity::render(RenderLock&&) {
  const int n = puzzle_.n();
  if (n <= 0) return;

  if (fullRedraw_) {
    fullRedraw_ = false;
    // Clear the bookkeeping *before* drawing. loop() runs on the other task and
    // may mark a cell (or the status line) dirty while this frame is being
    // composed; clearing afterwards would swallow that change and leave stale
    // pixels on the next partial render.
    memset(dirty_, 0, sizeof(dirty_));
    statusDirty_ = false;
    drawAll();
    // A new board (or a completed one) is the right moment to clear ghosting.
    renderer.displayBuffer(partialsSinceFull_ > 60 ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    partialsSinceFull_ = 0;
    return;
  }

  // Anything drawn this frame? Tracked as a single flag: the dirty rectangles
  // are painted into the persistent framebuffer, and then the whole frame is
  // committed in one go (see the header for why the windowed path is avoided).
  bool anyDrawn = false;

  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) {
      if (!dirty_[r][c]) continue;
      dirty_[r][c] = false;  // clear first, for the same reason as above
      drawCell(r, c);
      anyDrawn = true;
    }

  // The status line is text, not a cell; repaint it only when its contents
  // actually changed.
  if (statusDirty_) {
    statusDirty_ = false;
    drawStatusLine();
    anyDrawn = true;
  }

  // Likewise, the toolbar highlight is only reachable as a whole.
  if (curRow_ == -1) {
    drawToolbar();
    anyDrawn = true;
  }

  if (!anyDrawn) return;

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  partialsSinceFull_ += 1;

  if (partialsSinceFull_ >= 100) {
    // Ghosting accumulates on fast partial updates; resync the whole panel.
    partialsSinceFull_ = 0;
    drawAll();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}
