#include "KenKenPuzzle.h"

// ---------------------------------------------------------------------------
// RNG: xorshift32 — deterministic per seed, no heap, no globals.
// ---------------------------------------------------------------------------
uint32_t KenKenPuzzle::nextRandom() {
  uint32_t x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_ = x;
  return x;
}

int KenKenPuzzle::randomBelow(const int bound) {
  if (bound <= 1) return 0;
  return static_cast<int>(nextRandom() % static_cast<uint32_t>(bound));
}

// ---------------------------------------------------------------------------
// Latin square: randomised backtracking over row/column bitmasks.
// ---------------------------------------------------------------------------
namespace {

struct LatinFiller {
  KenKenPuzzle* self;
  int n;
  uint16_t* rowMask;
  uint16_t* colMask;
  int8_t (*sol)[KenKenPuzzle::MAX_N];

  bool fill(const int pos) {
    if (pos == n * n) return true;
    const int r = pos / n;
    const int c = pos % n;
    int order[KenKenPuzzle::MAX_N];
    for (int i = 0; i < n; ++i) order[i] = i + 1;
    for (int i = n - 1; i > 0; --i) {
      const int j = self->randomBelow(i + 1);
      const int tmp = order[i];
      order[i] = order[j];
      order[j] = tmp;
    }
    for (int k = 0; k < n; ++k) {
      const int v = order[k];
      const uint16_t bit = static_cast<uint16_t>(1u << v);
      if ((rowMask[r] & bit) || (colMask[c] & bit)) continue;
      rowMask[r] |= bit;
      colMask[c] |= bit;
      sol[r][c] = static_cast<int8_t>(v);
      if (fill(pos + 1)) return true;
      rowMask[r] = static_cast<uint16_t>(rowMask[r] & ~bit);
      colMask[c] = static_cast<uint16_t>(colMask[c] & ~bit);
    }
    return false;
  }
};

}  // namespace

bool KenKenPuzzle::buildLatinSquare(const int n) {
  uint16_t rowMask[MAX_N] = {};
  uint16_t colMask[MAX_N] = {};
  LatinFiller filler{this, n, rowMask, colMask, sol_};
  return filler.fill(0);
}

