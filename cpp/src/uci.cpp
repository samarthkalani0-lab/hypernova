// UCI front end. Run `supernova` and type UCI commands, or point a chess GUI at the executable.
// Command-line arguments are run as commands before reading stdin, e.g. `supernova bench` or
// `supernova "position startpos" "perft 5" quit`.
// `supernova "datagen <out.bin> <games> <nodes> <seed>" quit` writes self-play training data (see datagen.h).
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "datagen.h"
#include "eval.h"
#include "nnue.h"
#include "search.h"

using namespace sn;

#ifndef SN_NAME
#define SN_NAME "Hypernova"
#endif
#ifndef SN_VERSION
#define SN_VERSION "1.0"
#endif
#ifndef SN_AUTHOR
#define SN_AUTHOR "Samarth Kalani"
#endif

namespace {

std::mutex outMutex;
void say(const std::string& line) {
  std::lock_guard<std::mutex> lock(outMutex);
  std::cout << line << std::endl;
}

// "position [startpos | fen <fen>] [moves m1 m2 ...]"
Position buildPosition(std::istringstream& in) {
  std::string token, fen;
  in >> token;
  if (token == "fen") {
    while (in >> token && token != "moves") fen += token + " ";
  } else {
    fen = START_FEN;
    in >> token;  // consume "moves" if present
  }
  Position pos(fen);
  if (token == "moves") {
    while (in >> token) {
      const Move m = pos.parseUci(token);
      if (!m) break;
      pos.makeMove(m);
    }
  }
  return pos;
}

Limits parseGo(std::istringstream& in) {
  Limits l;
  std::string key;
  while (in >> key) {
    if (key == "infinite") l.infinite = true;
    else if (key == "depth") in >> l.depth;
    else if (key == "movetime") in >> l.movetime;
    else if (key == "wtime") in >> l.wtime;
    else if (key == "btime") in >> l.btime;
    else if (key == "winc") in >> l.winc;
    else if (key == "binc") in >> l.binc;
    else if (key == "movestogo") in >> l.movestogo;
    else if (key == "nodes") in >> l.nodes;
  }
  return l;
}

std::string formatInfo(const Info& info) {
  std::ostringstream s;
  const int64_t nps = int64_t(info.nodes * 1000 / uint64_t(std::max<int64_t>(info.timeMs, 1)));
  s << "info depth " << info.depth << " score " << (info.isMate ? "mate " : "cp ")
    << (info.isMate ? info.mate : info.score) << " nodes " << info.nodes << " nps " << nps << " time "
    << info.timeMs << " pv";
  for (Move m : info.pv) s << ' ' << moveToUci(m);
  return s.str();
}

// Fixed-depth searches on a few positions: a quick speed check and a node-count signature.
void bench(Engine& engine, int depth) {
  const char* fens[] = {
      START_FEN,
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
      "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
      "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
  };
  uint64_t total = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const char* fen : fens) {
    Position pos(fen);
    engine.clear();
    Limits l;
    l.depth = depth;
    const Result r = engine.search(pos, l);
    total += r.nodes;
    say(std::string(fen) + "  bestmove " + moveToUci(r.bestMove) + "  nodes " + std::to_string(r.nodes));
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  say("bench: " + std::to_string(total) + " nodes, " + std::to_string(ms) + " ms, " +
      std::to_string(total * 1000 / uint64_t(std::max<int64_t>(ms, 1))) + " nps");
}

}  // namespace

int main(int argc, char** argv) {
  Engine engine(64);
  Position pos;
  std::thread searchThread;

  auto stopSearch = [&] {
    engine.stop = true;
    if (searchThread.joinable()) searchThread.join();
  };

  std::vector<std::string> queued(argv + 1, argv + argc);
  size_t nextQueued = 0;
  std::string line;
  while (nextQueued < queued.size() ? (line = queued[nextQueued++], true) : bool(std::getline(std::cin, line))) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "uci") {
      say("id name " SN_NAME " " SN_VERSION "\nid author " SN_AUTHOR "\n"
          "option name Hash type spin default 64 min 1 max 4096\n"
          "option name Threads type spin default 1 min 1 max 256\n"
          "option name EvalFile type string default <empty>\nuciok");
    } else if (cmd == "isready") {
      say("readyok");
    } else if (cmd == "setoption") {
      std::string word, name, value;
      in >> word >> name >> word >> value;  // "name Hash value N"
      if (name == "Hash" && !value.empty()) { stopSearch(); engine.setHashSize(std::stoi(value)); }
      if (name == "Threads" && !value.empty()) { stopSearch(); engine.setThreads(std::stoi(value)); }
      if (name == "EvalFile" && !value.empty() && value != "<empty>") {
        stopSearch();
        const std::string err = nnue::loadFile(value);
        say("info string " + (err.empty() ? "loaded network " + value : err));
        pos.load(pos.fen());
      }
    } else if (cmd == "ucinewgame") {
      stopSearch();
      engine.clear();
    } else if (cmd == "position") {
      stopSearch();
      try {
        pos = buildPosition(in);
      } catch (const std::exception& e) {
        say(std::string("info string ") + e.what());
      }
    } else if (cmd == "go") {
      stopSearch();
      const Limits limits = parseGo(in);
      engine.stop = false;
      searchThread = std::thread([&engine, limits, searchPos = pos]() mutable {
        const Result r = engine.search(searchPos, limits, [](const Info& info) { say(formatInfo(info)); });
        say("bestmove " + (r.bestMove ? moveToUci(r.bestMove) : std::string("0000")));
      });
    } else if (cmd == "stop") {
      stopSearch();
    } else if (cmd == "quit") {
      break;
    } else if (cmd == "d") {
      say(pos.toString());
    } else if (cmd == "eval") {
      say(std::string(pos.useNet ? "nnue" : "handcrafted") + " eval " + std::to_string(evaluate(pos)) + " (side to move)");
    } else if (cmd == "perft") {
      int depth = 1;
      in >> depth;
      const auto start = std::chrono::steady_clock::now();
      const uint64_t n = pos.perft(depth);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      say("nodes " + std::to_string(n) + " time " + std::to_string(ms) + "ms");
    } else if (cmd == "bench") {
      int depth = 12;
      in >> depth;
      stopSearch();
      engine.stop = false;
      bench(engine, depth);
    } else if (cmd == "datagen") {
      std::string outPath;
      int games = 100;
      uint64_t nodes = 5000, seed = 1;
      in >> outPath >> games >> nodes >> seed;
      stopSearch();
      try {
        say("datagen done " + std::to_string(datagen(outPath, games, nodes, seed)) + " positions");
      } catch (const std::exception& e) {
        say(std::string("info string ") + e.what());
        return 1;
      }
    } else if (!cmd.empty()) {
      say("info string unknown command: " + cmd);
    }
  }
  stopSearch();
  return 0;
}
