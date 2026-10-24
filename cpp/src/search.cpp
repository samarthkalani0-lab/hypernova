#include "search.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "eval.h"

namespace sn {

namespace {

constexpr int HISTORY_MAX = 16384;
// Move-ordering score bands (bad captures sort below quiet moves).
constexpr int TT_MOVE_SCORE = 10000000, GOOD_CAPTURE = 1000000, UNCHECKED_CAPTURE = 950000, BAD_CAPTURE = -1000000;
constexpr int KILLER1 = 900000, KILLER2 = 800000;

#ifndef SN_HIST_SCALE
#define SN_HIST_SCALE 8  // history bonus multiplier: larger bonuses train continuation history faster
#endif
#ifndef SN_HLMR_DIV
#define SN_HLMR_DIV 4096  // history needed to change a late-move reduction by one ply
#endif

// Late-move reduction amounts by [depth][move number].
struct LmrTable {
  int r[64][64];
  LmrTable() {
    for (int d = 0; d < 64; d++)
      for (int i = 0; i < 64; i++)
        r[d][i] = d && i ? int(std::floor(0.75 + std::log(d) * std::log(i) / 2.25)) : 0;
  }
};
const LmrTable LMR;

bool isQuiet(const Position& p, Move m) { return !p.board[mTo(m)] && !mPromo(m) && !(mFlags(m) & FLAG_EP); }

}  // namespace

Engine::Engine(int hashMb) : contHist(size_t(PIECE_TO) * PIECE_TO, 0) { setHashSize(hashMb); }

void Engine::setHashSize(int mb) {
  const size_t want = size_t(std::max(1, mb)) * 1024 * 1024 / sizeof(TTEntry);
  size_t size = 2;  // at least one two-entry bucket
  while (size * 2 <= want) size *= 2;
  tt.assign(size, TTEntry{});
  ttData = tt.data();
  ttMask = size - 1;
}

void Engine::setThreads(int n) {
  n = std::max(1, n);
  helpers.resize(size_t(n - 1));
  for (auto& h : helpers)
    if (!h) h = std::make_unique<Engine>(1);
}

void Engine::clear() {
  std::fill(tt.begin(), tt.end(), TTEntry{});
  std::fill(&killers[0][0], &killers[0][0] + MAX_PLY * 2, Move(0));
  std::fill(historyTable, historyTable + PIECE_TO, 0);
  std::fill(contHist.begin(), contHist.end(), int16_t(0));
  generation = 0;
  for (auto& h : helpers) h->clear();
}

#ifdef SN_TT_BUCKETS  // off by default: tested at -10 Elo (3,293 games at 10+0.1)
// Two-entry buckets: a probe checks both slots; a store replaces the matching slot, an empty one,
// or else the less valuable one (shallower, or left over from an older search).
const Engine::TTEntry* Engine::probe(uint64_t key) const {
  const TTEntry* b = &ttData[key & ttMask & ~size_t(1)];
  const uint32_t check = uint32_t(key >> 32);
  if ((b[0].flagGen & 3) && b[0].key == check) return &b[0];
  if ((b[1].flagGen & 3) && b[1].key == check) return &b[1];
  return nullptr;
}

void Engine::store(uint64_t key, Move m, int score, int depth, int flag) {
  TTEntry* b = &ttData[key & ttMask & ~size_t(1)];
  const uint32_t check = uint32_t(key >> 32);
  TTEntry* e;
  if (b[0].key == check || !(b[0].flagGen & 3)) e = &b[0];
  else if (b[1].key == check || !(b[1].flagGen & 3)) e = &b[1];
  else {
    auto worth = [this](const TTEntry& x) { return x.depth - 8 * ((generation - (x.flagGen >> 2)) & 63); };
    e = worth(b[0]) <= worth(b[1]) ? &b[0] : &b[1];
  }
  if (!m && e->key == check) m = e->move;  // keep a known best move when the new result has none
  *e = {check, m, int16_t(score), int8_t(depth), uint8_t(flag | (generation << 2))};
}
#else
const Engine::TTEntry* Engine::probe(uint64_t key) const {
  const TTEntry& e = ttData[key & ttMask];
  return (e.flagGen & 3) && e.key == uint32_t(key >> 32) ? &e : nullptr;
}

void Engine::store(uint64_t key, Move m, int score, int depth, int flag) {
  ttData[key & ttMask] = {uint32_t(key >> 32), m, int16_t(score), int8_t(depth), uint8_t(flag | (generation << 2))};
}
#endif

int64_t Engine::elapsedMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
}

