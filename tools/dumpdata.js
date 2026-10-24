#!/usr/bin/env node
// Decodes self-play data records (format in cpp/src/datagen.h) and checks they are sane.
// Usage: node tools/dumpdata.js <file.bin | dir> [--show N]
// Prints the first N positions as FEN with score and result, then checks every record: one king
// per side, no pawns on the back ranks, and the side not to move is not in check.
const fs = require('fs');
const path = require('path');
const { Position, WHITE, BLACK } = require('../engine.js');

const args = process.argv.slice(2);
const target = args[0];
const show = args.includes('--show') ? +args[args.indexOf('--show') + 1] : 5;
if (!target) { console.error('usage: node tools/dumpdata.js <file.bin | dir> [--show N]'); process.exit(1); }
const files = fs.statSync(target).isDirectory()
  ? fs.readdirSync(target).filter(f => f.endsWith('.bin')).map(f => path.join(target, f))
  : [target];

const LETTERS = 'pnbrqk';
function toFen(buf, o) {
  const occ = buf.readBigUInt64LE(o);
  const board = new Array(64).fill(null);
  let n = 0;
  for (let sq = 0; sq < 64; sq++) {
    if (!((occ >> BigInt(sq)) & 1n)) continue;
    const nib = (buf[o + 8 + (n >> 1)] >> (4 * (n & 1))) & 15;
    const ch = LETTERS[nib & 7];
    board[sq] = nib & 8 ? ch : ch.toUpperCase();
    n++;
  }
  const ranks = [];
  for (let r = 7; r >= 0; r--) {
    let s = '', empty = 0;
    for (let f = 0; f < 8; f++) {
      const p = board[r * 8 + f];
      if (!p) { empty++; continue; }
      if (empty) { s += empty; empty = 0; }
      s += p;
    }
    ranks.push(s + (empty || ''));
  }
  const stm = buf[o + 27] ? 'b' : 'w';
  return { fen: `${ranks.join('/')} ${stm} - - 0 1`, board, count: n };
}

let total = 0, bad = 0;
const results = [0, 0, 0];
for (const file of files) {
  const buf = fs.readFileSync(file);
  if (buf.length % 32) { console.log(`${file}: size ${buf.length} is not a multiple of 32`); bad++; continue; }
  for (let o = 0; o < buf.length; o += 32, total++) {
    const { fen, board, count } = toFen(buf, o);
    const score = buf.readInt16LE(o + 24), result = buf[o + 26];
    if (total < show) console.log(`${fen}  score ${score}  result ${['0-1', '1/2', '1-0'][result]}`);
    results[result]++;
    const kings = board.filter(p => p === 'K').length, bkings = board.filter(p => p === 'k').length;
    const backPawn = [0, 7].some(r => board.slice(r * 8, r * 8 + 8).some(p => p === 'P' || p === 'p'));
    let problem = kings !== 1 || bkings !== 1 ? 'king count' : backPawn ? 'pawn on back rank' : count > 32 ? 'too many pieces' : result > 2 ? 'bad result' : null;
    if (!problem) {
      const pos = new Position(fen);
      const them = pos.side === WHITE ? BLACK : WHITE;
      if (pos.isAttacked(pos.kings[them === WHITE ? 0 : 1], pos.side)) problem = 'side not to move is in check';
    }
    if (problem) { bad++; if (bad <= 5) console.log(`BAD (${problem}): ${fen}`); }
  }
}
console.log(`${total} positions in ${files.length} file(s), ${bad} bad; results 1-0 ${results[2]}  1/2 ${results[1]}  0-1 ${results[0]}`);
process.exit(bad ? 1 : 0);
