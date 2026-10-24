#include "bitboard.h"

namespace sn {

Bitboard KNIGHT_ATT[64], KING_ATT[64];
Bitboard PAWN_ATT[2][64];
Magic BISHOP_MAGIC[64], ROOK_MAGIC[64];

namespace {

Bitboard bishopTable[5248], rookTable[102400];

const int BISHOP_DELTAS[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
const int ROOK_DELTAS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

// Squares reached by stepping (dr, df) from sq; with slide, keeps going until a blocker (inclusive).
Bitboard rays(int sq, Bitboard occ, const int deltas[][2], int count, bool slide) {
  Bitboard a = 0;
  for (int i = 0; i < count; i++) {
    int r = sq / 8 + deltas[i][0], f = sq % 8 + deltas[i][1];
    while (r >= 0 && r < 8 && f >= 0 && f < 8) {
      a |= bit(r * 8 + f);
      if (!slide || (occ & bit(r * 8 + f))) break;
      r += deltas[i][0];
      f += deltas[i][1];
    }
  }
  return a;
}

// Finds magic multipliers by trial (fixed seed, so every run builds identical tables).
void initMagics(Magic* magics, Bitboard* table, const int deltas[][2]) {
  static Bitboard occs[4096], refs[4096];
  static int epoch[4096];
  uint64_t seed = 0x9E3779B97F4A7C15ull;
  auto rnd = [&seed] {
    seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
    return seed * 0x2545F4914F6CDD1Dull;
  };
  int attempt = 0;
  for (int sq = 0; sq < 64; sq++) {
    const Bitboard ranks = (0xFFull | 0xFF00000000000000ull) & ~(0xFFull << (sq / 8 * 8));
    const Bitboard files = (0x0101010101010101ull | 0x8080808080808080ull) & ~(0x0101010101010101ull << (sq % 8));
    Magic& m = magics[sq];
    m.mask = rays(sq, 0, deltas, 4, true) & ~(ranks | files);
    m.shift = 64 - __builtin_popcountll(m.mask);
    m.attacks = table;
    int n = 0;
    Bitboard b = 0;
    do {  // every subset of the mask
      occs[n] = b;
      refs[n++] = rays(sq, b, deltas, 4, true);
      b = (b - m.mask) & m.mask;
    } while (b);
    for (;;) {
      m.magic = rnd() & rnd() & rnd();
      if (__builtin_popcountll((m.mask * m.magic) >> 56) < 6) continue;
      attempt++;
      bool ok = true;
      for (int i = 0; i < n && ok; i++) {
        const unsigned idx = unsigned((occs[i] * m.magic) >> m.shift);
        if (epoch[idx] != attempt) { epoch[idx] = attempt; table[idx] = refs[i]; }
        else if (table[idx] != refs[i]) ok = false;
      }
      if (ok) break;
    }
    table += n;
  }
}

struct Init {
  Init() {
    const int knight[8][2] = {{2, 1}, {2, -1}, {-2, 1}, {-2, -1}, {1, 2}, {1, -2}, {-1, 2}, {-1, -2}};
    const int king[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const int whitePawn[2][2] = {{1, -1}, {1, 1}}, blackPawn[2][2] = {{-1, -1}, {-1, 1}};
    for (int sq = 0; sq < 64; sq++) {
      KNIGHT_ATT[sq] = rays(sq, 0, knight, 8, false);
      KING_ATT[sq] = rays(sq, 0, king, 8, false);
      PAWN_ATT[0][sq] = rays(sq, 0, whitePawn, 2, false);
      PAWN_ATT[1][sq] = rays(sq, 0, blackPawn, 2, false);
    }
    initMagics(BISHOP_MAGIC, bishopTable, BISHOP_DELTAS);
    initMagics(ROOK_MAGIC, rookTable, ROOK_DELTAS);
  }
};
const Init init;

}  // namespace

}  // namespace sn
