#include "nnue.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace sn::nnue {

const Network* net = nullptr;

namespace {

Network loaded;

#ifdef SN_NET
// Embeds the network file named by SN_NET (set from CMake) into the executable.
#ifdef _WIN32
#define SN_RODATA ".section .rdata,\"dr\"\n"
#else
#define SN_RODATA ".section .rodata\n"
#endif
asm(SN_RODATA
    ".balign 64\n"
    ".global snEmbeddedNet\n"
    "snEmbeddedNet:\n"
    ".incbin \"" SN_NET "\"\n"
    ".global snEmbeddedNetEnd\n"
    "snEmbeddedNetEnd:\n"
    ".byte 0\n"
    ".text\n");
extern "C" const unsigned char snEmbeddedNet[], snEmbeddedNetEnd[];

struct LoadEmbedded {
  LoadEmbedded() { loadBuffer(snEmbeddedNet, size_t(snEmbeddedNetEnd - snEmbeddedNet)); }
};
const LoadEmbedded loadEmbedded;
#endif

template <typename T>
T readLE(const unsigned char*& p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  p += sizeof v;
  return v;
}

}  // namespace

bool hasEmbedded() {
#ifdef SN_NET
  return true;
#else
  return false;
#endif
}

std::string loadBuffer(const unsigned char* data, size_t size) {
  if (size < 12 || std::memcmp(data, "SNNU", 4) != 0) return "not a Supernova network file";
  const unsigned char* p = data + 4;
  const int version = readLE<int32_t>(p), n = readLE<int32_t>(p);
  if (version != 1) return "unsupported network version " + std::to_string(version);
  if (n <= 0 || n > MAX_HIDDEN || n % 16) return "unsupported hidden size " + std::to_string(n);
  const size_t expected = 12 + size_t(768 * n + n + 2 * n) * 2 + 4;
  if (size < expected) return "network file is truncated";
  Network nw;
  nw.hidden = n;
  nw.ftWeights.resize(size_t(768) * n);
  nw.ftBias.resize(n);
  nw.outWeights.resize(2 * size_t(n));
  for (auto& w : nw.ftWeights) w = readLE<int16_t>(p);
  for (auto& w : nw.ftBias) w = readLE<int16_t>(p);
  for (auto& w : nw.outWeights) w = readLE<int16_t>(p);
  nw.outBias = readLE<int32_t>(p);
  loaded = std::move(nw);
  net = &loaded;
  return "";
}

std::string loadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "cannot open " + path;
  const std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return loadBuffer(data.data(), data.size());
}

void refresh(Accumulator& acc, const int8_t* board88) {
  for (int p = 0; p < 2; p++) std::copy(net->ftBias.begin(), net->ftBias.end(), acc.v[p]);
  for (int sq = 0; sq < 64; sq++) {
    const int piece = board88[(sq >> 3) * 16 + (sq & 7)];
    if (piece) update(acc, piece, sq, 1);
  }
}

void copy(Accumulator& dst, const Accumulator& src) {
  const size_t bytes = size_t(net->hidden) * sizeof(int16_t);
  std::memcpy(dst.v[0], src.v[0], bytes);
  std::memcpy(dst.v[1], src.v[1], bytes);
}

void update(Accumulator& acc, int piece, int sq64, int sign) {
  const int n = net->hidden;
  for (int p = 0; p < 2; p++) {
    const int16_t* w = net->ftWeights.data() + size_t(featureIndex(p, piece, sq64)) * n;
    int16_t* a = acc.v[p];
    if (sign > 0)
      for (int i = 0; i < n; i++) a[i] = int16_t(a[i] + w[i]);
    else
      for (int i = 0; i < n; i++) a[i] = int16_t(a[i] - w[i]);
  }
}

int evaluate(const Accumulator& acc, bool whiteToMove) {
  const int n = net->hidden;
  const int16_t* us = acc.v[whiteToMove ? 0 : 1];
  const int16_t* them = acc.v[whiteToMove ? 1 : 0];
  const int16_t* w = net->outWeights.data();
  int32_t sum = 0;
  for (int i = 0; i < n; i++) sum += std::clamp<int32_t>(us[i], 0, QA) * w[i];
  for (int i = 0; i < n; i++) sum += std::clamp<int32_t>(them[i], 0, QA) * w[n + i];
  return int((int64_t(sum) + net->outBias) * SCALE / (QA * QB));
}

}  // namespace sn::nnue