// ---------------------------------------------------------------------------
// Cage partition: grow connected blobs of 1..4 cells, then derive op/target
// from the solution.
// ---------------------------------------------------------------------------
bool KenKenPuzzle::buildCages(const int n) {
  const int total = n * n;
  int8_t owner[MAX_N * MAX_N];
  int order[MAX_N * MAX_N];

  for (int i = 0; i < total; ++i) {
    owner[i] = -1;
    order[i] = i;
  }
  for (int i = total - 1; i > 0; --i) {
    const int j = randomBelow(i + 1);
    const int tmp = order[i];
    order[i] = order[j];
    order[j] = tmp;
  }

  cageCount_ = 0;
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      cageOf_[r][c] = -1;
      grid_[r][c] = 0;
    }
  }

  for (int idx = 0; idx < total; ++idx) {
    const int start = order[idx];
    if (owner[start] != -1) continue;
    if (cageCount_ >= MAX_CAGES) return false;

    Cage& cg = cages_[cageCount_];
    cg.size = 0;
    cg.op = '=';
    cg.target = 0;
    cg.cells[cg.size++] = static_cast<int8_t>(start);
    owner[start] = static_cast<int8_t>(cageCount_);

    // Keep cages modest: singles are rare, 2-3 dominate, 4 is the ceiling.
    const int roll = randomBelow(10);
    const int want = (roll < 1) ? 1 : (roll < 5) ? 2 : (roll < 9) ? 3 : 4;

    while (cg.size < want && cg.size < MAX_CAGE_CELLS) {
      int frontier[4 * MAX_CAGE_CELLS];
      int frontierCount = 0;
      for (int k = 0; k < cg.size; ++k) {
        const int cell = cg.cells[k];
        const int r = cell / n;
        const int c = cell % n;
        const int neighbours[4][2] = {{r - 1, c}, {r + 1, c}, {r, c - 1}, {r, c + 1}};
        for (const auto& nb : neighbours) {
          if (nb[0] < 0 || nb[0] >= n || nb[1] < 0 || nb[1] >= n) continue;
          const int ni = nb[0] * n + nb[1];
          if (owner[ni] != -1) continue;
          bool seen = false;
          for (int f = 0; f < frontierCount; ++f) {
            if (frontier[f] == ni) {
              seen = true;
              break;
            }
          }
          if (!seen) frontier[frontierCount++] = ni;
        }
      }
      if (frontierCount == 0) break;
      const int pick = frontier[randomBelow(frontierCount)];
      cg.cells[cg.size++] = static_cast<int8_t>(pick);
      owner[pick] = static_cast<int8_t>(cageCount_);
    }
    ++cageCount_;
  }

  // Sort each cage's cells into reading order and derive op/target.
  for (int ci = 0; ci < cageCount_; ++ci) {
    Cage& cg = cages_[ci];
    for (int k = 0; k < cg.size; ++k) {
      for (int j = k + 1; j < cg.size; ++j) {
        if (cg.cells[j] < cg.cells[k]) {
          const int8_t t = cg.cells[k];
          cg.cells[k] = cg.cells[j];
          cg.cells[j] = t;
        }
      }
    }

    int vals[MAX_CAGE_CELLS];
    int sum = 0;
    int product = 1;
    int lo = 99;
    int hi = 0;
    for (int k = 0; k < cg.size; ++k) {
      const int cell = cg.cells[k];
      const int v = sol_[cell / n][cell % n];
      vals[k] = v;
      sum += v;
      product *= v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    cg.labelCell = cg.cells[0];  // top-left-most cell carries the clue

    if (cg.size == 1) {
      cg.op = '=';
      cg.target = static_cast<int16_t>(vals[0]);
      grid_[cg.cells[0] / n][cg.cells[0] % n] = static_cast<int8_t>(vals[0]);
      continue;
    }

    const bool canSubtract = (cg.size == 2) && (hi != lo);
    const bool canDivide = (cg.size == 2) && (lo != 0) && (hi != lo) && (hi % lo == 0);
    const int pick = randomBelow(10);
    if (pick < 4) {
      cg.op = '+';
      cg.target = static_cast<int16_t>(sum);
    } else if (pick < 7) {
      cg.op = '*';
      cg.target = static_cast<int16_t>(product);
    } else if (canSubtract && !canDivide) {
      cg.op = '-';
      cg.target = static_cast<int16_t>(hi - lo);
    } else if (canDivide) {
      cg.op = '/';
      cg.target = static_cast<int16_t>(hi / lo);
    } else if (randomBelow(2) == 0) {
      cg.op = '+';
      cg.target = static_cast<int16_t>(sum);
    } else {
      cg.op = '*';
      cg.target = static_cast<int16_t>(product);
    }
  }

  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) cageOf_[r][c] = owner[r * n + c];
  return true;
}

