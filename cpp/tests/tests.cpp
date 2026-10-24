// Move-generator and search checks. Run: supernova_tests [--quick]
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "search.h"

using namespace sn;

namespace {

struct PerftCase {
  const char* fen;
  std::vector<uint64_t> counts;
};

// Reference node counts from https://www.chessprogramming.org/Perft_Results
const PerftCase PERFT[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", {20, 400, 8902, 197281, 4865609, 119060324}},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", {48, 2039, 97862, 4085603, 193690690}},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", {14, 191, 2812, 43238, 674624, 11030083}},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", {6, 264, 9467, 422333, 15833292}},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", {44, 1486, 62379, 2103487, 89941194}},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", {46, 2079, 89890, 3894594, 164075551}},
};

struct TacticCase {
  const char* name;
  const char* fen;
  std::vector<std::string> expected;
};

const TacticCase TACTICS[] = {
    {"mate in 1 (back rank)", "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", {"a1a8"}},
    {"scholar's mate", "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4", {"h5f7"}},
    {"knight fork wins the queen", "2q3k1/5ppp/8/3N4/8/8/5PPP/6K1 w - - 0 1", {"d5e7"}},
    {"avoid stalemate, mate instead", "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1", {"f7g7", "f7f8"}},
};

}  // namespace

int main(int argc, char** argv) {
  const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
  int failed = 0;

  for (const PerftCase& c : PERFT) {
    Position pos(c.fen);
    const size_t maxDepth = quick ? std::min<size_t>(c.counts.size(), 4) : c.counts.size();
    for (size_t d = 1; d <= maxDepth; d++) {
      const auto start = std::chrono::steady_clock::now();
      const uint64_t n = pos.perft(int(d));
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      const bool ok = n == c.counts[d - 1];
      if (!ok) failed++;
      if (!ok || d == maxDepth)
        std::cout << (ok ? "ok   " : "FAIL ") << "perft " << d << " = " << n
                  << (ok ? "" : " (expected " + std::to_string(c.counts[d - 1]) + ")") << "  " << ms << "ms  " << c.fen << "\n";
    }
    if (pos.fen() != Position(c.fen).fen()) { failed++; std::cout << "FAIL position not restored: " << c.fen << "\n"; }
  }

  // SEE: pawn takes pawn defended by a pawn, with a rook backing up the recapture.
  {
    Position pos("4k3/8/2p5/3p4/4P3/8/8/3RK3 w - - 0 1");
    const int s = pos.see(pos.parseUci("e4d5"));
    if (s != 100) failed++;
    std::cout << (s == 100 ? "ok   " : "FAIL ") << "SEE e4xd5 = " << s << "\n";
  }

  for (const TacticCase& t : TACTICS) {
    Position pos(t.fen);
    Engine engine(16);
    Limits l;
    l.depth = 8;
    l.movetime = 5000;
    const Result r = engine.search(pos, l);
    const std::string uci = moveToUci(r.bestMove);
    bool ok = false;
    for (const std::string& e : t.expected) ok |= e == uci;
    if (!ok) failed++;
    std::cout << (ok ? "ok   " : "FAIL ") << t.name << ": " << uci << "  depth " << r.depth << ", score " << r.score << "\n";
  }

  std::cout << (failed ? "\n" + std::to_string(failed) + " check(s) FAILED\n" : "\nAll checks passed\n");
  return failed ? 1 : 0;
}
