#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "position.h"

namespace sn {

constexpr int INF = 32000, MATE = 30000, MATE_BOUND = 29000;

struct Limits {
  int depth = 0;
  int64_t movetime = 0;
  int64_t wtime = -1, btime = -1, winc = 0, binc = 0;
  int movestogo = 0;
  uint64_t nodes = 0;
  bool infinite = false;
};

struct Info {
  int depth, score;
  bool isMate;
  int mate;  // moves to mate; negative when getting mated
  uint64_t nodes;
  int64_t timeMs;
  std::vector<Move> pv;
};

struct Result {
  Move bestMove;
  int score, depth;
  uint64_t nodes;
};

class Engine {
 public:
  explicit Engine(int hashMb = 64);
  void setHashSize(int mb);
  void setThreads(int n);  // Lazy SMP: helper threads search the same position and share the hash table
  void clear();
  // onInfo is called after each completed iteration.
  Result search(Position& pos, const Limits& limits, const std::function<void(const Info&)>& onInfo = nullptr);

  std::atomic<bool> stop{false};  // set from another thread to end the search early

 private:
  struct TTEntry {
    uint32_t key;
    Move move;
    int16_t score;
    int8_t depth;
    uint8_t flagGen;  // bound type in the low 2 bits, search generation in the high 6
  };
  enum { TT_NONE, TT_EXACT, TT_LOWER, TT_UPPER };
  static constexpr int PIECE_TO = 13 * 128;  // (piece + 6) * 128 + to

  std::vector<TTEntry> tt;
  TTEntry* ttData = nullptr;  // tt.data(), or the main engine's table for a helper
  size_t ttMask = 0;
  std::vector<std::unique_ptr<Engine>> helpers;
  uint8_t generation = 0;
  Position* pos = nullptr;
  uint64_t nodes = 0, nodeLimit = 0;
  bool stopped = false;
  std::chrono::steady_clock::time_point startTime;
  double hardLimitMs = 0;
  Move killers[MAX_PLY][2] = {};
  int historyTable[PIECE_TO] = {};
  // Continuation history: how good a (piece, to) move is after a given (piece, to) move 1 or 2 plies earlier.
  std::vector<int16_t> contHist;
  int pieceToStack[MAX_PLY + 1] = {};  // piece-to index of the move played at each ply, -1 for a null move
  Move excluded[MAX_PLY + 1] = {};     // move skipped by a singular-extension verification search
  Move rootBest = 0;

  int64_t elapsedMs() const;
  bool timeUp() const;
  const TTEntry* probe(uint64_t key) const;
  void store(uint64_t key, Move m, int score, int depth, int flag);
  int quietScore(int pieceTo, int ply) const;
  void scoreMoves(const Move* moves, int* scores, int n, Move ttMove, int ply);
  Move next(Move* moves, int* scores, int n, int i);
  void updateQuiet(int pieceTo, int ply, int bonus);
  int negamax(int depth, int alpha, int beta, int ply, bool allowNull);
  int quiesce(int alpha, int beta, int ply);
  std::vector<Move> extractPv(int maxLen);
};

}  // namespace sn