// ---------------------------------------------------------------------------
// Uniqueness: count solutions up to 2 under a node budget.
// Returns -1 when the budget ran out before a first solution was found.
// ---------------------------------------------------------------------------
namespace {

struct SolverState {
  const KenKenPuzzle* p;
  int n;
  int total;
  int budget;
  int8_t grid[KenKenPuzzle::MAX_N * KenKenPuzzle::MAX_N];
  uint16_t rowMask[KenKenPuzzle::MAX_N];
  uint16_t colMask[KenKenPuzzle::MAX_N];
  int8_t cageFilled[KenKenPuzzle::MAX_CAGES];
  int16_t cageSum[KenKenPuzzle::MAX_CAGES];
  int32_t cageProduct[KenKenPuzzle::MAX_CAGES];
  int solutions;
  bool exhausted;
};

bool cageFeasible(const SolverState& s, const int ci) {
  const auto& cg = s.p->cage(ci);
  if (s.cageFilled[ci] != static_cast<int>(cg.size)) {
    if (cg.op == '+') {
      const int remaining = static_cast<int>(cg.size) - s.cageFilled[ci];
      // Cannot reach the target, or already overshot the smallest possible sum.
      if (s.cageSum[ci] + remaining > cg.target) return false;
      if (s.cageSum[ci] + remaining * s.n < cg.target) return false;
      return true;
    }
    if (cg.op == '*') {
      if (s.cageProduct[ci] > cg.target) return false;
      if (s.cageProduct[ci] <= 0) return false;
      if (cg.target % s.cageProduct[ci] != 0) return false;
      return true;
    }
    if (cg.op == '=') return true;  // single cell; checked when it completes
    return true;
  }

  switch (cg.op) {
    case '=':
      return s.grid[cg.cells[0]] == cg.target;
    case '+':
      return s.cageSum[ci] == cg.target;
    case '*':
      return s.cageProduct[ci] == cg.target;
    case '-': {
      if (cg.size != 2) return false;
      const int a = s.grid[cg.cells[0]];
      const int b = s.grid[cg.cells[1]];
      return (a > b ? a - b : b - a) == cg.target;
    }
    case '/': {
      if (cg.size != 2) return false;
      const int a = s.grid[cg.cells[0]];
      const int b = s.grid[cg.cells[1]];
      const int hi = a > b ? a : b;
      const int lo = a > b ? b : a;
      return lo != 0 && hi % lo == 0 && hi / lo == cg.target;
    }
    default:
      return false;
  }
}

void solveFrom(SolverState& s, const int pos) {
  if (s.solutions >= 2 || s.exhausted) return;
  if (--s.budget <= 0) {
    s.exhausted = true;
    return;
  }
  if (pos == s.total) {
    ++s.solutions;
    return;
  }
  const int r = pos / s.n;
  const int c = pos % s.n;
  const int ci = s.p->cageOf(r, c);
  for (int v = 1; v <= s.n; ++v) {
    const uint16_t bit = static_cast<uint16_t>(1u << v);
    if (s.rowMask[r] & bit) continue;
    if (s.colMask[c] & bit) continue;
    s.grid[pos] = static_cast<int8_t>(v);
    s.rowMask[r] |= bit;
    s.colMask[c] |= bit;
    ++s.cageFilled[ci];
    s.cageSum[ci] = static_cast<int16_t>(s.cageSum[ci] + v);
    s.cageProduct[ci] *= v;
    if (cageFeasible(s, ci)) solveFrom(s, pos + 1);
    --s.cageFilled[ci];
    s.cageSum[ci] = static_cast<int16_t>(s.cageSum[ci] - v);
    s.cageProduct[ci] /= v;
    s.rowMask[r] = static_cast<uint16_t>(s.rowMask[r] & ~bit);
    s.colMask[c] = static_cast<uint16_t>(s.colMask[c] & ~bit);
    s.grid[pos] = 0;
    if (s.solutions >= 2 || s.exhausted) return;
  }
}

}  // namespace

int KenKenPuzzle::countSolutions(const int nodeBudget) {
  SolverState s{};
  s.p = this;
  s.n = n_;
  s.total = n_ * n_;
  s.budget = nodeBudget;
  s.solutions = 0;
  s.exhausted = false;
  for (int i = 0; i < s.total; ++i) s.grid[i] = 0;
  for (int i = 0; i < n_; ++i) {
    s.rowMask[i] = 0;
    s.colMask[i] = 0;
  }
  for (int i = 0; i < cageCount_; ++i) {
    s.cageFilled[i] = 0;
    s.cageSum[i] = 0;
    s.cageProduct[i] = 1;
  }
  solveFrom(s, 0);
  if (s.exhausted && s.solutions == 0) return -1;
  return s.solutions;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
bool KenKenPuzzle::generate(const int n, const uint32_t seed) {
  if (n < 3 || n > MAX_N) return false;
  n_ = n;
  ready_ = false;
  ambiguous_ = false;
  attempts_ = 0;
  cageCount_ = 0;
  rng_ = seed ? seed : 0x9E3779B9u;

  int lastSolutions = 0;
  bool haveCandidate = false;

  for (int attempt = 0; attempt < 24; ++attempt) {
    ++attempts_;
    if (!buildLatinSquare(n)) continue;
    if (!buildCages(n)) continue;
    haveCandidate = true;
    const int count = countSolutions(60000);
    lastSolutions = count;
    if (count == 1) {
      ready_ = true;
      return true;
    }
  }

  // Every candidate was ambiguous or the budget expired. Hand back the final
  // board anyway so the caller always has something playable.
  (void)lastSolutions;
  ambiguous_ = true;
  ready_ = haveCandidate;
  return ready_;
}
