#pragma once

#include "activities/Activity.h"
#include "activities/kenken/KenKenPuzzle.h"
#include "components/themes/BaseTheme.h"  // Rect

// Button-driven KenKen (KenKen = "MathDoku"). Grid 4x4 / 5x5 / 6x6, generated on
// the device, played with the four front buttons + the two side buttons.
//
// Rendering: after the first paint, every input only *draws* the touched cells
// into the persistent framebuffer, then commits the frame with the ordinary
// displayBuffer() path.
//
// Why not a windowed update: GfxRenderer::displayWindow() and the SDK's
// EInkDisplay::displayWindow() are marked EXPERIMENTAL upstream and were never
// exercised by any shipping activity (the upstream declaration is literally
// commented out); this app was the first caller and, per docs, had never been
// run on hardware. The SDK windowed path also drives the panel with the
// ordinary fast-refresh sequence (0x22 = 0x1C) instead of the partial-update
// sequence the SSD1677 guide documents for bounded updates (0x21 = 0x00,
// 0x22 = 0xFC), so a windowed refresh does not get the temperature-loaded
// waveform a partial update expects.
//
// Painting the dirty rectangles into the framebuffer and doing one full-frame
// write is correct by construction: the panel always receives a complete frame
// through the same code path every other screen in the firmware uses, so no
// partially-driven rectangle or stale controller RAM can accumulate.
//
// Cost: the dirty-rect bookkeeping stays, so drawing is still cheap; only the
// transfer grows (~48 KB, ~12 ms per the boot log). Refreshes stay FAST.
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
  static constexpr int TOOL_COUNT = 3;
  // Edit mode: a tap on Confirm commits the candidate as a pencil mark, a hold
  // commits it as the real value. The value is the permanent action, so it gets
  // the deliberate gesture — a tap can never lock in a wrong digit. This
  // replaces the old Notes on/off toolbar mode, which forced a round trip to
  // the toolbar before any value could be entered.
  static constexpr unsigned long LONG_PRESS_MS = 400;
  enum Tool { TOOL_LEVEL = 0, TOOL_CHECK = 1, TOOL_NEW = 2 };

  KenKenPuzzle puzzle_;
  int8_t values_[MAX_N][MAX_N] = {};
  uint8_t notes_[MAX_N][MAX_N] = {};  // candidate bitmask, bit v = value v

  // Start on the easiest board: index 0 of kLevelNames{"4x4","5x5","6x6"}.
  int levelIndex_ = 0;  // index into kLevels_
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
