#!/usr/bin/env node
// Phase 2: generates self-play training data in small chunks, so it can stop and resume at any time.
// Start:  node datagen.js --out data/gen1 [--engine bin/supernova-datagen.exe] [--positions 100000000]
//                         [--nodes 5000] [--games-per-chunk 100] [--threads 8] [--seed 1]
// Resume: node datagen.js --out data/gen1        (settings are remembered in data/gen1/config.json)
// Status: node datagen.js --out data/gen1 --status
// Change: node datagen.js --out data/gen1 --threads 6   (threads and the target can change on resume)
//
// Each chunk is one engine process writing chunk-NNNNNN.bin.tmp, renamed to .bin when complete.
// Turning the laptop off, sleeping or Ctrl+C loses only the chunks in progress (about a minute of
// work each); they are deleted and redone on the next start. The engine is copied into the output
// folder on the first run so that rebuilding the engine later cannot mix two versions in one data set.
// Record format: cpp/src/datagen.h. Check data with: node tools/dumpdata.js data/gen1
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const flag = name => { const i = args.indexOf(name); return i === -1 ? undefined : args[i + 1]; };
const outDir = flag('--out');
if (!outDir) { console.error('usage: node datagen.js --out <dir> [--positions N] [--nodes N] [--threads N] [--status]'); process.exit(1); }
fs.mkdirSync(outDir, { recursive: true });

// Settings: saved on the first run; threads and the position target may be changed on resume.
const configFile = path.join(outDir, 'config.json');
const saved = fs.existsSync(configFile) ? JSON.parse(fs.readFileSync(configFile, 'utf8')) : null;
const config = saved || {
  positions: +(flag('--positions') || 100e6),
  nodes: +(flag('--nodes') || 5000),
  gamesPerChunk: +(flag('--games-per-chunk') || 100),
  seed: +(flag('--seed') || 1),
  createdAt: new Date().toISOString(),
};
if (flag('--positions')) config.positions = +flag('--positions');
const threads = +(flag('--threads') || 8);
const engine = path.join(outDir, 'engine.exe');
if (!saved) {
  fs.copyFileSync(path.resolve(flag('--engine') || 'bin/supernova-datagen.exe'), engine);
  console.log(`new data set in ${outDir}: ${JSON.stringify(config)}`);
}
fs.writeFileSync(configFile, JSON.stringify(config, null, 2));

// Discard unfinished chunks from an interrupted run, then count what is already done.
const done = new Set();
let positions = 0;
for (const f of fs.readdirSync(outDir)) {
  if (f.endsWith('.tmp')) { try { fs.unlinkSync(path.join(outDir, f)); } catch {} continue; } // may still be locked by an orphaned engine
  const m = f.match(/^chunk-(\d+)\.bin$/);
  if (m) { done.add(+m[1]); positions += fs.statSync(path.join(outDir, f)).size / 32; }
}
const fmt = n => n >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(1) + 'k' : String(n);
console.log(`${fmt(positions)} / ${fmt(config.positions)} positions in ${done.size} chunks (${(positions * 32 / 2 ** 30).toFixed(2)} GB)`);
if (args.includes('--status')) process.exit(0);
if (positions >= config.positions) { console.log('target reached'); process.exit(0); }

const chunkFile = i => path.join(outDir, `chunk-${String(i).padStart(6, '0')}.bin`);
let nextIndex = 0, running = 0, stopping = false;
const children = new Set();
const start = Date.now(), startPositions = positions;
const log = line => fs.appendFileSync(path.join(outDir, 'progress.log'), `${new Date().toISOString()} ${line}\n`);
log(`start: ${fmt(positions)} positions, ${threads} threads`);

// Positions per chunk so far, to avoid launching more chunks than the target needs.
const perChunk = () => (done.size ? positions / done.size : config.gamesPerChunk * 80);

function launch() {
  while (!stopping && running < threads && positions + running * perChunk() < config.positions) {
    while (done.has(nextIndex)) nextIndex++;
    const index = nextIndex++;
    const tmp = chunkFile(index) + '.tmp';
    const seed = config.seed * 1000003 + index;
    const child = spawn(engine, [`datagen ${tmp} ${config.gamesPerChunk} ${config.nodes} ${seed}`, 'quit'], { stdio: 'ignore' });
    // Lower priority keeps the laptop usable while generation runs.
    try { os.setPriority(child.pid, os.constants.priority.PRIORITY_BELOW_NORMAL); } catch {}
    children.add(child);
    running++;
    child.on('exit', code => {
      children.delete(child);
      running--;
      if (stopping) return;
      if (code === 0 && fs.existsSync(tmp)) {
        fs.renameSync(tmp, chunkFile(index));
        done.add(index);
        positions += fs.statSync(chunkFile(index)).size / 32;
        const hours = (Date.now() - start) / 3.6e6, rate = (positions - startPositions) / hours;
        const eta = rate > 0 ? (config.positions - positions) / rate : Infinity;
        const line = `${fmt(positions)} / ${fmt(config.positions)} positions  ${fmt(Math.round(rate))}/hour  ETA ${eta.toFixed(1)} h`;
        console.log(line);
        if (done.size % 20 === 0) log(line);
      } else {
        console.log(`chunk ${index} failed (exit ${code}); it will be retried`);
        try { fs.unlinkSync(tmp); } catch {}
        nextIndex = Math.min(nextIndex, index);
      }
      if (positions >= config.positions && running === 0) { log(`target reached: ${fmt(positions)}`); console.log('target reached'); process.exit(0); }
      launch();
    });
  }
}

const shutdown = () => {
  if (stopping) return;
  stopping = true;
  console.log('\nstopping; unfinished chunks will be redone on the next start');
  log(`stopped at ${fmt(positions)} positions`);
  for (const c of children) { try { c.kill(); } catch {} }
  setTimeout(() => {
    for (const f of fs.readdirSync(outDir)) if (f.endsWith('.tmp')) { try { fs.unlinkSync(path.join(outDir, f)); } catch {} }
    process.exit(0);
  }, 1000);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
launch();
