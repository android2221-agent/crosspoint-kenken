#pragma once

#include "activities/Activity.h"
#include "activities/kenken/KenKenPuzzle.h"
#include "components/themes/BaseTheme.h"  // Rect

// Button-driven KenKen (KenKen = "MathDoku"). Grid 4x4 / 5x5 / 6x6, generated on
// the device, played with the four front buttons + the two side buttons.
//
// Rendering stays off the full-screen path: after the first paint every input
// only repaints the touched cells and pushes that rectangle to the panel with
// GfxRenderer::displayWindow(), so the screen behaves like menu navigation
// instead of flashing a whole frame per keypress.
class KenKenActivity final : public Activity {
 public:
  explicit KenKenActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KenKen", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }

 private:
  static constexpr int MAX_N = KenKenPuzzle::MAX_N;
  static constexpr int TOOL_COUNT = 4;
  enum Tool { TOOL_LEVEL = 0, TOOL_NOTES = 1, TOOL_CHECK = 2, TOOL_NEW = 3 };

  KenKenPuzzle puzzle_;
  int8_t values_[MAX_N][MAX_N] = {};
  uint8_t notes_[MAX_N][MAX_N] = {};  // candidate bitmask, bit v = value v

  int levelIndex_ = 2;  // index into kLevels_
  bool notesMode_ = false;
  int curRow_ = 0;  // -1 = toolbar row
  int curCol_ = 0;
  int toolIndex_ = 0;
  bool editing_ = false;
  int candidate_ = 1;
  bool solved_ = false;
  bool checked_ = false;
  bool wrong_[MAX_N][MAX_N] = {};
  int filled_ = 0;

  // Redraw bookkeeping (written by loop(), consumed by render()).
  bool fullRedraw_ = true;
  bool statusDirty_ = false;
  bool dirty_[MAX_N][MAX_N] = {};
  int partialsSinceFull_ = 0;

  // Last drawn status text rect, so the next redraw can erase the old glyphs.
  int statusX_ = 0;
  int statusW_ = 0;

  // Geometry, recomputed on entry and after a level change.
  int cellSize_ = 0;
  int gridX_ = 0;
  int gridY_ = 0;
  int toolbarY_ = 0;
  int toolbarH_ = 0;

  static int levelSize(int levelIndex);
  void computeLayout();
  void newPuzzle();
  void markAllDirty();
  void markDirty(int r, int c);

  void handleToolbarInput();
  void handleGridInput();

  void drawAll();
  void drawCell(int r, int c);
  void drawToolbar();
  // Clears and redraws the status text. Returns the (logical) rect it cleared,
  // which covers both the old and new text so partial repaints leave no glyphs.
  Rect drawStatusLine();
  void drawBigDigit(int value, int x, int y, int scale, bool black);
  void drawNotes(int r, int c, int cellX, int cellY, bool black);
  void drawClue(int r, int c, int cellX, int cellY, bool black);
  bool isCursor(int r, int c) const { return !editing_ && curRow_ == r && curCol_ == c; }
  bool cellEditable(int r, int c) const { return !puzzle_.isClue(r, c); }
  // Single-cell cages are givens: their value lives in the puzzle, not in values_.
  int cellValue(int r, int c) const {
    return puzzle_.isClue(r, c) ? puzzle_.clueAt(r, c) : values_[r][c];
  }
};
