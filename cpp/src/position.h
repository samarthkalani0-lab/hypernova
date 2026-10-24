#pragma once
#include <string>
#include <vector>

#include "bitboard.h"
#include "nnue.h"
#include "types.h"

namespace sn {

struct Undo {
  Move m;
  int8_t captured;
  uint8_t castling;
  int ep, halfmove;
  uint64_t key, pawnKey;
  int mg, eg, phase;
};

class Position {
 public:
  int8_t board[128];
  Bitboard bb[2][7];  // [white, black][piece type]; index 0 holds all pieces of that colour
  Bitboard occ;
  int kings[2];      // [white, black]
  int8_t counts[13];  // piece counts indexed by piece + 6
  int side, castling, ep, halfmove, fullmove;
  uint64_t key, pawnKey;
  int mg, eg, phase;  // incrementally maintained evaluation terms (White minus Black)
  std::vector<Undo> history;
  // NNUE accumulators, one per made move (used when a network was loaded before load()).
  bool useNet = false;
  int accTop = 0;
  std::vector<nnue::Accumulator> acc;

  explicit Position(const std::string& fen = START_FEN);
  void load(const std::string& fen);
  std::string fen() const;
  std::string toString() const;

  bool isAttacked(int sq, int by) const;
  bool inCheck() const { return isAttacked(kings[side == WHITE ? 0 : 1], -side); }

  // Pseudo-legal moves into out (size >= MAX_MOVES); returns the count. makeMove() rejects illegal ones.
  // capturesOnly also includes queen promotions (for quiescence search).
  int genMoves(Move* out, bool capturesOnly) const;
  bool makeMove(Move m);  // false (position unchanged) if the move leaves the king in check
  void unmakeMove();
  void makeNull();
  void unmakeNull();

  std::vector<Move> legalMoves();
  Move parseUci(const std::string& uci);
  int repetitions() const;
  bool hasNonPawnMaterial(int s) const;
  bool insufficientMaterial() const;
  int see(Move m);  // static exchange evaluation, in centipawns
  uint64_t perft(int depth);

 private:
  Bitboard attackersTo(int sq64, Bitboard occupied) const;
  void computeHash();
  void accumulate(int p, int sq, int sign);
  void put(int p, int sq);   // board and bitboards only
  void take(int p, int sq);
  void addPiece(int p, int sq);
  void removePiece(int p, int sq);
};

}  // namespace sn
