#include "datagen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

#include "search.h"

namespace sn {

namespace {

struct Record {
  uint64_t occ;
  uint8_t pieces[16];
  int16_t score;
  uint8_t result, stm;
  uint8_t pad[4];
};
static_assert(sizeof(Record) == 32, "record layout");

Record pack(const Position& pos, int whiteScore) {
  Record r;
  std::memset(&r, 0, sizeof r);
  int n = 0;
  for (int sq = 0; sq < 64; sq++) {
    const int p = pos.board[(sq >> 3) * 16 + (sq & 7)];
    if (!p) continue;
    r.occ |= uint64_t(1) << sq;
    const int nib = (p < 0 ? 8 : 0) | (std::abs(p) - 1);
    r.pieces[n / 2] |= uint8_t(nib << (4 * (n & 1)));
    n++;
  }
  r.score = int16_t(whiteScore);
  r.stm = pos.side == WHITE ? 0 : 1;
  return r;
}

}  // namespace

uint64_t datagen(const std::string& outPath, int games, uint64_t nodes, uint64_t seed) {
  FILE* out = std::fopen(outPath.c_str(), "wb");
  if (!out) throw std::runtime_error("cannot open " + outPath);
  Engine engine(16);
  std::mt19937_64 rng(seed);
  Limits limits;
  limits.nodes = nodes;
  uint64_t written = 0;

  for (int g = 0; g < games; g++) {
    Position pos;
    engine.clear();

    // Random opening of 8 or 9 plies, so both sides get to move first out of the random part.
    const int randomPlies = 8 + int(rng() % 2);
    bool ok = true;
    for (int i = 0; i < randomPlies && ok; i++) {
      const std::vector<Move> legal = pos.legalMoves();
      if (legal.empty()) ok = false;
      else pos.makeMove(legal[rng() % legal.size()]);
    }
    if (!ok) continue;

    std::vector<Record> recs;
    int result = -1;  // for White: 0 loss, 1 draw, 2 win
    int winStreak = 0, drawStreak = 0;
    for (int ply = 0; ply < 400; ply++) {
      const std::vector<Move> legal = pos.legalMoves();
      const bool check = pos.inCheck();
      if (legal.empty()) { result = check ? (pos.side == WHITE ? 0 : 2) : 1; break; }
      if (pos.halfmove >= 100 || pos.repetitions() >= 2 || pos.insufficientMaterial()) { result = 1; break; }

      engine.stop = false;
      const Result r = engine.search(pos, limits);
      const int whiteScore = pos.side == WHITE ? r.score : -r.score;
      if (ply == 0 && std::abs(r.score) > 400) break;  // random opening left a lopsided position

      // Adjudicate: a decisive score held for 8 plies, or a dead-level score late in the game.
      if (std::abs(whiteScore) >= 2000) {
        const int s = whiteScore > 0 ? 1 : -1;
        winStreak = (winStreak > 0) == (s > 0) && winStreak ? winStreak + s : s;
        if (std::abs(winStreak) >= 8) { result = winStreak > 0 ? 2 : 0; break; }
      } else winStreak = 0;
      drawStreak = ply >= 80 && std::abs(whiteScore) <= 10 ? drawStreak + 1 : 0;
      if (drawStreak >= 12) { result = 1; break; }

      // Keep only quiet positions with a meaningful score: not in check, best move not a capture or
      // promotion, more than one legal move (the search returns no score otherwise), no mate score.
      const Move m = r.bestMove;
      const bool noisy = pos.board[mTo(m)] != 0 || (mFlags(m) & FLAG_EP) || mPromo(m);
      if (!check && !noisy && legal.size() > 1 && std::abs(r.score) < MATE_BOUND)
        recs.push_back(pack(pos, std::max(-32000, std::min(32000, whiteScore))));
      if (!m || !pos.makeMove(m)) break;
    }
    if (result < 0) {
      if (recs.empty()) continue;
      result = 1;  // 400-ply cap reached
    }
    for (Record& rec : recs) rec.result = uint8_t(result);
    std::fwrite(recs.data(), sizeof(Record), recs.size(), out);
    written += recs.size();
  }
  std::fclose(out);
  return written;
}

}  // namespace sn
