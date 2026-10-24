#pragma once
#include <cstdint>
#include <string>

namespace sn {

// Self-play training data. Each game starts with a few random moves, then both sides search a
// fixed number of nodes per move. Quiet positions are written as 32-byte records:
//   uint64 occupancy (bit i = square i, a1 = 0, h8 = 63)
//   uint8  pieces[16]  one nibble per occupied square in ascending order, low nibble first:
//                      color * 8 + type, color 0 = white / 1 = black, type 0 = pawn .. 5 = king
//   int16  score       search score in centipawns, White's point of view
//   uint8  result      game result for White: 0 loss, 1 draw, 2 win
//   uint8  stm         side to move: 0 white, 1 black
//   uint8  pad[4]
// Returns the number of positions written.
uint64_t datagen(const std::string& outPath, int games, uint64_t nodes, uint64_t seed);

}  // namespace sn