bool Engine::timeUp() const {
  if (stop.load(std::memory_order_relaxed)) return true;
  if (nodeLimit && nodes >= nodeLimit) return true;
  return elapsedMs() >= hardLimitMs;
}

Result Engine::search(Position& p, const Limits& limits, const std::function<void(const Info&)>& onInfo) {
  pos = &p;
  nodes = 0;
  stopped = false;
  startTime = std::chrono::steady_clock::now();
  std::fill(&killers[0][0], &killers[0][0] + MAX_PLY * 2, Move(0));
  std::fill(excluded, excluded + MAX_PLY + 1, Move(0));
  for (int& h : historyTable) h /= 4;
  generation = uint8_t((generation + 1) & 63);

  double soft = 1e18, hard = 1e18;
  if (limits.movetime) {
    soft = hard = double(limits.movetime);
  } else if (limits.wtime >= 0 || limits.btime >= 0) {
    const double time = double(std::max<int64_t>(0, p.side == WHITE ? limits.wtime : limits.btime));
    const double inc = double(p.side == WHITE ? limits.winc : limits.binc);
    const int mtg = limits.movestogo ? limits.movestogo : 30;
    const double base = time / mtg + inc * 0.8;
#ifndef SN_NO_TIME_MGMT
    // Iterations normally stop near 0.6x the base share; the hard limit lets an unstable search
    // (best move changing, score dropping) run up to 3x the base share.
    hard = std::max(5.0, std::min(base * 3, time / 2 - 20));
    soft = std::min(base * 0.6, hard);
#else
    hard = std::max(5.0, std::min(base, time / 2 - 20));
    soft = hard * 0.5;
#endif
  }
  [[maybe_unused]] const bool scaleTime = !limits.movetime && soft < 1e17;
  [[maybe_unused]] Move prevBest = 0;
  [[maybe_unused]] int stable = 0, prevScore = 0;
  if (limits.infinite) soft = hard = 1e18;
  hardLimitMs = hard;
  nodeLimit = limits.nodes;
  const int maxDepth = std::min(limits.depth ? limits.depth : 64, MAX_PLY - 10);

  const std::vector<Move> legal = p.legalMoves();
  Move best = legal.empty() ? 0 : legal[0];
  int bestScore = 0, completed = 0;
  if (legal.size() <= 1 && !limits.infinite && !limits.depth) return {best, 0, 0, 0};

  // Helpers search until this thread finishes; only their hash-table entries are used.
  std::vector<Position> helperPos(helpers.size(), p);
  std::vector<std::thread> helperThreads;
  for (size_t i = 0; i < helpers.size(); i++) {
    Engine& h = *helpers[i];
    h.ttData = ttData;
    h.ttMask = ttMask;
    h.generation = uint8_t((generation - 1) & 63);  // its search() advances it to ours
    h.stop = false;
    Limits hl;
    hl.infinite = true;
    hl.depth = maxDepth;
    helperThreads.emplace_back([&h, &hp = helperPos[i], hl] { h.search(hp, hl); });
  }
  auto totalNodes = [this] {
    uint64_t n = nodes;
    for (auto& h : helpers) n += h->nodes;
    return n;
  };

  for (int d = 1; d <= maxDepth; d++) {
    rootBest = 0;
    // Aspiration window around the previous score; widen and re-search on failure.
    int delta = 25, alpha = -INF, beta = INF, score = 0;
    if (d >= 5 && std::abs(bestScore) < MATE_BOUND) { alpha = bestScore - delta; beta = bestScore + delta; }
    for (;;) {
      score = negamax(d, alpha, beta, 0, false);
      if (stopped) break;
      if (score <= alpha) { beta = (alpha + beta) / 2; alpha = std::max(-INF, score - delta); }
      else if (score >= beta) beta = std::min(INF, score + delta);
      else break;
      delta *= 2;
      if (delta > 800) { alpha = -INF; beta = INF; }
    }
    // A move that beat the previous best before time ran out is still trustworthy.
    if (rootBest) best = rootBest;
    if (stopped) break;
    bestScore = score;
    completed = d;
    if (onInfo) {
      Info info{d, score, false, 0, totalNodes(), elapsedMs(), extractPv(d)};
      if (std::abs(score) > MATE_BOUND) {
        info.isMate = true;
        info.mate = score > 0 ? (MATE - score + 1) / 2 : -(MATE + score + 1) / 2;
      }
      onInfo(info);
    }
    double scale = 1;
#ifndef SN_NO_TIME_MGMT
    // Spend less time when the best move keeps coming back, more when it changes or the score drops.
    if (scaleTime && d >= 4) {
      static constexpr double STABILITY[] = {2.0, 1.4, 1.1, 0.9, 0.75};
      stable = best == prevBest ? std::min(stable + 1, 4) : 0;
      scale = STABILITY[stable];
      if (score < prevScore - 30) scale *= 1.3;
    }
    prevBest = best;
    prevScore = score;
#endif
    if (elapsedMs() >= soft * scale) break;
    if (!limits.infinite && std::abs(score) > MATE_BOUND && d >= MATE - std::abs(score)) break;
  }
  // In infinite mode UCI requires waiting for "stop" before reporting the move.
  while (limits.infinite && !stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for (auto& h : helpers) h->stop = true;
  for (auto& t : helperThreads) t.join();
  return {best, bestScore, completed, totalNodes()};
}

std::vector<Move> Engine::extractPv(int maxLen) {
  std::vector<Move> pv;
  Move buf[MAX_MOVES];
  while (int(pv.size()) < maxLen) {
    const TTEntry* e = probe(pos->key);
    if (!e || !e->move) break;
    const int n = pos->genMoves(buf, false);
    if (std::find(buf, buf + n, e->move) == buf + n || !pos->makeMove(e->move)) break;
    pv.push_back(e->move);
  }
  for (size_t i = 0; i < pv.size(); i++) pos->unmakeMove();
  return pv;
}

void Engine::scoreMoves(const Move* moves, int* scores, int n, Move ttMove, int ply) {
  const int8_t* b = pos->board;
  const Move k0 = killers[ply][0], k1 = killers[ply][1];
  for (int i = 0; i < n; i++) {
    const Move m = moves[i];
    if (m == ttMove) { scores[i] = TT_MOVE_SCORE; continue; }
    const int from = mFrom(m), to = mTo(m), promo = mPromo(m);
    const int victim = (mFlags(m) & FLAG_EP) ? PAWN : std::abs(b[to]);
    if (victim || promo) {
      const int attacker = std::abs(b[from]);
      // Taking a cheaper piece might lose material; next() checks those with SEE when they come up.
      const bool good = promo || VALUE[victim] >= VALUE[attacker];
      scores[i] = (good ? GOOD_CAPTURE : UNCHECKED_CAPTURE) + VALUE[victim] * 10 + VALUE[promo] - attacker;
    } else if (m == k0) {
      scores[i] = KILLER1;
    } else if (m == k1) {
      scores[i] = KILLER2;
    } else {
      scores[i] = quietScore((b[from] + 6) * 128 + to, ply);
    }
  }
}

// Picks the best remaining move into slot i, resolving possibly-losing captures with SEE only when reached.
Move Engine::next(Move* moves, int* scores, int n, int i) {
  for (;;) {
    int bi = i;
    for (int j = i + 1; j < n; j++)
      if (scores[j] > scores[bi]) bi = j;
    std::swap(moves[i], moves[bi]);
    std::swap(scores[i], scores[bi]);
    const int s = scores[i];
    if (s < UNCHECKED_CAPTURE || s >= GOOD_CAPTURE) return moves[i];
    if (pos->see(moves[i]) >= 0) { scores[i] = s - UNCHECKED_CAPTURE + GOOD_CAPTURE; return moves[i]; }
    scores[i] = s - UNCHECKED_CAPTURE + BAD_CAPTURE;
  }
}

// Ordering score of a quiet move: its own history plus how well it has worked as a reply to the
// moves played 1 and 2 plies earlier.
int Engine::quietScore(int pieceTo, int ply) const {
  int s = historyTable[pieceTo];
#ifndef SN_NO_CONTHIST
  if (ply >= 1 && pieceToStack[ply - 1] >= 0) s += contHist[size_t(pieceToStack[ply - 1]) * PIECE_TO + pieceTo];
  if (ply >= 2 && pieceToStack[ply - 2] >= 0) s += contHist[size_t(pieceToStack[ply - 2]) * PIECE_TO + pieceTo];
#endif
  return s;
}

// History "gravity" updates keep every score within +-HISTORY_MAX.
void Engine::updateQuiet(int pieceTo, int ply, int bonus) {
  auto gravity = [bonus](auto& h) { h += bonus - h * std::abs(bonus) / HISTORY_MAX; };
  gravity(historyTable[pieceTo]);
#ifndef SN_NO_CONTHIST
  for (int back = 1; back <= 2 && back <= ply; back++)
    if (pieceToStack[ply - back] >= 0) {
      int16_t& h = contHist[size_t(pieceToStack[ply - back]) * PIECE_TO + pieceTo];
      h = int16_t(h + bonus - h * std::abs(bonus) / HISTORY_MAX);
    }
#else
  (void)ply;
#endif
}

int Engine::negamax(int depth, int alpha, int beta, int ply, bool allowNull) {
  if ((++nodes & 2047) == 0 && timeUp()) stopped = true;
  if (stopped) return 0;
  Position& p = *pos;
  const Move excludedMove = excluded[ply];

  if (ply > 0) {
    if (p.halfmove >= 100 || p.repetitions() > 0) return 0;
    // Mate-distance pruning
    alpha = std::max(alpha, -MATE + ply);
    beta = std::min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
  }
  if (ply >= MAX_PLY - 1) return evaluate(p);

  const bool inCheck = p.inCheck();
  if (inCheck) depth++;
  if (depth <= 0) return quiesce(alpha, beta, ply);

  // The singular-extension verification search revisits this node without the TT move; it must not
  // take the TT result for this position (that already includes the excluded move).
  const TTEntry* e = excludedMove ? nullptr : probe(p.key);
  Move ttMove = 0;
  int ttScore = 0, ttDepth = -1, ttFlag = TT_NONE;
  if (e) {
    ttMove = e->move;
    ttDepth = e->depth;
    ttFlag = e->flagGen & 3;
    ttScore = e->score;
    if (ttScore > MATE_BOUND) ttScore -= ply;
    else if (ttScore < -MATE_BOUND) ttScore += ply;
    if (ply > 0 && ttDepth >= depth &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)))
      return ttScore;
  }

  const bool pvNode = beta - alpha > 1;
  const int staticEval = inCheck ? -INF : evaluate(p);
  if (!inCheck && !pvNode && !excludedMove && std::abs(beta) < MATE_BOUND) {
    // Reverse futility pruning: far above beta near the leaves, assume it holds.
    if (depth <= 3 && staticEval - 120 * depth >= beta) return staticEval;
    // Null-move pruning
    if (allowNull && depth >= 3 && staticEval >= beta && p.hasNonPawnMaterial(p.side)) {
      const int R = 3 + depth / 6 + std::min(2, (staticEval - beta) / 200);
      pieceToStack[ply] = -1;
      p.makeNull();
      const int s = -negamax(depth - 1 - R, -beta, -beta + 1, ply + 1, false);
      p.unmakeNull();
      if (stopped) return 0;
      if (s >= beta) return s > MATE_BOUND ? beta : s;
    }
  }
  // Internal iterative reduction: without a hash move ordering is poor, so search a bit shallower.
  if (depth >= 4 && !ttMove) depth--;

  Move moves[MAX_MOVES];
  int scores[MAX_MOVES];
  const int n = p.genMoves(moves, false);
  scoreMoves(moves, scores, n, ttMove, ply);
  const Move k0 = killers[ply][0], k1 = killers[ply][1];
  const int origAlpha = alpha;
  const bool canPrune = !pvNode && !inCheck;
  int quietsTried[MAX_MOVES], quietCount = 0;
  int best = -INF, legal = 0;
  Move bestMove = 0;

  for (int i = 0; i < n; i++) {
    const Move m = next(moves, scores, n, i);
    if (m == excludedMove) continue;
    const int to = mTo(m);
    const bool quiet = isQuiet(p, m);
    const int movingPiece = p.board[mFrom(m)];
    const int pieceTo = (movingPiece + 6) * 128 + to;

    bool futile = false;
    if (canPrune && legal > 0 && quiet && best > -MATE_BOUND) {
      // Late-move pruning: at low depth, quiet moves this late in the ordering rarely matter.
      if (depth <= 4 && quietCount >= 3 + 2 * depth * depth) continue;
      // Futility pruning: a quiet move can't lift a hopeless static eval up to alpha.
      futile = depth <= 3 && staticEval + 100 + 90 * depth <= alpha;
    }

    int extension = 0;
#ifndef SN_NO_SINGULAR
    // Singular extension: if the TT move beats every alternative by a clear margin, search it deeper.
    // If even the alternatives beat beta, this node is very likely a cutoff (multi-cut).
    if (m == ttMove && ply > 0 && depth >= 8 && !excludedMove && ttDepth >= depth - 3 &&
        (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && std::abs(ttScore) < MATE_BOUND) {
      const int singularBeta = ttScore - 2 * depth;
      excluded[ply] = m;
      const int s = negamax((depth - 1) / 2, singularBeta - 1, singularBeta, ply, false);
      excluded[ply] = 0;
      if (stopped) return 0;
      if (s < singularBeta) extension = 1;
      else if (singularBeta >= beta) return singularBeta;
    }
#endif

    if (!p.makeMove(m)) continue;
    const bool givesCheck = p.inCheck();
    if (futile && !givesCheck) { p.unmakeMove(); continue; }
    legal++;
    pieceToStack[ply] = pieceTo;
    const int newDepth = depth - 1 + extension;

    int score;
    if (legal == 1) {
      score = -negamax(newDepth, -beta, -alpha, ply + 1, true);
    } else {
      // Late-move reductions for quiet moves, then PVS re-searches when needed.
      int r = 0;
      if (depth >= 3 && quiet && !inCheck && !givesCheck && legal > (pvNode ? 3 : 2)) {
        r = LMR.r[std::min(depth, 63)][std::min(legal, 63)];
        if (pvNode) r--;
        if (m == k0 || m == k1) r--;
#ifndef SN_NO_HISTORY_LMR
        // Reduce moves with a good history (and continuation history) less, and bad ones more.
        r -= std::max(-2, std::min(2, quietScore(pieceTo, ply) / SN_HLMR_DIV));
#endif
        r = std::max(0, std::min(r, newDepth - 1));
      }
      score = -negamax(newDepth - r, -alpha - 1, -alpha, ply + 1, true);
      if (score > alpha && r) score = -negamax(newDepth, -alpha - 1, -alpha, ply + 1, true);
      if (score > alpha && score < beta) score = -negamax(newDepth, -beta, -alpha, ply + 1, true);
    }
    p.unmakeMove();
    if (stopped) return 0;

    if (score > best) {
      best = score;
      bestMove = m;
      if (score > alpha) {
        alpha = score;
        if (ply == 0) rootBest = m;
        if (alpha >= beta) {
          if (quiet) {
            if (k0 != m) { killers[ply][1] = k0; killers[ply][0] = m; }
            const int bonus = std::min(SN_HIST_SCALE * depth * depth, SN_HIST_SCALE * 400);
            updateQuiet(pieceTo, ply, bonus);
            // Quiet moves tried before the cutoff were worse; lower their scores.
            for (int j = 0; j < quietCount; j++) updateQuiet(quietsTried[j], ply, -bonus);
          }
          break;
        }
      }
    }
    if (quiet) quietsTried[quietCount++] = pieceTo;
  }

  if (!legal) {
    // In a verification search the excluded move may be the only legal one: that's a forced move, not mate.
    if (excludedMove) return alpha;
    return inCheck ? -MATE + ply : 0;
  }

  if (!excludedMove) {
    const int flag = best <= origAlpha ? TT_UPPER : best >= beta ? TT_LOWER : TT_EXACT;
    store(p.key, bestMove, best > MATE_BOUND ? best + ply : best < -MATE_BOUND ? best - ply : best, depth, flag);
  }
  return best;
}

