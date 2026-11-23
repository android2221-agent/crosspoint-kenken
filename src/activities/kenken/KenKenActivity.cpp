#include "KenKenActivity.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <HalStorage.h>

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

// Separator drawn between pencil marks so adjacent candidates read as "1,2,3"
// rather than a run of digits. Implemented as rects in drawComma() rather than
// a bitmap glyph: see the note there.

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

  computeNoteStyle();
}

// Text sizing. Deliberately a plain lookup table rather than anything derived
// from the cell size: there are exactly three boards, and someone tuning the
// look should be able to read the answer instead of recomputing it.
//
//   value - every entered digit and every given clue. ONE size per board, so a
//           number is the same size whether or not its cell also carries an
//           arithmetic label. (It used to shrink on labelled cells.)
//   note  - pencil marks, always smaller than a value so the two never read
//           alike.
//   rows  - how many rows of notes. Fixed at 2: three marks on the first row,
//           the rest on the second.
//
// Notes are drawn three to a row, comma-separated, so a full note row is
// 21 * noteScale px wide and a two-row block is 15 * noteScale px tall.
// The largest figures below are 84 px of a 110 px 4x4 cell, which leaves room
// for the cage label and the 3 px cage border.
void KenKenActivity::computeNoteStyle() {
  const int n = levelSize(levelIndex_);

  // Two orientations cover all four enum values: the two portrait ones share a
  // 480x800 screen, the two landscape ones 800x480.
  const auto o = renderer.getOrientation();
  const bool portrait =
      o == GfxRenderer::Orientation::Portrait || o == GfxRenderer::Orientation::PortraitInverted;

  int value = 6;
  int note = 3;
  if (portrait) {
    if (n == 4) {
      value = 8;
      note = 4;
    } else if (n == 5) {
      value = 7;
      note = 3;
    } else {  // 6x6
      value = 6;
      note = 3;
    }
  } else {
    if (n == 4) {
      value = 5;
      note = 2;
    } else if (n == 5) {
      value = 4;
      note = 2;
    } else {  // 6x6
      value = 3;
      note = 1;
    }
  }

  valueScale_ = value;
  noteScale_ = note;
  noteRows_ = 2;
}

// ---------------------------------------------------------------------------
// Save / resume
//
// The generator is deterministic — everything (Latin square, cages, operators,
// targets, givens) follows from a single 32-bit seed — so the save file only
// has to hold the seed, the level, and the player's values and notes. That is
// ~90 bytes instead of a serialised board.
//
// Written on exit (which also runs before deep sleep) and whenever a new board
// is created. Never on a keystroke: SPIFFS sectors have a finite erase budget.
// ---------------------------------------------------------------------------
namespace {
constexpr const char* kSavePath = "/.crosspoint/kenken.txt";
constexpr int kSaveVersion = 1;

// Strict integer parse of s[start, end). Returns false for empty or non-numeric
// text: String::toInt() would quietly give 0, turning a corrupt save into a
// board full of blanks.
bool parseField(const String& s, const int start, const int end, int& out) {
  if (end <= start) return false;
  int value = 0;
  for (int i = start; i < end; ++i) {
    const char ch = s[i];
    if (ch < '0' || ch > '9') return false;
    value = value * 10 + (ch - '0');
    if (value > 100000) return false;  // nothing we write is this large
  }
  out = value;
  return true;
}
}  // namespace

bool KenKenActivity::saveGame() {
  if (!Storage.ready()) return false;  // no SD card: play continues, nothing persists
  const int n = levelSize(levelIndex_);
  if (puzzle_.n() != n) return false;

  // One line per value/note row keeps the format trivially greppable.
  String out;
  out.reserve(320);
  out += "kenken ";
  out += String(kSaveVersion);
  out += " ";
  out += String(static_cast<unsigned long>(seed_));
  out += " ";
  out += String(levelIndex_);
  out += "\n";
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      out += String(static_cast<int>(values_[r][c]));
      out += ',';
    }
    out += '\n';
  }
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      out += String(static_cast<int>(notes_[r][c]));
      out += ',';
    }
    out += '\n';
  }

  if (!Storage.ensureDirectoryExists("/.crosspoint")) return false;
  const bool ok = Storage.writeFile(kSavePath, out);
  if (!ok) LOG_ERR("KK", "save failed");
  return ok;
}

