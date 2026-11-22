#pragma once

#include <cstdint>

// KenKen puzzle generation and uniqueness checking for the ESP32-C3 e-reader.
//
// Generator shape: build a random Latin square, partition it into cages, then
// count solutions of the resulting puzzle. Only puzzles with exactly one
// solution are returned. Every solve attempt runs under a node budget so a bad
// cage layout can never stall the caller's main loop.
class KenKenPuzzle {
 public:
  static constexpr int MAX_N = 6;
  static constexpr int MAX_CAGES = MAX_N * MAX_N;
  static constexpr int MAX_CAGE_CELLS = 6;

  struct Cage;

  // Generates a puzzle of size n (4..6). Returns false only when every attempt
  // was rejected; `bestEffort()` then describes the last candidate.
  bool generate(int n, uint32_t seed);

  int n() const { return n_; }
  bool ready() const { return ready_; }
  // True when the returned puzzle is ambiguous (generation budget exhausted).
  bool ambiguous() const { return ambiguous_; }

  int solutionAt(int r, int c) const { return sol_[r][c]; }
  bool isClue(int r, int c) const { return grid_[r][c] != 0; }
  int clueAt(int r, int c) const { return grid_[r][c]; }
  int cageOf(int r, int c) const { return cageOf_[r][c]; }
  int cageCount() const { return cageCount_; }
  const Cage& cage(int i) const { return cages_[i]; }
  int attemptCount() const { return attempts_; }

  struct Cage {
    int8_t cells[MAX_CAGE_CELLS];  // cell index r*n+c
    uint8_t size;
    char op;  // '+', '-', '*', '/', '=' (single cell)
    int16_t target;
    // First cell in reading order, which carries the clue label on screen.
    int8_t labelCell;
  };

  // Exposed so the backtracking helpers (file-local structs) can randomise.
  uint32_t nextRandom();
  int randomBelow(int bound);

 private:
  bool buildLatinSquare(int n);
  bool buildCages(int n);
  bool isClueCandidate(int r, int c) const;
  // 0 = no solution, 1 = unique, 2 = two or more, -1 = node budget exceeded.
  int countSolutions(int nodeBudget);

  int n_ = 0;
  bool ready_ = false;
  bool ambiguous_ = false;
  int attempts_ = 0;
  int cageCount_ = 0;
  int8_t sol_[MAX_N][MAX_N] = {};
  int8_t grid_[MAX_N][MAX_N] = {};  // given digits (single-cell cages)
  int8_t cageOf_[MAX_N][MAX_N] = {};
  Cage cages_[MAX_CAGES] = {};
  uint32_t rng_ = 0x9E3779B9u;
};
