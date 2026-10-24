#include "eval.h"

#include <algorithm>
#include <cstdlib>

#include "position.h"

namespace sn {

const int PHASE_INC[7] = {0, 0, 1, 1, 2, 4, 0};
const int VALUE[7] = {0, 100, 320, 330, 500, 900, 20000};

namespace {

// PeSTO tables (Ronald Friederich): middlegame/endgame values blended by game phase.
// From White's point of view, rank 8 first.
const int MG_VALUE[7] = {0, 82, 337, 365, 477, 1025, 0};
const int EG_VALUE[7] = {0, 94, 281, 297, 512, 936, 0};

const int MG_TABLE[7][64] = {
    {},
    {0, 0, 0, 0, 0, 0, 0, 0, 98, 134, 61, 95, 68, 126, 34, -11, -6, 7, 26, 31, 65, 56, 25, -20,
     -14, 13, 6, 21, 23, 12, 17, -23, -27, -2, -5, 12, 17, 6, 10, -25, -26, -4, -4, -10, 3, 3, 33, -12,
     -35, -1, -20, -23, -15, 24, 38, -22, 0, 0, 0, 0, 0, 0, 0, 0},
    {-167, -89, -34, -49, 61, -97, -15, -107, -73, -41, 72, 36, 23, 62, 7, -17, -47, 60, 37, 65, 84, 129, 73, 44,
     -9, 17, 19, 53, 37, 69, 18, 22, -13, 4, 16, 13, 28, 19, 21, -8, -23, -9, 12, 10, 19, 17, 25, -16,
     -29, -53, -12, -3, -1, 18, -14, -19, -105, -21, -58, -33, -17, -28, -19, -23},
    {-29, 4, -82, -37, -25, -42, 7, -8, -26, 16, -18, -13, 30, 59, 18, -47, -16, 37, 43, 40, 35, 50, 37, -2,
     -4, 5, 19, 50, 37, 37, 7, -2, -6, 13, 13, 26, 34, 12, 10, 4, 0, 15, 15, 15, 14, 27, 18, 10,
     4, 15, 16, 0, 7, 21, 33, 1, -33, -3, -14, -21, -13, -12, -39, -21},
    {32, 42, 32, 51, 63, 9, 31, 43, 27, 32, 58, 62, 80, 67, 26, 44, -5, 19, 26, 36, 17, 45, 61, 16,
     -24, -11, 7, 26, 24, 35, -8, -20, -36, -26, -12, -1, 9, -7, 6, -23, -45, -25, -16, -17, 3, 0, -5, -33,
     -44, -16, -20, -9, -1, 11, -6, -71, -19, -13, 1, 17, 16, 7, -37, -26},
    {-28, 0, 29, 12, 59, 44, 43, 45, -24, -39, -5, 1, -16, 57, 28, 54, -13, -17, 7, 8, 29, 56, 47, 57,
     -27, -27, -16, -16, -1, 17, -2, 1, -9, -26, -9, -10, -2, -4, 3, -3, -14, 2, -11, -2, -5, 2, 14, 5,
     -35, -8, 11, 2, 8, 15, -3, 1, -1, -18, -9, 10, -15, -25, -31, -50},
    {-65, 23, 16, -15, -56, -34, 2, 13, 29, -1, -20, -7, -8, -4, -38, -29, -9, 24, 2, -16, -20, 6, 22, -22,
     -17, -20, -12, -27, -30, -25, -14, -36, -49, -1, -27, -39, -46, -44, -33, -51, -14, -14, -22, -46, -44, -30, -15, -27,
     1, 7, -8, -64, -43, -16, 9, 8, -15, 36, 12, -54, 8, -28, 24, 14},
};

const int EG_TABLE[7][64] = {
    {},
    {0, 0, 0, 0, 0, 0, 0, 0, 178, 173, 158, 134, 147, 132, 165, 187, 94, 100, 85, 67, 56, 53, 82, 84,
     32, 24, 13, 5, -2, 4, 17, 17, 13, 9, -3, -7, -7, -8, 3, -1, 4, 7, -6, 1, 0, -5, -1, -8,
     13, 8, 8, 10, 13, 0, 2, -7, 0, 0, 0, 0, 0, 0, 0, 0},
    {-58, -38, -13, -28, -31, -27, -63, -99, -25, -8, -25, -2, -9, -25, -24, -52, -24, -20, 10, 9, -1, -9, -19, -41,
     -17, 3, 22, 22, 22, 11, 8, -18, -18, -6, 16, 25, 16, 17, 4, -18, -23, -3, -1, 15, 10, -3, -20, -22,
     -42, -20, -10, -5, -2, -20, -23, -44, -29, -51, -23, -15, -22, -18, -50, -64},
    {-14, -21, -11, -8, -7, -9, -17, -24, -8, -4, 7, -12, -3, -13, -4, -14, 2, -8, 0, -1, -2, 6, 0, 4,
     -3, 9, 12, 9, 14, 10, 3, 2, -6, 3, 13, 19, 7, 10, -3, -9, -12, -3, 8, 10, 13, 3, -7, -15,
     -14, -18, -7, -1, 4, -9, -15, -27, -23, -9, -23, -5, -9, -16, -5, -17},
    {13, 10, 18, 15, 12, 12, 8, 5, 11, 13, 13, 11, -3, 3, 8, 3, 7, 7, 7, 5, 4, -3, -5, -3,
     4, 3, 13, 1, 2, 1, -1, 2, 3, 5, 8, 4, -5, -6, -8, -11, -4, 0, -5, -1, -7, -12, -8, -16,
     -6, -6, 0, 2, -9, -9, -11, -3, -9, 2, 3, -1, -5, -13, 4, -20},
    {-9, 22, 22, 27, 27, 19, 10, 20, -17, 20, 32, 41, 58, 25, 30, 0, -20, 6, 9, 49, 47, 35, 19, 9,
     3, 22, 24, 45, 57, 40, 57, 36, -18, 28, 19, 47, 31, 34, 39, 23, -16, -27, 15, 6, 9, 17, 10, 5,
     -22, -23, -30, -16, -16, -23, -36, -32, -33, -28, -22, -43, -5, -32, -20, -41},
    {-74, -35, -18, -18, -11, 15, 4, -17, -12, 17, 14, 17, 17, 38, 23, 11, 10, 17, 23, 15, 20, 45, 44, 13,
     -8, 22, 24, 27, 26, 33, 26, 3, -18, -4, 21, 24, 27, 23, 9, -11, -19, -3, 11, 21, 23, 16, 7, -9,
     -27, -11, 4, 13, 14, 4, -5, -17, -53, -34, -21, -11, -28, -14, -24, -43},
};

// Pawn-structure terms by relative rank (0 = own back rank).
const int PASSED_MG[8] = {0, 0, 5, 10, 20, 35, 60, 0};
const int PASSED_EG[8] = {0, 5, 10, 20, 35, 60, 90, 0};
const int DOUBLED_MG = -10, DOUBLED_EG = -20, ISOLATED_MG = -8, ISOLATED_EG = -12;

// Pawn structure only depends on pawns, so it's cached by the pawn-only hash.
struct PawnEntry {
  uint64_t key;
  int16_t mg, eg;
  bool valid;
};
constexpr int PAWN_CACHE_SIZE = 1 << 14;
thread_local PawnEntry pawnCache[PAWN_CACHE_SIZE];

void evalPawns(const int8_t* b, int& mg, int& eg) {
  int wFile[8] = {}, bFile[8] = {}, wMinRank[8], bMaxRank[8];
  std::fill(wMinRank, wMinRank + 8, 8);
  std::fill(bMaxRank, bMaxRank + 8, -1);
  for (int sq = 0; sq < 128; sq++) {
    if (offBoard(sq)) { sq += 7; continue; }
    const int r = sq >> 4, f = sq & 7;
    if (b[sq] == PAWN) { wFile[f]++; wMinRank[f] = std::min(wMinRank[f], r); }
    else if (b[sq] == -PAWN) { bFile[f]++; bMaxRank[f] = std::max(bMaxRank[f], r); }
  }
  mg = 0; eg = 0;
  for (int f = 0; f < 8; f++) {
    const int fl = f > 0 ? f - 1 : f, fr = f < 7 ? f + 1 : f;
    if (wFile[f] > 1) { mg += DOUBLED_MG * (wFile[f] - 1); eg += DOUBLED_EG * (wFile[f] - 1); }
    if (bFile[f] > 1) { mg -= DOUBLED_MG * (bFile[f] - 1); eg -= DOUBLED_EG * (bFile[f] - 1); }
    if (wFile[f] && !(f > 0 && wFile[f - 1]) && !(f < 7 && wFile[f + 1])) { mg += ISOLATED_MG * wFile[f]; eg += ISOLATED_EG * wFile[f]; }
    if (bFile[f] && !(f > 0 && bFile[f - 1]) && !(f < 7 && bFile[f + 1])) { mg -= ISOLATED_MG * bFile[f]; eg -= ISOLATED_EG * bFile[f]; }
    // Passed pawns: only the most advanced pawn on a file can be passed.
    for (int r = 6; r >= 1; r--) {
      if (b[r * 16 + f] != PAWN) continue;
      if (bMaxRank[fl] <= r && bMaxRank[f] <= r && bMaxRank[fr] <= r) { mg += PASSED_MG[r]; eg += PASSED_EG[r]; }
      break;
    }
    for (int r = 1; r <= 6; r++) {
      if (b[r * 16 + f] != -PAWN) continue;
      if (wMinRank[fl] >= r && wMinRank[f] >= r && wMinRank[fr] >= r) { mg -= PASSED_MG[7 - r]; eg -= PASSED_EG[7 - r]; }
      break;
    }
  }
}

// Middlegame bonus for pawns sheltering a castled king.
int kingShield(const int8_t* b, int ksq, int side) {
  const int kr = side == WHITE ? ksq >> 4 : 7 - (ksq >> 4), kf = ksq & 7;
  if (kr > 1) return -20;
  int s = 0;
  const int pawn = PAWN * side;
  const int r1 = side == WHITE ? kr + 1 : 6 - kr, r2 = side == WHITE ? kr + 2 : 5 - kr;
  for (int f = std::max(0, kf - 1); f <= std::min(7, kf + 1); f++) {
    if (b[r1 * 16 + f] == pawn) s += 12;
    else if (b[r2 * 16 + f] == pawn) s += 6;
    else s -= 12;
  }
  return s;
}

int centerDistance(int sq) {
  const int r = sq >> 4, f = sq & 7;
  return std::max(3 - r, r - 4) + std::max(3 - f, f - 4);
}

int nonPawnMaterial(const int8_t* c, int side) {
  return 3 * (c[KNIGHT * side + 6] + c[BISHOP * side + 6]) + 5 * c[ROOK * side + 6] + 9 * c[QUEEN * side + 6];
}

struct TableInit {
  TableInit() {
    for (int t = PAWN; t <= KING; t++) {
      for (int sq = 0; sq < 128; sq++) {
        if (offBoard(sq)) continue;
        const int r = sq >> 4, f = sq & 7, w = (7 - r) * 8 + f, bl = r * 8 + f;
        MG_PSQ[(t + 6) * 128 + sq] = int16_t(MG_VALUE[t] + MG_TABLE[t][w]);
        EG_PSQ[(t + 6) * 128 + sq] = int16_t(EG_VALUE[t] + EG_TABLE[t][w]);
        MG_PSQ[(-t + 6) * 128 + sq] = int16_t(-(MG_VALUE[t] + MG_TABLE[t][bl]));
        EG_PSQ[(-t + 6) * 128 + sq] = int16_t(-(EG_VALUE[t] + EG_TABLE[t][bl]));
      }
    }
  }
};

}  // namespace

int16_t MG_PSQ[13 * 128], EG_PSQ[13 * 128];
static const TableInit tableInit;

int evaluate(const Position& pos) {
  if (pos.useNet) return std::clamp(nnue::evaluate(pos.acc[pos.accTop], pos.side == WHITE), -20000, 20000);
  const int8_t* b = pos.board;
  const int8_t* c = pos.counts;
  int mg = pos.mg, eg = pos.eg;

  PawnEntry& pe = pawnCache[pos.pawnKey & (PAWN_CACHE_SIZE - 1)];
  if (!pe.valid || pe.key != pos.pawnKey) {
    int pm, pg;
    evalPawns(b, pm, pg);
    pe = {pos.pawnKey, int16_t(pm), int16_t(pg), true};
  }
  mg += pe.mg; eg += pe.eg;

  if (c[6 + BISHOP] >= 2) { mg += 30; eg += 50; }
  if (c[6 - BISHOP] >= 2) { mg -= 30; eg -= 50; }
  mg += kingShield(b, pos.kings[0], WHITE) - kingShield(b, pos.kings[1], BLACK);

  const int phase = std::min(pos.phase, 24);
  int score = (mg * phase + eg * (24 - phase)) / 24;

  const int strong = score > 0 ? WHITE : BLACK, weak = -strong;
  // Without pawns, a small material edge usually can't win (e.g. R vs B, N+N vs K).
  if (!c[PAWN * strong + 6] && nonPawnMaterial(c, strong) - nonPawnMaterial(c, weak) < 4) {
    score /= 8;
  } else if (phase <= 6 && !c[PAWN * weak + 6] && std::abs(score) > 300) {
    // Mop-up: when winning a pawnless ending, push the losing king to the edge and bring ours closer.
    const int ks = pos.kings[strong == WHITE ? 0 : 1], kw = pos.kings[strong == WHITE ? 1 : 0];
    const int dist = std::abs((ks >> 4) - (kw >> 4)) + std::abs((ks & 7) - (kw & 7));
    score += strong * (10 * centerDistance(kw) + 4 * (14 - dist));
  }
  return score * pos.side + 10;  // small tempo bonus
}

}  // namespace sn
