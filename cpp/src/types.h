#pragma once
#include <cstdint>
#include <string>

namespace sn {

constexpr int PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6;
constexpr int WHITE = 1, BLACK = -1;
constexpr int FLAG_EP = 1, FLAG_CASTLE = 2, FLAG_DOUBLE = 4;
constexpr int MAX_MOVES = 256;
constexpr int MAX_PLY = 100;

// Move encoding (same layout as engine.js): from | to << 7 | promo << 14 | flags << 17
using Move = uint32_t;
constexpr Move encode(int from, int to, int promo, int flags) {
  return Move(from | (to << 7) | (promo << 14) | (flags << 17));
}
constexpr int mFrom(Move m) { return m & 127; }
constexpr int mTo(Move m) { return (m >> 7) & 127; }
constexpr int mPromo(Move m) { return (m >> 14) & 7; }
constexpr int mFlags(Move m) { return (m >> 17) & 7; }

// 0x88 board: rank * 16 + file. Any square with bit 0x88 set (including negatives) is off the board.
constexpr bool offBoard(int sq) { return sq & 0x88; }

extern const char* const START_FEN;

std::string sqName(int sq);
std::string moveToUci(Move m);

}  // namespace sn
