#pragma once
#include <cstdint>

namespace sn {

class Position;

// Signed piece-square scores (material included) indexed by (piece + 6) * 128 + sq: White adds, Black subtracts.
extern int16_t MG_PSQ[13 * 128], EG_PSQ[13 * 128];
extern const int PHASE_INC[7];
// Rough piece values used for move ordering, pruning margins and SEE.
extern const int VALUE[7];

// Score in centipawns from the side to move's perspective.
int evaluate(const Position& pos);

}  // namespace sn
