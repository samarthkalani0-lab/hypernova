#include "position.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "eval.h"

namespace sn {

const char* const START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

namespace {

const char PIECE_CHARS[] = ".pnbrqk";

struct Zobrist {
  uint64_t piece[13 * 128], castle[16], ep[128], side;
  Zobrist() {
    uint64_t s = 0x2545F4914F6CDD1Dull;
    auto next = [&s] {  // splitmix64
      uint64_t z = (s += 0x9E3779B97F4A7C15ull);
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      return z ^ (z >> 31);
    };
    for (auto& k : piece) k = next();
    for (auto& k : castle) k = next();
    for (auto& k : ep) k = next();
    side = next();
  }
};
const Zobrist Z;

// Castling rights bits: 1 = white O-O, 2 = white O-O-O, 4 = black O-O, 8 = black O-O-O.
// Moving from or to one of these squares clears the matching rights.
struct CastleMask {
  uint8_t m[128];
  CastleMask() {
    std::fill(m, m + 128, 15);
    m[0] = 13; m[4] = 12; m[7] = 14;
    m[112] = 7; m[116] = 3; m[119] = 11;
  }
};
const CastleMask CASTLE_MASK;

int sqFromName(const std::string& s) { return (s[1] - '1') * 16 + (s[0] - 'a'); }

}  // namespace

std::string sqName(int sq) {
  return std::string(1, char('a' + (sq & 7))) + char('1' + (sq >> 4));
}

std::string moveToUci(Move m) {
  std::string s = sqName(mFrom(m)) + sqName(mTo(m));
  if (mPromo(m)) s += PIECE_CHARS[mPromo(m)];
  return s;
}

Position::Position(const std::string& fen) {
  history.reserve(1024);
  load(fen);
}

void Position::load(const std::string& fen) {
  std::istringstream in(fen);
  std::string placement, stm = "w", cast = "-", eps = "-";
  int hm = 0, fm = 1;
  in >> placement >> stm >> cast >> eps >> hm >> fm;
  std::fill(board, board + 128, 0);
  int r = 7, f = 0;
  for (char ch : placement) {
    if (ch == '/') { r--; f = 0; continue; }
    if (ch >= '1' && ch <= '8') { f += ch - '0'; continue; }
    const char* p = std::strchr(PIECE_CHARS, std::tolower(static_cast<unsigned char>(ch)));
    const int t = p ? int(p - PIECE_CHARS) : 0;
    if (t <= 0 || t > KING || r < 0 || f > 7) throw std::invalid_argument("Invalid FEN: " + fen);
    const int pc = std::isupper(static_cast<unsigned char>(ch)) ? t : -t;
    const int sq = r * 16 + f;
    board[sq] = int8_t(pc);
    if (t == KING) kings[pc > 0 ? 0 : 1] = sq;
    f++;
  }
  side = stm == "b" ? BLACK : WHITE;
  castling = 0;
  for (char c : cast) {
    if (c == 'K') castling |= 1;
    if (c == 'Q') castling |= 2;
    if (c == 'k') castling |= 4;
    if (c == 'q') castling |= 8;
  }
  ep = eps.size() == 2 ? sqFromName(eps) : -1;
  halfmove = hm;
  fullmove = fm > 0 ? fm : 1;
  history.clear();
  computeHash();
  useNet = nnue::net != nullptr;
  accTop = 0;
  if (useNet) {
    if (acc.empty()) acc.resize(256);
    nnue::refresh(acc[0], board);
  }
}

void Position::computeHash() {
  key = 0; pawnKey = 0; mg = 0; eg = 0; phase = 0;
  std::fill(counts, counts + 13, 0);
  std::fill(&bb[0][0], &bb[0][0] + 14, 0);
  occ = 0;
  for (int sq = 0; sq < 128; sq++) {
    if (offBoard(sq)) { sq += 7; continue; }
    if (board[sq]) { accumulate(board[sq], sq, 1); put(board[sq], sq); }
  }
  key ^= Z.castle[castling];
  if (ep != -1) key ^= Z.ep[ep];
  if (side == BLACK) key ^= Z.side;
}

// Adds (sign = 1) or removes (sign = -1) a piece's contribution to hash, eval and phase.
void Position::accumulate(int p, int sq, int sign) {
  const int i = (p + 6) * 128 + sq;
  key ^= Z.piece[i];
  mg += sign * MG_PSQ[i];
  eg += sign * EG_PSQ[i];
  phase += sign * PHASE_INC[std::abs(p)];
  if (p == PAWN || p == -PAWN) pawnKey ^= Z.piece[i];
}

void Position::put(int p, int sq) {
  const Bitboard b = bit(to64(sq));
  Bitboard* c = bb[p > 0 ? 0 : 1];
  board[sq] = int8_t(p);
  counts[p + 6]++;
  c[0] |= b; c[std::abs(p)] |= b; occ |= b;
}
void Position::take(int p, int sq) {
  const Bitboard b = bit(to64(sq));
  Bitboard* c = bb[p > 0 ? 0 : 1];
  board[sq] = 0;
  counts[p + 6]--;
  c[0] ^= b; c[std::abs(p)] ^= b; occ ^= b;
}
void Position::addPiece(int p, int sq) {
  put(p, sq);
  accumulate(p, sq, 1);
  if (useNet) nnue::update(acc[accTop], p, to64(sq), 1);
}
void Position::removePiece(int p, int sq) {
  take(p, sq);
  accumulate(p, sq, -1);
  if (useNet) nnue::update(acc[accTop], p, to64(sq), -1);
}

std::string Position::fen() const {
  std::string s;
  for (int r = 7; r >= 0; r--) {
    int empty = 0;
    for (int f = 0; f < 8; f++) {
      const int p = board[r * 16 + f];
      if (!p) { empty++; continue; }
      if (empty) { s += char('0' + empty); empty = 0; }
      const char ch = PIECE_CHARS[std::abs(p)];
      s += p > 0 ? char(std::toupper(ch)) : ch;
    }
    if (empty) s += char('0' + empty);
    if (r) s += '/';
  }
  std::string c;
  if (castling & 1) c += 'K';
  if (castling & 2) c += 'Q';
  if (castling & 4) c += 'k';
  if (castling & 8) c += 'q';
  std::ostringstream out;
  out << s << (side == WHITE ? " w " : " b ") << (c.empty() ? "-" : c) << ' ' << (ep == -1 ? "-" : sqName(ep)) << ' '
      << halfmove << ' ' << fullmove;
  return out.str();
}

std::string Position::toString() const {
  std::string s;
  for (int r = 7; r >= 0; r--) {
    s += char('1' + r);
    s += ' ';
    for (int f = 0; f < 8; f++) {
      const int p = board[r * 16 + f];
      const char ch = PIECE_CHARS[std::abs(p)];
      s += p > 0 ? char(std::toupper(ch)) : ch;
      s += ' ';
    }
    s += '\n';
  }
  return s + "  a b c d e f g h\n" + fen();
}

bool Position::isAttacked(int sq, int by) const {
  const int s = to64(sq), c = by == WHITE ? 0 : 1;
  const Bitboard* p = bb[c];
  return (PAWN_ATT[c ^ 1][s] & p[PAWN]) || (KNIGHT_ATT[s] & p[KNIGHT]) || (KING_ATT[s] & p[KING]) ||
         (bishopAttacks(s, occ) & (p[BISHOP] | p[QUEEN])) || (rookAttacks(s, occ) & (p[ROOK] | p[QUEEN]));
}

// Pieces of both colours attacking sq64, with sliders blocked by `occupied`.
Bitboard Position::attackersTo(int s, Bitboard occupied) const {
  const Bitboard* w = bb[0];
  const Bitboard* b = bb[1];
  return (PAWN_ATT[1][s] & w[PAWN]) | (PAWN_ATT[0][s] & b[PAWN]) | (KNIGHT_ATT[s] & (w[KNIGHT] | b[KNIGHT])) |
         (KING_ATT[s] & (w[KING] | b[KING])) |
         (bishopAttacks(s, occupied) & (w[BISHOP] | b[BISHOP] | w[QUEEN] | b[QUEEN])) |
         (rookAttacks(s, occupied) & (w[ROOK] | b[ROOK] | w[QUEEN] | b[QUEEN]));
}

int Position::genMoves(Move* out, bool capturesOnly) const {
  int n = 0;
  const int c = side == WHITE ? 0 : 1;
  const Bitboard* own = bb[c];
  const Bitboard enemy = bb[c ^ 1][0];
  const Bitboard targets = capturesOnly ? enemy : ~own[0];
  auto addAll = [&](int from64, Bitboard to) {
    const int from = to88(from64);
    while (to) out[n++] = encode(from, to88(popLsb(to)), 0, 0);
  };

  const Bitboard promoFrom = side == WHITE ? RANK_7 : RANK_2, doubleFrom = side == WHITE ? RANK_2 : RANK_7;
  const int epBit = ep != -1 ? to64(ep) : -1;
  for (Bitboard pawns = own[PAWN]; pawns;) {
    const int s = popLsb(pawns), sq = to88(s), fwd = s + 8 * side;
    const bool promoting = promoFrom & bit(s);
    if (!(occ & bit(fwd))) {
      if (promoting) {
        out[n++] = encode(sq, to88(fwd), QUEEN, 0);
        if (!capturesOnly) {
          out[n++] = encode(sq, to88(fwd), KNIGHT, 0);
          out[n++] = encode(sq, to88(fwd), ROOK, 0);
          out[n++] = encode(sq, to88(fwd), BISHOP, 0);
        }
      } else if (!capturesOnly) {
        out[n++] = encode(sq, to88(fwd), 0, 0);
        if ((doubleFrom & bit(s)) && !(occ & bit(fwd + 8 * side))) out[n++] = encode(sq, to88(fwd + 8 * side), 0, FLAG_DOUBLE);
      }
    }
    for (Bitboard caps = PAWN_ATT[c][s] & enemy; caps;) {
      const int t = to88(popLsb(caps));
      out[n++] = encode(sq, t, promoting ? QUEEN : 0, 0);
      if (promoting && !capturesOnly) {
        out[n++] = encode(sq, t, KNIGHT, 0);
        out[n++] = encode(sq, t, ROOK, 0);
        out[n++] = encode(sq, t, BISHOP, 0);
      }
    }
    if (epBit != -1 && (PAWN_ATT[c][s] & bit(epBit))) out[n++] = encode(sq, ep, 0, FLAG_EP);
  }
  for (Bitboard b = own[KNIGHT]; b;) { const int s = popLsb(b); addAll(s, KNIGHT_ATT[s] & targets); }
  for (Bitboard b = own[BISHOP]; b;) { const int s = popLsb(b); addAll(s, bishopAttacks(s, occ) & targets); }
  for (Bitboard b = own[ROOK]; b;) { const int s = popLsb(b); addAll(s, rookAttacks(s, occ) & targets); }
  for (Bitboard b = own[QUEEN]; b;) {
    const int s = popLsb(b);
    addAll(s, (bishopAttacks(s, occ) | rookAttacks(s, occ)) & targets);
  }
  const int k = lsb(own[KING]);
  addAll(k, KING_ATT[k] & targets);

  if (!capturesOnly && castling) {
    const int opp = -side;
    if (side == WHITE) {
      if ((castling & 1) && !(occ & 0x60ull) && !isAttacked(4, opp) && !isAttacked(5, opp) && !isAttacked(6, opp))
        out[n++] = encode(4, 6, 0, FLAG_CASTLE);
      if ((castling & 2) && !(occ & 0x0Eull) && !isAttacked(4, opp) && !isAttacked(3, opp) && !isAttacked(2, opp))
        out[n++] = encode(4, 2, 0, FLAG_CASTLE);
    } else {
      if ((castling & 4) && !(occ & (0x60ull << 56)) && !isAttacked(116, opp) && !isAttacked(117, opp) &&
          !isAttacked(118, opp))
        out[n++] = encode(116, 118, 0, FLAG_CASTLE);
      if ((castling & 8) && !(occ & (0x0Eull << 56)) && !isAttacked(116, opp) && !isAttacked(115, opp) &&
          !isAttacked(114, opp))
        out[n++] = encode(116, 114, 0, FLAG_CASTLE);
    }
  }
  return n;
}

bool Position::makeMove(Move m) {
  const int from = mFrom(m), to = mTo(m), promo = mPromo(m), flags = mFlags(m);
  const int piece = board[from], s = side, captured = board[to];
  history.push_back({m, int8_t(captured), uint8_t(castling), ep, halfmove, key, pawnKey, mg, eg, phase});
  if (useNet) {
    if (accTop + 1 >= int(acc.size())) acc.resize(acc.size() * 2);
    nnue::copy(acc[accTop + 1], acc[accTop]);
    accTop++;
  }

  if (ep != -1) key ^= Z.ep[ep];
  removePiece(piece, from);
  if (captured) removePiece(captured, to);
  addPiece(promo ? promo * s : piece, to);

  if (flags & FLAG_EP) {
    const int cs = to - 16 * s;
    removePiece(board[cs], cs);
  } else if (flags & FLAG_CASTLE) {
    const int rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
    const int rook = board[rf];
    removePiece(rook, rf);
    addPiece(rook, rt);
  }
  if (piece * s == KING) kings[s == WHITE ? 0 : 1] = to;

  key ^= Z.castle[castling];
  castling &= CASTLE_MASK.m[from] & CASTLE_MASK.m[to];
  key ^= Z.castle[castling];

  // Only record an en-passant square when a capture there is actually possible,
  // so identical positions hash identically (matters for repetition detection).
  ep = -1;
  if (flags & FLAG_DOUBLE) {
    const int enemyPawn = -s * PAWN;
    if ((!offBoard(to - 1) && board[to - 1] == enemyPawn) || (!offBoard(to + 1) && board[to + 1] == enemyPawn)) {
      ep = from + 16 * s;
      key ^= Z.ep[ep];
    }
  }

  halfmove = piece * s == PAWN || captured ? 0 : halfmove + 1;
  if (s == BLACK) fullmove++;
  side = -s;
  key ^= Z.side;

  if (isAttacked(kings[s == WHITE ? 0 : 1], -s)) { unmakeMove(); return false; }
  return true;
}

void Position::unmakeMove() {
  const Undo u = history.back();
  history.pop_back();
  if (useNet) accTop--;
  const int from = mFrom(u.m), to = mTo(u.m), promo = mPromo(u.m), flags = mFlags(u.m);
  side = -side;
  const int s = side;
  const int moved = board[to], piece = promo ? PAWN * s : moved;
  take(moved, to);
  put(piece, from);
  if (u.captured) put(u.captured, to);
  if (flags & FLAG_EP) {
    put(-s * PAWN, to - 16 * s);
  } else if (flags & FLAG_CASTLE) {
    const int rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
    const int rook = board[rt];
    take(rook, rt);
    put(rook, rf);
  }
  if (piece * s == KING) kings[s == WHITE ? 0 : 1] = from;
  if (s == BLACK) fullmove--;
  castling = u.castling; ep = u.ep; halfmove = u.halfmove;
  key = u.key; pawnKey = u.pawnKey; mg = u.mg; eg = u.eg; phase = u.phase;
}

void Position::makeNull() {
  history.push_back({0, 0, uint8_t(castling), ep, halfmove, key, pawnKey, mg, eg, phase});
  if (ep != -1) { key ^= Z.ep[ep]; ep = -1; }
  halfmove = 0;  // repetition scans must not look past a null move
  side = -side;
  key ^= Z.side;
}

void Position::unmakeNull() {
  const Undo u = history.back();
  history.pop_back();
  side = -side;
  ep = u.ep; halfmove = u.halfmove; key = u.key;
}

std::vector<Move> Position::legalMoves() {
  Move buf[MAX_MOVES];
  const int n = genMoves(buf, false);
  std::vector<Move> out;
  for (int i = 0; i < n; i++)
    if (makeMove(buf[i])) { unmakeMove(); out.push_back(buf[i]); }
  return out;
}

Move Position::parseUci(const std::string& uci) {
  for (Move m : legalMoves())
    if (moveToUci(m) == uci) return m;
  return 0;
}

// How many earlier positions in the reversible stretch equal the current one.
int Position::repetitions() const {
  const int len = int(history.size());
  int n = 0;
  for (int i = len - 2; i >= 0 && i >= len - halfmove; i -= 2)
    if (history[i].key == key) n++;
  return n;
}

bool Position::hasNonPawnMaterial(int s) const {
  return counts[KNIGHT * s + 6] + counts[BISHOP * s + 6] + counts[ROOK * s + 6] + counts[QUEEN * s + 6] > 0;
}

bool Position::insufficientMaterial() const {
  const int8_t* c = counts;
  if (c[6 + PAWN] || c[6 - PAWN] || c[6 + ROOK] || c[6 - ROOK] || c[6 + QUEEN] || c[6 - QUEEN]) return false;
  return c[6 + KNIGHT] + c[6 - KNIGHT] + c[6 + BISHOP] + c[6 - BISHOP] <= 1;
}

// Expected material gain of capture m if both sides keep recapturing with their cheapest attacker
// (and may stop whenever continuing would lose). Removing each capturer from the occupancy
// uncovers sliders behind it (x-rays).
int Position::see(Move m) {
  const int from = mFrom(m), to = mTo(m), t = to64(to);
  int gain[40];
  gain[0] = (mFlags(m) & FLAG_EP) ? VALUE[PAWN] : VALUE[std::abs(board[to])];
  Bitboard o = occ ^ bit(to64(from));
  if (mFlags(m) & FLAG_EP) o ^= bit(to64(to - 16 * side));
  const Bitboard diag = bb[0][BISHOP] | bb[1][BISHOP] | bb[0][QUEEN] | bb[1][QUEEN];
  const Bitboard straight = bb[0][ROOK] | bb[1][ROOK] | bb[0][QUEEN] | bb[1][QUEEN];
  Bitboard attackers = attackersTo(t, o) & o;
  int attackerValue = VALUE[std::abs(board[from])];
  int s = -side, d = 0;
  for (;;) {
    d++;
    gain[d] = attackerValue - gain[d - 1];
    if (std::max(-gain[d - 1], gain[d]) < 0 || d >= 38) break;
    const Bitboard* mine = bb[s == WHITE ? 0 : 1];
    if (!(attackers & mine[0])) break;
    int type = PAWN;
    while (!(attackers & mine[type])) type++;
    const Bitboard pick = attackers & mine[type];
    o ^= pick & (0 - pick);
    if (type == PAWN || type == BISHOP || type == QUEEN) attackers |= bishopAttacks(t, o) & diag;
    if (type == ROOK || type == QUEEN) attackers |= rookAttacks(t, o) & straight;
    attackers &= o;
    attackerValue = VALUE[type];
    s = -s;
  }
  while (--d) gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
  return gain[0];
}

uint64_t Position::perft(int depth) {
  if (depth == 0) return 1;
  Move buf[MAX_MOVES];
  const int n = genMoves(buf, false);
  uint64_t total = 0;
  for (int i = 0; i < n; i++) {
    if (!makeMove(buf[i])) continue;
    total += perft(depth - 1);
    unmakeMove();
  }
  return total;
}

}  // namespace sn