int Engine::quiesce(int alpha, int beta, int ply) {
  if ((++nodes & 2047) == 0 && timeUp()) stopped = true;
  if (stopped) return 0;
  Position& p = *pos;
  const int standPat = evaluate(p);
  if (ply >= MAX_PLY - 1 || standPat >= beta) return standPat;
  alpha = std::max(alpha, standPat);

  Move moves[MAX_MOVES];
  int scores[MAX_MOVES];
  const int n = p.genMoves(moves, true);
  scoreMoves(moves, scores, n, 0, ply);
  int best = standPat;
  for (int i = 0; i < n; i++) {
    const Move m = next(moves, scores, n, i);
    if (scores[i] < 0) break;  // only material-losing captures remain
    const int victim = (mFlags(m) & FLAG_EP) ? PAWN : std::abs(p.board[mTo(m)]);
    // Delta pruning: skip captures that can't raise alpha even with a margin.
    if (!mPromo(m) && standPat + VALUE[victim] + 200 < alpha) continue;
    if (!p.makeMove(m)) continue;
    const int score = -quiesce(-beta, -alpha, ply + 1);
    p.unmakeMove();
    if (stopped) return 0;
    if (score > best) {
      best = score;
      if (score > alpha) {
        alpha = score;
        if (alpha >= beta) break;
      }
    }
  }
  return best;
}

}  // namespace sn