bool KenKenActivity::loadGame() {
  if (!Storage.ready()) return false;
  if (!Storage.exists(kSavePath)) return false;

  String in = Storage.readFile(kSavePath);
  if (in.isEmpty()) return false;

  // Reject anything that is not ours or not this format version.
  if (!in.startsWith("kenken ")) return false;

  int pos = 7;
  const int version = in.substring(pos).toInt();
  if (version != kSaveVersion) return false;
  pos = in.indexOf(' ', pos) + 1;
  if (pos <= 0) return false;

  // Parse as UNSIGNED. String::toInt() is atol(), i.e. signed 32-bit, and half
  // of all uint32 seeds exceed INT32_MAX — those would clamp and restore a
  // different board under the player's entries.
  const uint32_t seed = static_cast<uint32_t>(strtoul(in.c_str() + pos, nullptr, 10));
  pos = in.indexOf(' ', pos) + 1;
  if (pos <= 0) return false;

  const int savedLevel = in.substring(pos).toInt();
  if (savedLevel < 0 || savedLevel >= kLevelCount) return false;
  pos = in.indexOf('\n', pos);
  if (pos < 0) return false;
  ++pos;

  const int n = levelSize(savedLevel);
  if (savedLevel != levelIndex_) {
    // Keep the saved level rather than discard the game: the player was on that
    // board when they left.
    levelIndex_ = savedLevel;
    computeLayout();
  }

  // Rebuild the exact board from the seed.
  puzzle_.generate(n, seed);
  if (!puzzle_.ready() || puzzle_.n() != n) return false;
  seed_ = seed;

  memset(values_, 0, sizeof(values_));
  memset(notes_, 0, sizeof(notes_));
  memset(wrong_, 0, sizeof(wrong_));

  // Values: must be a plain number in 0..n. Anything else means the file is not
  // what we wrote, so reject the whole save rather than restore a half-garbage
  // board under the player's eyes.
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      const int comma = in.indexOf(',', pos);
      if (comma < 0) return false;
      int v = 0;
      if (!parseField(in, pos, comma, v) || v < 0 || v > n) return false;
      values_[r][c] = static_cast<int8_t>(v);
      pos = comma + 1;
    }
    const int nl = in.indexOf('\n', pos);
    if (nl < 0) return false;
    pos = nl + 1;
  }
  // Notes: a bitmask, bit v = candidate v. Only bits 1..n are ever set.
  const int noteMask = (1 << (n + 1)) - 1;
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      const int comma = in.indexOf(',', pos);
      if (comma < 0) return false;
      int v = 0;
      if (!parseField(in, pos, comma, v) || v < 0 || (v & ~noteMask) != 0) return false;
      notes_[r][c] = static_cast<uint8_t>(v);
      pos = comma + 1;
    }
    const int nl = in.indexOf('\n', pos);
    if (nl >= 0) pos = nl + 1;  // last row may end at EOF
  }

  // Recompute derived state from what was restored.
  filled_ = 0;
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) {
      if (puzzle_.isClue(r, c)) ++filled_;
      else if (values_[r][c] > 0) ++filled_;
    }
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
  LOG_DBG("KK", "Resumed saved game (level %d)", savedLevel);
  return true;
}

void KenKenActivity::onExit() {
  saveGame();
  Activity::onExit();
}

void KenKenActivity::onEnter() {
  Activity::onEnter();
  computeLayout();
  // Resume the board the player left, including their entries, until they press
  // New. Only fall back to generating when there is nothing saved or the save
  // does not match this level.
  if (!loadGame()) newPuzzle();
}

void KenKenActivity::markAllDirty() {
  fullRedraw_ = true;
  memset(dirty_, 0, sizeof(dirty_));
}

void KenKenActivity::markDirty(const int r, const int c) {
  if (r < 0 || r >= puzzle_.n() || c < 0 || c >= puzzle_.n()) return;
  dirty_[r][c] = true;
}

