/*
 * Supernova chess engine.
 * 0x88 board, pseudo-legal move generation with make/unmake legality checks,
 * Zobrist hashing, iterative-deepening PVS alpha-beta with a transposition table,
 * null-move pruning, late-move reductions, killer/history ordering and quiescence search.
 * Runs unchanged in Node (require) and the browser (window.Chess).
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Chess = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6;
  const WHITE = 1, BLACK = -1;
  const FLAG_EP = 1, FLAG_CASTLE = 2, FLAG_DOUBLE = 4;

  const KNIGHT_OFFSETS = [33, 31, 18, 14, -33, -31, -18, -14];
  const BISHOP_DIRS = [15, 17, -15, -17];
  const ROOK_DIRS = [1, -1, 16, -16];
  const KING_DIRS = [1, -1, 16, -16, 15, 17, -15, -17];

  const PIECE_CHARS = '.pnbrqk';
  const FILES = 'abcdefgh';
  const START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

  // Castling rights bits: 1 = white O-O, 2 = white O-O-O, 4 = black O-O, 8 = black O-O-O.
  // Moving from or to one of these squares clears the matching rights.
  const CASTLE_MASK = new Uint8Array(128).fill(15);
  CASTLE_MASK[0] = 13; CASTLE_MASK[4] = 12; CASTLE_MASK[7] = 14;
  CASTLE_MASK[112] = 7; CASTLE_MASK[116] = 3; CASTLE_MASK[119] = 11;

  // Move encoding: from | to << 7 | promo << 14 | flags << 17
  const encode = (from, to, promo, flags) => from | (to << 7) | (promo << 14) | (flags << 17);
  const mFrom = m => m & 127;
  const mTo = m => (m >> 7) & 127;
  const mPromo = m => (m >> 14) & 7;
  const mFlags = m => (m >> 17) & 7;

  const sqName = sq => FILES[sq & 7] + ((sq >> 4) + 1);
  const sqFromName = s => (s.charCodeAt(1) - 49) * 16 + (s.charCodeAt(0) - 97);

  function moveToUci(m) {
    const p = mPromo(m);
    return sqName(mFrom(m)) + sqName(mTo(m)) + (p ? PIECE_CHARS[p] : '');
  }

  // ---------- Zobrist keys (two 32-bit halves: lo indexes the TT, hi verifies) ----------
  let seed = 0x2545F491;
  function rand32() { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return seed | 0; }
  const Z_PIECE_LO = new Int32Array(13 * 128), Z_PIECE_HI = new Int32Array(13 * 128);
  const Z_CASTLE_LO = new Int32Array(16), Z_CASTLE_HI = new Int32Array(16);
  const Z_EP_LO = new Int32Array(128), Z_EP_HI = new Int32Array(128);
  for (const t of [Z_PIECE_LO, Z_PIECE_HI, Z_CASTLE_LO, Z_CASTLE_HI, Z_EP_LO, Z_EP_HI])
    for (let i = 0; i < t.length; i++) t[i] = rand32();
  const Z_SIDE_LO = rand32(), Z_SIDE_HI = rand32();

  // ---------- Evaluation tables (PeSTO, by Ronald Friederich) ----------
  // Middlegame/endgame values are blended by game phase. Tables are from White's view, rank 8 first.
  const MG_VALUE = [0, 82, 337, 365, 477, 1025, 0];
  const EG_VALUE = [0, 94, 281, 297, 512, 936, 0];
  const PHASE_INC = [0, 0, 1, 1, 2, 4, 0];
  const MG_TABLE = {
    [PAWN]: [
      0, 0, 0, 0, 0, 0, 0, 0,
      98, 134, 61, 95, 68, 126, 34, -11,
      -6, 7, 26, 31, 65, 56, 25, -20,
      -14, 13, 6, 21, 23, 12, 17, -23,
      -27, -2, -5, 12, 17, 6, 10, -25,
      -26, -4, -4, -10, 3, 3, 33, -12,
      -35, -1, -20, -23, -15, 24, 38, -22,
      0, 0, 0, 0, 0, 0, 0, 0],
    [KNIGHT]: [
      -167, -89, -34, -49, 61, -97, -15, -107,
      -73, -41, 72, 36, 23, 62, 7, -17,
      -47, 60, 37, 65, 84, 129, 73, 44,
      -9, 17, 19, 53, 37, 69, 18, 22,
      -13, 4, 16, 13, 28, 19, 21, -8,
      -23, -9, 12, 10, 19, 17, 25, -16,
      -29, -53, -12, -3, -1, 18, -14, -19,
      -105, -21, -58, -33, -17, -28, -19, -23],
    [BISHOP]: [
      -29, 4, -82, -37, -25, -42, 7, -8,
      -26, 16, -18, -13, 30, 59, 18, -47,
      -16, 37, 43, 40, 35, 50, 37, -2,
      -4, 5, 19, 50, 37, 37, 7, -2,
      -6, 13, 13, 26, 34, 12, 10, 4,
      0, 15, 15, 15, 14, 27, 18, 10,
      4, 15, 16, 0, 7, 21, 33, 1,
      -33, -3, -14, -21, -13, -12, -39, -21],
    [ROOK]: [
      32, 42, 32, 51, 63, 9, 31, 43,
      27, 32, 58, 62, 80, 67, 26, 44,
      -5, 19, 26, 36, 17, 45, 61, 16,
      -24, -11, 7, 26, 24, 35, -8, -20,
      -36, -26, -12, -1, 9, -7, 6, -23,
      -45, -25, -16, -17, 3, 0, -5, -33,
      -44, -16, -20, -9, -1, 11, -6, -71,
      -19, -13, 1, 17, 16, 7, -37, -26],
    [QUEEN]: [
      -28, 0, 29, 12, 59, 44, 43, 45,
      -24, -39, -5, 1, -16, 57, 28, 54,
      -13, -17, 7, 8, 29, 56, 47, 57,
      -27, -27, -16, -16, -1, 17, -2, 1,
      -9, -26, -9, -10, -2, -4, 3, -3,
      -14, 2, -11, -2, -5, 2, 14, 5,
      -35, -8, 11, 2, 8, 15, -3, 1,
      -1, -18, -9, 10, -15, -25, -31, -50],
    [KING]: [
      -65, 23, 16, -15, -56, -34, 2, 13,
      29, -1, -20, -7, -8, -4, -38, -29,
      -9, 24, 2, -16, -20, 6, 22, -22,
      -17, -20, -12, -27, -30, -25, -14, -36,
      -49, -1, -27, -39, -46, -44, -33, -51,
      -14, -14, -22, -46, -44, -30, -15, -27,
      1, 7, -8, -64, -43, -16, 9, 8,
      -15, 36, 12, -54, 8, -28, 24, 14],
  };
  const EG_TABLE = {
    [PAWN]: [
      0, 0, 0, 0, 0, 0, 0, 0,
      178, 173, 158, 134, 147, 132, 165, 187,
      94, 100, 85, 67, 56, 53, 82, 84,
      32, 24, 13, 5, -2, 4, 17, 17,
      13, 9, -3, -7, -7, -8, 3, -1,
      4, 7, -6, 1, 0, -5, -1, -8,
      13, 8, 8, 10, 13, 0, 2, -7,
      0, 0, 0, 0, 0, 0, 0, 0],
    [KNIGHT]: [
      -58, -38, -13, -28, -31, -27, -63, -99,
      -25, -8, -25, -2, -9, -25, -24, -52,
      -24, -20, 10, 9, -1, -9, -19, -41,
      -17, 3, 22, 22, 22, 11, 8, -18,
      -18, -6, 16, 25, 16, 17, 4, -18,
      -23, -3, -1, 15, 10, -3, -20, -22,
      -42, -20, -10, -5, -2, -20, -23, -44,
      -29, -51, -23, -15, -22, -18, -50, -64],
    [BISHOP]: [
      -14, -21, -11, -8, -7, -9, -17, -24,
      -8, -4, 7, -12, -3, -13, -4, -14,
      2, -8, 0, -1, -2, 6, 0, 4,
      -3, 9, 12, 9, 14, 10, 3, 2,
      -6, 3, 13, 19, 7, 10, -3, -9,
      -12, -3, 8, 10, 13, 3, -7, -15,
      -14, -18, -7, -1, 4, -9, -15, -27,
      -23, -9, -23, -5, -9, -16, -5, -17],
    [ROOK]: [
      13, 10, 18, 15, 12, 12, 8, 5,
      11, 13, 13, 11, -3, 3, 8, 3,
      7, 7, 7, 5, 4, -3, -5, -3,
      4, 3, 13, 1, 2, 1, -1, 2,
      3, 5, 8, 4, -5, -6, -8, -11,
      -4, 0, -5, -1, -7, -12, -8, -16,
      -6, -6, 0, 2, -9, -9, -11, -3,
      -9, 2, 3, -1, -5, -13, 4, -20],
    [QUEEN]: [
      -9, 22, 22, 27, 27, 19, 10, 20,
      -17, 20, 32, 41, 58, 25, 30, 0,
      -20, 6, 9, 49, 47, 35, 19, 9,
      3, 22, 24, 45, 57, 40, 57, 36,
      -18, 28, 19, 47, 31, 34, 39, 23,
      -16, -27, 15, 6, 9, 17, 10, 5,
      -22, -23, -30, -16, -16, -23, -36, -32,
      -33, -28, -22, -43, -5, -32, -20, -41],
    [KING]: [
      -74, -35, -18, -18, -11, 15, 4, -17,
      -12, 17, 14, 17, 17, 38, 23, 11,
      10, 17, 23, 15, 20, 45, 44, 13,
      -8, 22, 24, 27, 26, 33, 26, 3,
      -18, -4, 21, 24, 27, 23, 9, -11,
      -19, -3, 11, 21, 23, 16, 7, -9,
      -27, -11, 4, 13, 14, 4, -5, -17,
      -53, -34, -21, -11, -28, -14, -24, -43],
  };

  // Signed piece-square scores indexed by (piece + 6) * 128 + sq: White adds, Black subtracts.
  const MG_PSQ = new Int16Array(13 * 128), EG_PSQ = new Int16Array(13 * 128);
  for (let t = PAWN; t <= KING; t++) {
    for (let sq = 0; sq < 128; sq++) {
      if (sq & 0x88) continue;
      const r = sq >> 4, f = sq & 7, w = (7 - r) * 8 + f, bl = r * 8 + f;
      MG_PSQ[(t + 6) * 128 + sq] = MG_VALUE[t] + MG_TABLE[t][w];
      EG_PSQ[(t + 6) * 128 + sq] = EG_VALUE[t] + EG_TABLE[t][w];
      MG_PSQ[(-t + 6) * 128 + sq] = -(MG_VALUE[t] + MG_TABLE[t][bl]);
      EG_PSQ[(-t + 6) * 128 + sq] = -(EG_VALUE[t] + EG_TABLE[t][bl]);
    }
  }

  // ---------- Position ----------
  class Position {
    constructor(fen) {
      this.board = new Int8Array(128);
      this.kings = [0, 0]; // [white, black]
      this.counts = new Int8Array(13); // piece counts indexed by piece + 6
      this.load(fen || START_FEN);
    }

    load(fen) {
      const parts = fen.trim().split(/\s+/);
      const b = this.board;
      b.fill(0);
      let r = 7, f = 0;
      for (const ch of parts[0]) {
        if (ch === '/') { r--; f = 0; continue; }
        if (ch >= '1' && ch <= '8') { f += +ch; continue; }
        const t = PIECE_CHARS.indexOf(ch.toLowerCase());
        if (t <= 0 || r < 0 || f > 7) throw new Error('Invalid FEN: ' + fen);
        const p = ch === ch.toUpperCase() ? t : -t;
        const sq = r * 16 + f;
        b[sq] = p;
        if (t === KING) this.kings[p > 0 ? 0 : 1] = sq;
        f++;
      }
      this.side = parts[1] === 'b' ? BLACK : WHITE;
      const c = parts[2] || '-';
      this.castling = (c.includes('K') ? 1 : 0) | (c.includes('Q') ? 2 : 0) | (c.includes('k') ? 4 : 0) | (c.includes('q') ? 8 : 0);
      this.ep = parts[3] && parts[3] !== '-' ? sqFromName(parts[3]) : -1;
      this.halfmove = parseInt(parts[4], 10) || 0;
      this.fullmove = parseInt(parts[5], 10) || 1;
      this.history = [];
      this.computeHash();
    }

    // Recomputes the hash and the incrementally maintained evaluation terms from the board.
    computeHash() {
      this.lo = 0; this.hi = 0;
      this.mg = 0; this.eg = 0; this.phase = 0; this.pawnKey = 0;
      this.counts.fill(0);
      for (let sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        const p = this.board[sq];
        if (p) { this.accumulate(p, sq, 1); this.counts[p + 6]++; }
      }
      this.lo ^= Z_CASTLE_LO[this.castling]; this.hi ^= Z_CASTLE_HI[this.castling];
      if (this.ep !== -1) { this.lo ^= Z_EP_LO[this.ep]; this.hi ^= Z_EP_HI[this.ep]; }
      if (this.side === BLACK) { this.lo ^= Z_SIDE_LO; this.hi ^= Z_SIDE_HI; }
    }

    // Adds (sign = 1) or removes (sign = -1) a piece's contribution to hash, eval and phase.
    accumulate(p, sq, sign) {
      const i = (p + 6) * 128 + sq;
      this.lo ^= Z_PIECE_LO[i]; this.hi ^= Z_PIECE_HI[i];
      this.mg += sign * MG_PSQ[i]; this.eg += sign * EG_PSQ[i];
      this.phase += sign * PHASE_INC[p > 0 ? p : -p];
      if (p === PAWN || p === -PAWN) this.pawnKey ^= Z_PIECE_LO[i];
    }

    addPiece(p, sq) { this.board[sq] = p; this.counts[p + 6]++; this.accumulate(p, sq, 1); }
    removePiece(p, sq) { this.board[sq] = 0; this.counts[p + 6]--; this.accumulate(p, sq, -1); }

    fen() {
      let s = '';
      for (let r = 7; r >= 0; r--) {
        let empty = 0;
        for (let f = 0; f < 8; f++) {
          const p = this.board[r * 16 + f];
          if (!p) { empty++; continue; }
          if (empty) { s += empty; empty = 0; }
          const ch = PIECE_CHARS[Math.abs(p)];
          s += p > 0 ? ch.toUpperCase() : ch;
        }
        if (empty) s += empty;
        if (r) s += '/';
      }
      let c = '';
      if (this.castling & 1) c += 'K';
      if (this.castling & 2) c += 'Q';
      if (this.castling & 4) c += 'k';
      if (this.castling & 8) c += 'q';
      return `${s} ${this.side === WHITE ? 'w' : 'b'} ${c || '-'} ${this.ep === -1 ? '-' : sqName(this.ep)} ${this.halfmove} ${this.fullmove}`;
    }

    toString() {
      let s = '';
      for (let r = 7; r >= 0; r--) {
        s += (r + 1) + ' ';
        for (let f = 0; f < 8; f++) {
          const p = this.board[r * 16 + f];
          const ch = PIECE_CHARS[Math.abs(p)];
          s += (p > 0 ? ch.toUpperCase() : ch) + ' ';
        }
        s += '\n';
      }
      return s + '  a b c d e f g h\n' + this.fen();
    }

    isAttacked(sq, by) {
      const b = this.board;
      if (by === WHITE) {
        if (!((sq - 15) & 0x88) && b[sq - 15] === PAWN) return true;
        if (!((sq - 17) & 0x88) && b[sq - 17] === PAWN) return true;
      } else {
        if (!((sq + 15) & 0x88) && b[sq + 15] === -PAWN) return true;
        if (!((sq + 17) & 0x88) && b[sq + 17] === -PAWN) return true;
      }
      const n = KNIGHT * by, k = KING * by;
      for (let i = 0; i < 8; i++) {
        let t = sq + KNIGHT_OFFSETS[i];
        if (!(t & 0x88) && b[t] === n) return true;
        t = sq + KING_DIRS[i];
        if (!(t & 0x88) && b[t] === k) return true;
      }
      const bq = BISHOP * by, rq = ROOK * by, q = QUEEN * by;
      for (let i = 0; i < 4; i++) {
        let d = BISHOP_DIRS[i], t = sq + d;
        while (!(t & 0x88)) {
          const p = b[t];
          if (p) { if (p === bq || p === q) return true; break; }
          t += d;
        }
        d = ROOK_DIRS[i]; t = sq + d;
        while (!(t & 0x88)) {
          const p = b[t];
          if (p) { if (p === rq || p === q) return true; break; }
          t += d;
        }
      }
      return false;
    }

    inCheck() {
      return this.isAttacked(this.kings[this.side === WHITE ? 0 : 1], -this.side);
    }

    // Pseudo-legal moves; makeMove() rejects those that leave the king in check.
    // capturesOnly also includes queen promotions (for quiescence search).
    genMoves(capturesOnly) {
      const moves = [];
      const b = this.board, side = this.side;
      for (let sq = 0; sq < 128; sq++) {
        if (sq & 0x88) { sq += 7; continue; }
        const p = b[sq];
        if (p * side <= 0) continue;
        const type = p * side;
        if (type === PAWN) {
          const dir = 16 * side, fwd = sq + dir, rank = sq >> 4;
          const promoting = rank === (side === WHITE ? 6 : 1);
          if (!(fwd & 0x88) && !b[fwd]) {
            if (promoting) {
              moves.push(encode(sq, fwd, QUEEN, 0));
              if (!capturesOnly) {
                moves.push(encode(sq, fwd, KNIGHT, 0), encode(sq, fwd, ROOK, 0), encode(sq, fwd, BISHOP, 0));
              }
            } else if (!capturesOnly) {
              moves.push(encode(sq, fwd, 0, 0));
              if (rank === (side === WHITE ? 1 : 6) && !b[fwd + dir]) moves.push(encode(sq, fwd + dir, 0, FLAG_DOUBLE));
            }
          }
          for (let c = fwd - 1; c <= fwd + 1; c += 2) {
            if (c & 0x88) continue;
            if (b[c] * side < 0) {
              if (promoting) {
                moves.push(encode(sq, c, QUEEN, 0));
                if (!capturesOnly) moves.push(encode(sq, c, KNIGHT, 0), encode(sq, c, ROOK, 0), encode(sq, c, BISHOP, 0));
              } else moves.push(encode(sq, c, 0, 0));
            } else if (c === this.ep) moves.push(encode(sq, c, 0, FLAG_EP));
          }
        } else if (type === KNIGHT || type === KING) {
          const offs = type === KNIGHT ? KNIGHT_OFFSETS : KING_DIRS;
          for (let i = 0; i < 8; i++) {
            const t = sq + offs[i];
            if (t & 0x88) continue;
            const q = b[t];
            if (q ? q * side < 0 : !capturesOnly) moves.push(encode(sq, t, 0, 0));
          }
        } else {
          const dirs = type === BISHOP ? BISHOP_DIRS : type === ROOK ? ROOK_DIRS : KING_DIRS;
          for (let i = 0; i < dirs.length; i++) {
            const d = dirs[i];
            let t = sq + d;
            while (!(t & 0x88)) {
              const q = b[t];
              if (q) { if (q * side < 0) moves.push(encode(sq, t, 0, 0)); break; }
              if (!capturesOnly) moves.push(encode(sq, t, 0, 0));
              t += d;
            }
          }
        }
      }
      if (!capturesOnly && this.castling) {
        const opp = -side;
        if (side === WHITE) {
          if ((this.castling & 1) && !b[5] && !b[6] && !this.isAttacked(4, opp) && !this.isAttacked(5, opp) && !this.isAttacked(6, opp))
            moves.push(encode(4, 6, 0, FLAG_CASTLE));
          if ((this.castling & 2) && !b[1] && !b[2] && !b[3] && !this.isAttacked(4, opp) && !this.isAttacked(3, opp) && !this.isAttacked(2, opp))
            moves.push(encode(4, 2, 0, FLAG_CASTLE));
        } else {
          if ((this.castling & 4) && !b[117] && !b[118] && !this.isAttacked(116, opp) && !this.isAttacked(117, opp) && !this.isAttacked(118, opp))
            moves.push(encode(116, 118, 0, FLAG_CASTLE));
          if ((this.castling & 8) && !b[113] && !b[114] && !b[115] && !this.isAttacked(116, opp) && !this.isAttacked(115, opp) && !this.isAttacked(114, opp))
            moves.push(encode(116, 114, 0, FLAG_CASTLE));
        }
      }
      return moves;
    }

    // Returns false (and leaves the position unchanged) if the move is illegal.
    makeMove(m) {
      const b = this.board;
      const from = m & 127, to = (m >> 7) & 127, promo = (m >> 14) & 7, flags = (m >> 17) & 7;
      const piece = b[from], side = this.side;
      const captured = b[to];
      this.history.push({
        m, captured, castling: this.castling, ep: this.ep, halfmove: this.halfmove, lo: this.lo, hi: this.hi,
        mg: this.mg, eg: this.eg, phase: this.phase, pawnKey: this.pawnKey,
      });

      if (this.ep !== -1) { this.lo ^= Z_EP_LO[this.ep]; this.hi ^= Z_EP_HI[this.ep]; }
      this.removePiece(piece, from);
      if (captured) this.removePiece(captured, to);
      this.addPiece(promo ? promo * side : piece, to);

      if (flags & FLAG_EP) {
        const cs = to - 16 * side;
        this.removePiece(b[cs], cs);
      } else if (flags & FLAG_CASTLE) {
        const rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
        const rook = b[rf];
        this.removePiece(rook, rf);
        this.addPiece(rook, rt);
      }
      if (piece * side === KING) this.kings[side === WHITE ? 0 : 1] = to;

      this.lo ^= Z_CASTLE_LO[this.castling]; this.hi ^= Z_CASTLE_HI[this.castling];
      this.castling &= CASTLE_MASK[from] & CASTLE_MASK[to];
      this.lo ^= Z_CASTLE_LO[this.castling]; this.hi ^= Z_CASTLE_HI[this.castling];

      // Only record an en-passant square when a capture there is actually possible,
      // so identical positions hash identically (matters for repetition detection).
      this.ep = -1;
      if (flags & FLAG_DOUBLE) {
        const enemyPawn = -side * PAWN;
        if ((!((to - 1) & 0x88) && b[to - 1] === enemyPawn) || (!((to + 1) & 0x88) && b[to + 1] === enemyPawn)) {
          this.ep = from + 16 * side;
          this.lo ^= Z_EP_LO[this.ep]; this.hi ^= Z_EP_HI[this.ep];
        }
      }

      this.halfmove = piece * side === PAWN || captured ? 0 : this.halfmove + 1;
      if (side === BLACK) this.fullmove++;
      this.side = -side;
      this.lo ^= Z_SIDE_LO; this.hi ^= Z_SIDE_HI;

      if (this.isAttacked(this.kings[side === WHITE ? 0 : 1], -side)) { this.unmakeMove(); return false; }
      return true;
    }

    unmakeMove() {
      const u = this.history.pop();
      const m = u.m, b = this.board;
      const from = m & 127, to = (m >> 7) & 127, promo = (m >> 14) & 7, flags = (m >> 17) & 7;
      this.side = -this.side;
      const side = this.side;
      const counts = this.counts;
      const piece = promo ? PAWN * side : b[to];
      if (promo) { counts[b[to] + 6]--; counts[piece + 6]++; }
      b[from] = piece;
      b[to] = u.captured;
      if (u.captured) counts[u.captured + 6]++;
      if (flags & FLAG_EP) { b[to - 16 * side] = -side * PAWN; counts[-side * PAWN + 6]++; }
      else if (flags & FLAG_CASTLE) {
        const rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
        b[rf] = b[rt]; b[rt] = 0;
      }
      if (piece * side === KING) this.kings[side === WHITE ? 0 : 1] = from;
      if (side === BLACK) this.fullmove--;
      this.castling = u.castling; this.ep = u.ep; this.halfmove = u.halfmove;
      this.lo = u.lo; this.hi = u.hi;
      this.mg = u.mg; this.eg = u.eg; this.phase = u.phase; this.pawnKey = u.pawnKey;
    }

    makeNull() {
      this.history.push({ m: 0, captured: 0, castling: this.castling, ep: this.ep, halfmove: this.halfmove, lo: this.lo, hi: this.hi });
      if (this.ep !== -1) { this.lo ^= Z_EP_LO[this.ep]; this.hi ^= Z_EP_HI[this.ep]; this.ep = -1; }
      this.halfmove = 0; // repetition scans must not look past a null move
      this.side = -this.side;
      this.lo ^= Z_SIDE_LO; this.hi ^= Z_SIDE_HI;
    }

    unmakeNull() {
      const u = this.history.pop();
      this.side = -this.side;
      this.ep = u.ep; this.halfmove = u.halfmove; this.lo = u.lo; this.hi = u.hi;
    }

    legalMoves() {
      const out = [];
      for (const m of this.genMoves(false)) if (this.makeMove(m)) { this.unmakeMove(); out.push(m); }
      return out;
    }

    // How many earlier positions in the reversible stretch equal the current one.
    repetitions() {
      const h = this.history;
      let n = 0;
      for (let i = h.length - 2; i >= 0 && i >= h.length - this.halfmove; i -= 2)
        if (h[i].lo === this.lo && h[i].hi === this.hi) n++;
      return n;
    }

    hasNonPawnMaterial(side) {
      const c = this.counts;
      return c[KNIGHT * side + 6] + c[BISHOP * side + 6] + c[ROOK * side + 6] + c[QUEEN * side + 6] > 0;
    }

    insufficientMaterial() {
      const c = this.counts;
      if (c[6 + PAWN] || c[6 - PAWN] || c[6 + ROOK] || c[6 - ROOK] || c[6 + QUEEN] || c[6 - QUEEN]) return false;
      return c[6 + KNIGHT] + c[6 - KNIGHT] + c[6 + BISHOP] + c[6 - BISHOP] <= 1;
    }

    // null if the game goes on, else { result: '1-0' | '0-1' | '1/2-1/2', reason }
    gameResult() {
      if (!this.legalMoves().length) {
        if (this.inCheck()) return { result: this.side === WHITE ? '0-1' : '1-0', reason: 'checkmate' };
        return { result: '1/2-1/2', reason: 'stalemate' };
      }
      if (this.halfmove >= 100) return { result: '1/2-1/2', reason: 'fifty-move rule' };
      if (this.repetitions() >= 2) return { result: '1/2-1/2', reason: 'threefold repetition' };
      if (this.insufficientMaterial()) return { result: '1/2-1/2', reason: 'insufficient material' };
      return null;
    }

    parseUci(str) {
      return this.legalMoves().find(m => moveToUci(m) === str) || 0;
    }

    san(m) {
      const b = this.board, from = mFrom(m), to = mTo(m), promo = mPromo(m), flags = mFlags(m);
      const p = b[from], type = Math.abs(p);
      let s;
      if (flags & FLAG_CASTLE) s = (to & 7) === 6 ? 'O-O' : 'O-O-O';
      else {
        const capture = b[to] !== 0 || (flags & FLAG_EP);
        if (type === PAWN) {
          s = (capture ? FILES[from & 7] + 'x' : '') + sqName(to) + (promo ? '=' + 'NBRQ'[promo - 2] : '');
        } else {
          s = 'PNBRQK'[type - 1];
          const rivals = this.legalMoves().filter(o => o !== m && mTo(o) === to && b[mFrom(o)] === p);
          if (rivals.length) {
            const sameFile = rivals.some(o => (mFrom(o) & 7) === (from & 7));
            const sameRank = rivals.some(o => (mFrom(o) >> 4) === (from >> 4));
            s += !sameFile ? FILES[from & 7] : !sameRank ? String((from >> 4) + 1) : sqName(from);
          }
          s += (capture ? 'x' : '') + sqName(to);
        }
      }
      this.makeMove(m);
      if (this.inCheck()) s += this.legalMoves().length ? '+' : '#';
      this.unmakeMove();
      return s;
    }

    // Square of the least valuable `side` piece attacking `sq`, or -1.
    leastValuableAttacker(sq, side) {
      const b = this.board;
      const p1 = sq - 15 * side, p2 = sq - 17 * side, pawn = PAWN * side;
      if (!(p1 & 0x88) && b[p1] === pawn) return p1;
      if (!(p2 & 0x88) && b[p2] === pawn) return p2;
      const knight = KNIGHT * side;
      for (let i = 0; i < 8; i++) {
        const t = sq + KNIGHT_OFFSETS[i];
        if (!(t & 0x88) && b[t] === knight) return t;
      }
      // Sliders: nearest piece along each ray, cheapest type wins.
      let best = -1, bestType = 7;
      for (let i = 0; i < 8; i++) {
        const d = KING_DIRS[i], diagonal = i >= 4;
        let t = sq + d;
        while (!(t & 0x88) && !b[t]) t += d;
        if (t & 0x88) continue;
        const type = b[t] * side;
        if ((type === QUEEN || type === (diagonal ? BISHOP : ROOK)) && type < bestType) { best = t; bestType = type; }
      }
      if (best !== -1) return best;
      const king = KING * side;
      for (let i = 0; i < 8; i++) {
        const t = sq + KING_DIRS[i];
        if (!(t & 0x88) && b[t] === king) return t;
      }
      return -1;
    }

    // Static exchange evaluation: expected material gain of capture m if both sides keep recapturing
    // with their cheapest attacker (and may stop whenever continuing would lose).
    see(m) {
      const b = this.board, from = m & 127, to = (m >> 7) & 127;
      const gain = SEE_GAIN, saved = SEE_SAVED;
      let n = 0;
      saved[n++] = from; saved[n++] = b[from];
      gain[0] = (m >> 17) & FLAG_EP ? VALUE[PAWN] : VALUE[Math.abs(b[to])];
      if ((m >> 17) & FLAG_EP) { const cs = to - 16 * this.side; saved[n++] = cs; saved[n++] = b[cs]; b[cs] = 0; }
      let attackerValue = VALUE[Math.abs(b[from])];
      b[from] = 0;
      let side = -this.side, d = 0;
      for (;;) {
        d++;
        gain[d] = attackerValue - gain[d - 1];
        if (Math.max(-gain[d - 1], gain[d]) < 0) break;
        const sq = this.leastValuableAttacker(to, side);
        if (sq < 0) break;
        attackerValue = VALUE[Math.abs(b[sq])];
        saved[n++] = sq; saved[n++] = b[sq];
        b[sq] = 0;
        side = -side;
      }
      while (--d) gain[d - 1] = -Math.max(-gain[d - 1], gain[d]);
      for (let i = n - 2; i >= 0; i -= 2) b[saved[i]] = saved[i + 1];
      return gain[0];
    }

    perft(depth) {
      if (depth === 0) return 1;
      let n = 0;
      for (const m of this.genMoves(false)) {
        if (!this.makeMove(m)) continue;
        n += this.perft(depth - 1);
        this.unmakeMove();
      }
      return n;
    }
  }

  // ---------- Evaluation ----------
  const SEE_GAIN = new Int32Array(40), SEE_SAVED = new Int32Array(80);

  // Rough piece values used for move ordering, pruning margins and SEE.
  const VALUE = [0, 100, 320, 330, 500, 900, 20000];

  // Pawn-structure terms by relative rank (0 = own back rank).
  const PASSED_MG = [0, 0, 5, 10, 20, 35, 60, 0];
  const PASSED_EG = [0, 5, 10, 20, 35, 60, 90, 0];
  const DOUBLED_MG = -10, DOUBLED_EG = -20, ISOLATED_MG = -8, ISOLATED_EG = -12;

  // Pawn structure only depends on pawns, so it's cached by the pawn-only hash.
  const PAWN_CACHE_SIZE = 1 << 14;
  const pawnCacheKey = new Int32Array(PAWN_CACHE_SIZE);
  const pawnCacheValid = new Uint8Array(PAWN_CACHE_SIZE);
  const pawnCacheMg = new Int16Array(PAWN_CACHE_SIZE), pawnCacheEg = new Int16Array(PAWN_CACHE_SIZE);
  const wFile = new Int8Array(8), bFile = new Int8Array(8), wMinRank = new Int8Array(8), bMaxRank = new Int8Array(8);

  function evalPawns(b) {
    wFile.fill(0); bFile.fill(0); wMinRank.fill(8); bMaxRank.fill(-1);
    for (let sq = 0; sq < 128; sq++) {
      if (sq & 0x88) { sq += 7; continue; }
      const r = sq >> 4, f = sq & 7;
      if (b[sq] === PAWN) { wFile[f]++; if (r < wMinRank[f]) wMinRank[f] = r; }
      else if (b[sq] === -PAWN) { bFile[f]++; if (r > bMaxRank[f]) bMaxRank[f] = r; }
    }
    let mg = 0, eg = 0;
    for (let f = 0; f < 8; f++) {
      const fl = f > 0 ? f - 1 : f, fr = f < 7 ? f + 1 : f;
      if (wFile[f] > 1) { mg += DOUBLED_MG * (wFile[f] - 1); eg += DOUBLED_EG * (wFile[f] - 1); }
      if (bFile[f] > 1) { mg -= DOUBLED_MG * (bFile[f] - 1); eg -= DOUBLED_EG * (bFile[f] - 1); }
      if (wFile[f] && !(f > 0 && wFile[f - 1]) && !(f < 7 && wFile[f + 1])) { mg += ISOLATED_MG * wFile[f]; eg += ISOLATED_EG * wFile[f]; }
      if (bFile[f] && !(f > 0 && bFile[f - 1]) && !(f < 7 && bFile[f + 1])) { mg -= ISOLATED_MG * bFile[f]; eg -= ISOLATED_EG * bFile[f]; }
      // Passed pawns: only the most advanced pawn on a file can be passed.
      for (let r = 6; r >= 1; r--) {
        if (b[r * 16 + f] !== PAWN) continue;
        if (bMaxRank[fl] <= r && bMaxRank[f] <= r && bMaxRank[fr] <= r) { mg += PASSED_MG[r]; eg += PASSED_EG[r]; }
        break;
      }
      for (let r = 1; r <= 6; r++) {
        if (b[r * 16 + f] !== -PAWN) continue;
        if (wMinRank[fl] >= r && wMinRank[f] >= r && wMinRank[fr] >= r) { mg -= PASSED_MG[7 - r]; eg -= PASSED_EG[7 - r]; }
        break;
      }
    }
    return [mg, eg];
  }

  // Middlegame bonus for pawns sheltering a castled king.
  function kingShield(b, ksq, side) {
    const kr = side === WHITE ? ksq >> 4 : 7 - (ksq >> 4), kf = ksq & 7;
    if (kr > 1) return -20;
    let s = 0;
    const pawn = PAWN * side;
    for (let f = Math.max(0, kf - 1); f <= Math.min(7, kf + 1); f++) {
      const r1 = side === WHITE ? kr + 1 : 6 - kr, r2 = side === WHITE ? kr + 2 : 5 - kr;
      if (b[r1 * 16 + f] === pawn) s += 12;
      else if (b[r2 * 16 + f] === pawn) s += 6;
      else s -= 12;
    }
    return s;
  }

  const centerDistance = sq => {
    const r = sq >> 4, f = sq & 7;
    return Math.max(3 - r, r - 4) + Math.max(3 - f, f - 4);
  };

  function nonPawnMaterial(c, side) {
    return 3 * (c[KNIGHT * side + 6] + c[BISHOP * side + 6]) + 5 * c[ROOK * side + 6] + 9 * c[QUEEN * side + 6];
  }

  // Score in centipawns from the side to move's perspective.
  function evaluate(pos) {
    const b = pos.board, c = pos.counts;
    let mg = pos.mg, eg = pos.eg;

    const pi = pos.pawnKey & (PAWN_CACHE_SIZE - 1);
    if (!pawnCacheValid[pi] || pawnCacheKey[pi] !== pos.pawnKey) {
      const [pm, pe] = evalPawns(b);
      pawnCacheKey[pi] = pos.pawnKey; pawnCacheValid[pi] = 1;
      pawnCacheMg[pi] = pm; pawnCacheEg[pi] = pe;
    }
    mg += pawnCacheMg[pi]; eg += pawnCacheEg[pi];

    if (c[6 + BISHOP] >= 2) { mg += 30; eg += 50; }
    if (c[6 - BISHOP] >= 2) { mg -= 30; eg -= 50; }
    mg += kingShield(b, pos.kings[0], WHITE) - kingShield(b, pos.kings[1], BLACK);

    const phase = pos.phase > 24 ? 24 : pos.phase;
    let score = (mg * phase + eg * (24 - phase)) / 24 | 0;

    const strong = score > 0 ? WHITE : BLACK, weak = -strong;
    // Without pawns, a small material edge usually can't win (e.g. R vs B, N+N vs K).
    if (!c[PAWN * strong + 6] && nonPawnMaterial(c, strong) - nonPawnMaterial(c, weak) < 4) score = score / 8 | 0;
    // Mop-up: when winning a pawnless ending, push the losing king to the edge and bring ours closer.
    else if (phase <= 6 && !c[PAWN * weak + 6] && Math.abs(score) > 300) {
      const ks = pos.kings[strong === WHITE ? 0 : 1], kw = pos.kings[strong === WHITE ? 1 : 0];
      const dist = Math.abs((ks >> 4) - (kw >> 4)) + Math.abs((ks & 7) - (kw & 7));
      score += strong * (10 * centerDistance(kw) + 4 * (14 - dist));
    }
    return score * pos.side + 10; // small tempo bonus
  }

  // ---------- Search ----------
  const INF = 32000, MATE = 30000, MATE_BOUND = 29000, MAX_PLY = 100;
  const TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3;
  const HISTORY_MAX = 16384;
  // Move-ordering score bands (bad captures sort below quiet moves).
  const GOOD_CAPTURE = 1000000, UNCHECKED_CAPTURE = 950000, BAD_CAPTURE = -1000000;
  // Late-move reduction amounts by [depth][move number].
  const LMR = Array.from({ length: 64 }, (_, d) =>
    Array.from({ length: 64 }, (_, i) => (d && i ? Math.floor(0.75 + Math.log(d) * Math.log(i) / 2.25) : 0)));
  const now = typeof performance !== 'undefined' ? () => performance.now() : () => Date.now();

  class Engine {
    constructor(ttBits = 20) {
      const size = 1 << ttBits;
      this.ttMask = size - 1;
      this.ttKey = new Int32Array(size);
      this.ttMove = new Int32Array(size);
      this.ttScore = new Int16Array(size);
      this.ttDepth = new Int8Array(size);
      this.ttFlag = new Uint8Array(size);
      this.killers = new Int32Array(MAX_PLY * 2);
      this.historyTable = new Int32Array(13 * 128);
      this.stopFlag = null; // optional Int32Array on a SharedArrayBuffer; set [0] = 1 to stop
    }

    clear() {
      this.ttFlag.fill(0);
      this.historyTable.fill(0);
      this.killers.fill(0);
    }

    timeUp() {
      if (this.stopFlag && Atomics.load(this.stopFlag, 0)) return true;
      return now() - this.startTime >= this.hardLimit;
    }

    /*
     * limits: { depth, movetime, wtime, btime, winc, binc, movestogo, infinite }
     * onInfo({ depth, score, mate, nodes, time, pv }) is called after each completed iteration.
     */
    search(pos, limits = {}, onInfo) {
      this.pos = pos;
      this.nodes = 0;
      this.stopped = false;
      this.startTime = now();
      this.killers.fill(0);
      for (let i = 0; i < this.historyTable.length; i++) this.historyTable[i] >>= 2;

      let soft = Infinity, hard = Infinity;
      if (limits.movetime) soft = hard = limits.movetime;
      else if (limits.wtime != null || limits.btime != null) {
        const time = (pos.side === WHITE ? limits.wtime : limits.btime) || 0;
        const inc = (pos.side === WHITE ? limits.winc : limits.binc) || 0;
        const mtg = limits.movestogo || 30;
        hard = Math.max(5, Math.min(time / mtg + inc * 0.8, time / 2 - 20));
        soft = hard * 0.5;
      }
      if (limits.infinite) soft = hard = Infinity;
      this.hardLimit = hard;
      const maxDepth = Math.min(limits.depth || 64, MAX_PLY - 10);

      const legal = pos.legalMoves();
      let best = legal[0] || 0, bestScore = 0, completed = 0;
      if (legal.length <= 1 && !limits.infinite && !limits.depth) return { bestMove: best, score: 0, depth: 0, nodes: 0 };

      for (let d = 1; d <= maxDepth; d++) {
        this.rootBest = 0;
        // Aspiration window around the previous score; widen and re-search on failure.
        let delta = 25, alpha = -INF, beta = INF, score;
        if (d >= 5 && Math.abs(bestScore) < MATE_BOUND) { alpha = bestScore - delta; beta = bestScore + delta; }
        for (;;) {
          score = this.negamax(d, alpha, beta, 0, false);
          if (this.stopped) break;
          if (score <= alpha) { beta = (alpha + beta) >> 1; alpha = Math.max(-INF, score - delta); }
          else if (score >= beta) beta = Math.min(INF, score + delta);
          else break;
          delta *= 2;
          if (delta > 800) { alpha = -INF; beta = INF; }
        }
        // A move that beat the previous best before time ran out is still trustworthy.
        if (this.rootBest) best = this.rootBest;
        if (this.stopped) break;
        bestScore = score; completed = d;
        if (onInfo) {
          const info = { depth: d, score, nodes: this.nodes, time: Math.round(now() - this.startTime), pv: this.extractPv(d) };
          if (Math.abs(score) > MATE_BOUND) info.mate = score > 0 ? Math.ceil((MATE - score) / 2) : -Math.ceil((MATE + score) / 2);
          onInfo(info);
        }
        if (now() - this.startTime >= soft) break;
        if (!limits.infinite && Math.abs(score) > MATE_BOUND && d >= MATE - Math.abs(score)) break;
      }
      return { bestMove: best, score: bestScore, depth: completed, nodes: this.nodes };
    }

    extractPv(maxLen) {
      const pos = this.pos, pv = [];
      while (pv.length < maxLen) {
        const idx = pos.lo & this.ttMask;
        if (!this.ttFlag[idx] || this.ttKey[idx] !== pos.hi) break;
        const m = this.ttMove[idx];
        if (!m || !pos.genMoves(false).includes(m) || !pos.makeMove(m)) break;
        pv.push(m);
      }
      for (let i = 0; i < pv.length; i++) pos.unmakeMove();
      return pv;
    }

    scoreMoves(moves, ttMove, ply) {
      const pos = this.pos, b = pos.board;
      const scores = new Int32Array(moves.length);
      const k0 = this.killers[ply * 2], k1 = this.killers[ply * 2 + 1];
      for (let i = 0; i < moves.length; i++) {
        const m = moves[i];
        if (m === ttMove) { scores[i] = 10000000; continue; }
        const from = m & 127, to = (m >> 7) & 127, promo = (m >> 14) & 7;
        const victim = (m >> 17) & FLAG_EP ? PAWN : Math.abs(b[to]);
        if (victim || promo) {
          const attacker = Math.abs(b[from]);
          // Taking a cheaper piece might lose material; next() checks those with SEE when they come up.
          const good = promo || VALUE[victim] >= VALUE[attacker];
          scores[i] = (good ? GOOD_CAPTURE : UNCHECKED_CAPTURE) + VALUE[victim] * 10 + VALUE[promo] - attacker;
        }
        else if (m === k0) scores[i] = 900000;
        else if (m === k1) scores[i] = 800000;
        else scores[i] = this.historyTable[(b[from] + 6) * 128 + to];
      }
      return scores;
    }

    // Moves the best-scored remaining move to index i (lazy selection sort).
    static pick(moves, scores, i) {
      let bi = i;
      for (let j = i + 1; j < moves.length; j++) if (scores[j] > scores[bi]) bi = j;
      if (bi !== i) {
        const m = moves[i]; moves[i] = moves[bi]; moves[bi] = m;
        const s = scores[i]; scores[i] = scores[bi]; scores[bi] = s;
      }
      return moves[i];
    }

    // Picks the next move to search, resolving possibly-losing captures with SEE only when reached.
    next(moves, scores, i) {
      for (;;) {
        const m = Engine.pick(moves, scores, i);
        const s = scores[i];
        if (s < UNCHECKED_CAPTURE || s >= GOOD_CAPTURE) return m;
        if (this.pos.see(m) >= 0) { scores[i] = s - UNCHECKED_CAPTURE + GOOD_CAPTURE; return m; }
        scores[i] = s - UNCHECKED_CAPTURE + BAD_CAPTURE;
      }
    }

    // History "gravity" update keeps scores within +-HISTORY_MAX.
    updateHistory(piece, to, bonus) {
      const i = (piece + 6) * 128 + to;
      this.historyTable[i] += bonus - (this.historyTable[i] * Math.abs(bonus) / HISTORY_MAX | 0);
    }

    negamax(depth, alpha, beta, ply, allowNull) {
      if ((++this.nodes & 2047) === 0 && this.timeUp()) this.stopped = true;
      if (this.stopped) return 0;
      const pos = this.pos;

      if (ply > 0) {
        if (pos.halfmove >= 100 || pos.repetitions() > 0) return 0;
        // Mate-distance pruning
        if (alpha < -MATE + ply) alpha = -MATE + ply;
        if (beta > MATE - ply - 1) beta = MATE - ply - 1;
        if (alpha >= beta) return alpha;
      }
      if (ply >= MAX_PLY - 1) return evaluate(pos);

      const inCheck = pos.inCheck();
      if (inCheck) depth++;
      if (depth <= 0) return this.quiesce(alpha, beta, ply);

      const idx = pos.lo & this.ttMask;
      let ttMove = 0;
      if (this.ttFlag[idx] && this.ttKey[idx] === pos.hi) {
        ttMove = this.ttMove[idx];
        if (ply > 0 && this.ttDepth[idx] >= depth) {
          let s = this.ttScore[idx];
          if (s > MATE_BOUND) s -= ply; else if (s < -MATE_BOUND) s += ply;
          const flag = this.ttFlag[idx];
          if (flag === TT_EXACT || (flag === TT_LOWER && s >= beta) || (flag === TT_UPPER && s <= alpha)) return s;
        }
      }

      const pvNode = beta - alpha > 1;
      const staticEval = inCheck ? -INF : evaluate(pos);
      if (!inCheck && !pvNode && Math.abs(beta) < MATE_BOUND) {
        // Reverse futility pruning: far above beta near the leaves, assume it holds.
        if (depth <= 3 && staticEval - 120 * depth >= beta) return staticEval;
        // Null-move pruning
        if (allowNull && depth >= 3 && staticEval >= beta && pos.hasNonPawnMaterial(pos.side)) {
          const R = 3 + (depth / 6 | 0) + Math.min(2, (staticEval - beta) / 200 | 0);
          pos.makeNull();
          const s = -this.negamax(depth - 1 - R, -beta, -beta + 1, ply + 1, false);
          pos.unmakeNull();
          if (this.stopped) return 0;
          if (s >= beta) return s > MATE_BOUND ? beta : s;
        }
      }
      // Internal iterative reduction: without a hash move ordering is poor, so search a bit shallower.
      if (depth >= 4 && !ttMove) depth--;

      const moves = pos.genMoves(false);
      const scores = this.scoreMoves(moves, ttMove, ply);
      const b = pos.board;
      const k0 = this.killers[ply * 2], k1 = this.killers[ply * 2 + 1];
      const origAlpha = alpha;
      const canPrune = !pvNode && !inCheck;
      const quietsSearched = [];
      let best = -INF, bestMove = 0, legal = 0;

      for (let i = 0; i < moves.length; i++) {
        const m = this.next(moves, scores, i);
        const to = (m >> 7) & 127;
        const quiet = !b[to] && !((m >> 14) & 7) && !((m >> 17) & FLAG_EP);
        const movingPiece = b[m & 127];

        let futile = false;
        if (canPrune && legal > 0 && quiet && best > -MATE_BOUND) {
          // Late-move pruning: at low depth, quiet moves this late in the ordering rarely matter.
          if (depth <= 4 && quietsSearched.length / 2 >= 3 + 2 * depth * depth) continue; // two entries per move
          // Futility pruning: a quiet move can't lift a hopeless static eval up to alpha.
          futile = depth <= 3 && staticEval + 100 + 90 * depth <= alpha;
        }

        if (!pos.makeMove(m)) continue;
        const givesCheck = pos.inCheck();
        if (futile && !givesCheck) { pos.unmakeMove(); continue; }
        legal++;

        let score;
        if (legal === 1) {
          score = -this.negamax(depth - 1, -beta, -alpha, ply + 1, true);
        } else {
          // Late-move reductions for quiet moves, then PVS re-searches when needed.
          let r = 0;
          if (depth >= 3 && quiet && !inCheck && !givesCheck && legal > (pvNode ? 3 : 2)) {
            r = LMR[Math.min(depth, 63)][Math.min(legal, 63)];
            if (pvNode) r--;
            if (m === k0 || m === k1) r--;
            r = Math.max(0, Math.min(r, depth - 2));
          }
          score = -this.negamax(depth - 1 - r, -alpha - 1, -alpha, ply + 1, true);
          if (score > alpha && r) score = -this.negamax(depth - 1, -alpha - 1, -alpha, ply + 1, true);
          if (score > alpha && score < beta) score = -this.negamax(depth - 1, -beta, -alpha, ply + 1, true);
        }
        pos.unmakeMove();
        if (this.stopped) return 0;

        if (score > best) {
          best = score;
          bestMove = m;
          if (score > alpha) {
            alpha = score;
            if (ply === 0) this.rootBest = m;
            if (alpha >= beta) {
              if (quiet) {
                if (k0 !== m) {
                  this.killers[ply * 2 + 1] = k0;
                  this.killers[ply * 2] = m;
                }
                const bonus = Math.min(depth * depth, 400);
                this.updateHistory(movingPiece, to, bonus);
                // Quiet moves tried before the cutoff were worse; lower their history.
                for (let j = 0; j < quietsSearched.length; j += 2) this.updateHistory(quietsSearched[j], quietsSearched[j + 1], -bonus);
              }
              break;
            }
          }
        }
        if (quiet) quietsSearched.push(movingPiece, to);
      }

      if (!legal) return inCheck ? -MATE + ply : 0;

      this.ttKey[idx] = pos.hi;
      this.ttMove[idx] = bestMove;
      this.ttDepth[idx] = depth;
      this.ttScore[idx] = best > MATE_BOUND ? best + ply : best < -MATE_BOUND ? best - ply : best;
      this.ttFlag[idx] = best <= origAlpha ? TT_UPPER : best >= beta ? TT_LOWER : TT_EXACT;
      return best;
    }

    quiesce(alpha, beta, ply) {
      if ((++this.nodes & 2047) === 0 && this.timeUp()) this.stopped = true;
      if (this.stopped) return 0;
      const pos = this.pos;
      const standPat = evaluate(pos);
      if (ply >= MAX_PLY - 1 || standPat >= beta) return standPat;
      if (standPat > alpha) alpha = standPat;

      const moves = pos.genMoves(true);
      const scores = this.scoreMoves(moves, 0, ply);
      const b = pos.board;
      let best = standPat;
      for (let i = 0; i < moves.length; i++) {
        const m = this.next(moves, scores, i);
        if (scores[i] < 0) break; // only material-losing captures remain
        const victim = (m >> 17) & FLAG_EP ? PAWN : Math.abs(b[(m >> 7) & 127]);
        // Delta pruning: skip captures that can't raise alpha even with a margin.
        if (!((m >> 14) & 7) && standPat + VALUE[victim] + 200 < alpha) continue;
        if (!pos.makeMove(m)) continue;
        const score = -this.quiesce(-beta, -alpha, ply + 1);
        pos.unmakeMove();
        if (this.stopped) return 0;
        if (score > best) {
          best = score;
          if (score > alpha) { alpha = score; if (alpha >= beta) break; }
        }
      }
      return best;
    }
  }

  return {
    Position, Engine, evaluate, moveToUci, sqName, sqFromName,
    START_FEN, WHITE, BLACK, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    mFrom, mTo, mPromo, mFlags, MATE_BOUND,
  };
});
