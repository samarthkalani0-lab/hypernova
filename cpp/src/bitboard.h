#pragma once
#include <cstdint>

// Bitboards use 64-square indexing (rank * 8 + file); the rest of the engine uses 0x88 squares.
namespace sn {

using Bitboard = uint64_t;

constexpr int to64(int sq88) { return (sq88 + (sq88 & 7)) >> 1; }
constexpr int to88(int sq64) { return sq64 + (sq64 & ~7); }
constexpr Bitboard bit(int sq64) { return 1ull << sq64; }

inline int lsb(Bitboard b) { return __builtin_ctzll(b); }
inline int popLsb(Bitboard& b) {
  const int s = lsb(b);
  b &= b - 1;
  return s;
}

constexpr Bitboard RANK_2 = 0xFF00ull, RANK_7 = 0xFF000000000000ull;

struct Magic {
  Bitboard mask, magic;
  const Bitboard* attacks;
  int shift;
};

extern Bitboard KNIGHT_ATT[64], KING_ATT[64];
extern Bitboard PAWN_ATT[2][64];  // [white, black]: squares a pawn of that colour on sq attacks
extern Magic BISHOP_MAGIC[64], ROOK_MAGIC[64];

inline Bitboard bishopAttacks(int sq, Bitboard occ) {
  const Magic& m = BISHOP_MAGIC[sq];
  return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}
inline Bitboard rookAttacks(int sq, Bitboard occ) {
  const Magic& m = ROOK_MAGIC[sq];
  return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}

}  // namespace sn
