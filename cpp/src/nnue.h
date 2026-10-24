#pragma once
#include <cstdint>
#include <string>
#include <vector>

// NNUE evaluation (768 -> N x 2 -> 1), the format written by train/train.py:
//   char[4] "SNNU", int32 version 1, int32 N
//   int16 ft_weights[768][N] (x255), int16 ft_bias[N] (x255), int16 out_weights[2N] (x64), int32 out_bias (x255*64)
// Feature index from a perspective: own/enemy (0/1) * 384 + piece type (0-5) * 64 + square,
// with squares flipped vertically (sq ^ 56) for Black's perspective.
namespace sn::nnue {

constexpr int QA = 255, QB = 64, SCALE = 400, MAX_HIDDEN = 1024;

struct Network {
  int hidden = 0;
  std::vector<int16_t> ftWeights, ftBias, outWeights;
  int32_t outBias = 0;
};

// The active network, or nullptr to use the handcrafted evaluation.
extern const Network* net;

// Loads a network from a file or a memory buffer; returns an error message, or "" on success.
std::string loadFile(const std::string& path);
std::string loadBuffer(const unsigned char* data, size_t size);
// True when built with SN_NET: that network is compiled into the executable and loaded at startup.
bool hasEmbedded();

struct Accumulator {
  alignas(64) int16_t v[2][MAX_HIDDEN];  // [white perspective, black perspective]
};

inline int featureIndex(int perspective, int piece, int sq64) {  // piece: +-1..6, perspective: 0 white, 1 black
  const int own = (piece > 0) == (perspective == 0) ? 0 : 1;
  const int type = (piece > 0 ? piece : -piece) - 1;
  return own * 384 + type * 64 + (perspective ? sq64 ^ 56 : sq64);
}

void refresh(Accumulator& acc, const int8_t* board88);
void copy(Accumulator& dst, const Accumulator& src);  // only the network's hidden size
void update(Accumulator& acc, int piece, int sq64, int sign);
int evaluate(const Accumulator& acc, bool whiteToMove);  // centipawns, side to move's view

}  // namespace sn::nnue
