#!/usr/bin/env node
// Converts a PGN opening book into one line of UCI moves per opening, for match.js --book.
// Usage: node tools/pgn2uci.js books/8moves_v3.pgn books/8moves_v3.txt
const fs = require('fs');
const { Position, moveToUci } = require('../engine.js');

const [input, output] = process.argv.slice(2);
if (!input || !output) { console.error('usage: node tools/pgn2uci.js <in.pgn> <out.txt>'); process.exit(1); }

const strip = s => s.replace(/[+#!?]/g, '');
const games = fs.readFileSync(input, 'utf8').split(/\r?\n\r?\n(?=\[)/);
const lines = [];
let bad = 0;
for (const game of games) {
  const movetext = game.split(/\r?\n/).filter(l => !l.startsWith('[')).join(' ');
  const sans = movetext.replace(/\{[^}]*\}/g, ' ').split(/\s+/)
    .filter(t => t && !/^\d+\.+$/.test(t) && !/^(1-0|0-1|1\/2-1\/2|\*)$/.test(t))
    .map(t => t.replace(/^\d+\.+/, ''));
  if (!sans.length) continue;
  const pos = new Position();
  const ucis = [];
  for (const san of sans) {
    const m = pos.legalMoves().find(c => strip(pos.san(c)) === strip(san));
    if (!m) { ucis.length = 0; break; }
    ucis.push(moveToUci(m));
    pos.makeMove(m);
  }
  if (ucis.length) lines.push(ucis.join(' ')); else bad++;
}
fs.writeFileSync(output, lines.join('\n') + '\n');
console.log(`${lines.length} openings written to ${output}, ${bad} could not be parsed`);