void KenKenActivity::resetEntries() {
  const int n = levelSize(levelIndex_);
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

void KenKenActivity::newPuzzle() {
  const int n = levelSize(levelIndex_);
  // Seed from the clock plus the level so two boards in a row differ. Generation
  // almost always succeeds first time; retry rather than show an empty grid.
  for (int attempt = 0; attempt < 3; ++attempt) {
    seed_ = static_cast<uint32_t>(millis() * 2654435761u + n * 7919u + attempt * 104729u);
    puzzle_.generate(n, seed_);
    if (puzzle_.ready()) break;
  }
  resetEntries();
  saveGame();
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
    // Candidate selection. The front Left/Right buttons and the side Up/Down
    // buttons both step through 1..n then blank, so either hand can drive it:
    // the side buttons are far easier to reach while holding the device.
    int delta = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      delta = -1;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
               mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      delta = +1;
    }
    if (delta != 0) {
      candidate_ = (candidate_ + delta + (n + 1)) % (n + 1);
      markDirty(curRow_, curCol_);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const int before = values_[curRow_][curCol_];
      const bool wasChecked = checked_;
      // Tap = pencil mark, hold = real value (see LONG_PRESS_MS). Measured on
      // release, so holding then letting go still commits.
      const bool asValue = mappedInput.getHeldTime() >= LONG_PRESS_MS;

      if (!asValue) {
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

        // Strip this value from the pencil marks of every other cell in the row
        // and column. KenKen's row/column rule is the same as a Latin square's,
        // so a committed digit can never be a valid candidate elsewhere in
        // either line; leaving those marks behind just fills the board with
        // candidates that have already been disproven.
        if (candidate_ > 0) {
          const uint8_t clearedBit = static_cast<uint8_t>(~(1u << candidate_));
          for (int i = 0; i < n; ++i) {
            if (i != curCol_ && notes_[curRow_][i] != 0) {
              notes_[curRow_][i] &= clearedBit;
              dirty_[curRow_][i] = true;
            }
            if (i != curRow_ && notes_[i][curCol_] != 0) {
              notes_[i][curCol_] &= clearedBit;
              dirty_[i][curCol_] = true;
            }
          }
        }
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
// Pencil-mark separator, drawn as rects rather than a bitmap glyph so its width
// is exactly one narrow column. A 5-column glyph at scale 5 is 10 px wide before
// any padding, which either collided with the digit beside it or forced a slot
// so wide the marks no longer fitted the cell. A dot with a tail hanging below
// the baseline is what reads as a comma at this size.
void KenKenActivity::drawComma(const int x, const int y, const int scale, const bool black) {
  const int stroke = std::max(2, scale);
  const int dotTop = y + 5 * scale;  // sits low in the 7-row glyph box
  renderer.fillRect(x, dotTop, stroke, stroke, black);
  renderer.fillRect(x - std::max(1, stroke / 3), dotTop + stroke, stroke, stroke, black);
}

void KenKenActivity::drawGlyph(const uint8_t* glyph, const int x, const int y, const int scale, const bool black) {
  for (int row = 0; row < 7; ++row) {
    const uint8_t bits = glyph[row];
    if (bits == 0) continue;
    for (int col = 0; col < 5; ++col) {
      if (bits & (1u << (4 - col))) {
        renderer.fillRect(x + col * scale, y + row * scale, scale, scale, black);
      }
    }
  }
}

void KenKenActivity::drawBigDigit(const int value, const int x, const int y, const int scale, const bool black) {
  if (value < 0 || value > 9) return;
  drawGlyph(kDigits[value], x, y, scale, black);
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
  const int marks = __builtin_popcount(static_cast<unsigned>(mask & 0xFF));

  // Fixed layout, no fitting or searching:
  //   three marks per row, comma-separated, up to two rows
  //   a mark is 5x7 cells of `scale`, plus one `scale` of gap after it
  //   the comma gets two `scale` columns of its own
  // Size comes from the table in computeNoteStyle(), so every cell on a board
  // draws its marks identically.
  const int scale = noteScale_;
  const int comma = 2 * scale;
  const int gap = scale;
  const int slotStepX = 5 * scale + comma + gap;
  const int slotStepY = 7 * scale + gap;
  const int blockH = noteRows_ * 7 * scale + (noteRows_ - 1) * gap;
  const int startY = cellY + cellSize_ - 5 - blockH;

  int slot = 0;
  int row = 0;
  while (slot < marks) {
    const int count = std::min(3, marks - slot);
    // A row of `count` marks with (count-1) commas between them.
    const int rowW = count * 5 * scale + (count - 1) * (comma + gap);
    const int rowX = cellX + (cellSize_ - rowW) / 2;
    const int y = startY + row * slotStepY;

    int drawn = 0;
    for (int v = 1; v <= n && drawn < count; ++v) {
      if (!(mask & (1u << v))) continue;
      const int x = rowX + drawn * slotStepX;
      if (drawn > 0) drawComma(x - comma - gap + (comma - scale) / 2, y, scale, black);
      drawBigDigit(v, x, y, scale, black);
      ++drawn;
    }
    slot += count;
    ++row;
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

  // One digit size for the whole board (valueScale_), so a number is the same
  // size whether or not this cell also carries an arithmetic label. The digit is
  // nudged down on a labelled cell only to sit clear of the label, not resized.
  const int digitScale = valueScale_;
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
  // Confirm now means "note" on a tap and "set the value" on a hold. Keep the
  // label short: drawButtonHints draws a fixed 80 px button and does not clip,
  // so anything longer bleeds past the border. The tap/hold distinction is
  // spelled out in the app's README and the status line instead.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Note/Set", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
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
