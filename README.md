# Hypernova

Copyright (C) 2026 Samarth Kalani. Hypernova is free software under the GNU General Public License v3.0
(see `LICENSE`); the source code is in `Hypernova-<version>-source.zip` next to this release.

Hypernova is a UCI chess engine with an NNUE evaluation trained only on its own self-play games.
It needs a UCI chess GUI (Arena, Cute Chess, Banksia, ChessBase, Fritz, ...); it has no board of its own.

## Which executable

| File | CPU |
| --- | --- |
| `Hypernova-<version>-avx2.exe` | AVX2 (Intel Haswell 2013+, AMD Zen / Excavator). Use this one if it starts. |
| `Hypernova-<version>-popcnt.exe` | Older 64-bit CPUs (2009+), slower. |

The network is built into both executables; the `.nnue` file is included only for reference.

## UCI options

| Option | Default | Notes |
| --- | --- | --- |
| Hash | 64 | Transposition table size in MB (1-4096). |
| Threads | 1 | Search threads (Lazy SMP), 1-256. |
| EvalFile | `<empty>` | Path to a different `.nnue` network; leave empty to use the built-in one. |

Hypernova does not ponder and has no opening book or endgame tablebase support; use the GUI's book.

## Technical summary

- Search: principal variation search with aspiration windows, transposition table, null-move pruning,
  reverse futility pruning, late move pruning and reductions (history-adjusted), futility pruning,
  SEE pruning, singular extensions, killer moves, history and continuation history.
- Board: magic bitboards for move generation and attacks.
- Evaluation: NNUE, 768 inputs -> 256x2 -> 1, clipped ReLU, int16 quantized, trained with PyTorch on
  about 90 million positions from Hypernova's own self-play games (no other engine's data or evaluation).
- Time management: soft and hard limits, scaled by best-move stability and score drops.

## Commands outside UCI

`bench [depth]` runs a fixed-depth search on a few positions (node count and speed),
`perft <depth>` counts moves from the current position, `eval` prints the static evaluation,
and `d` prints the board.

## Building from source

Needs CMake and a C++17 compiler (GCC/MinGW or Clang). From the source folder:

    cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release -DSN_NET=path/to/hypernova.nnue
    cmake --build build

`-DSN_ARCH=x86-64-v2` builds for CPUs without AVX2. Training code is in `train/` (PyTorch, CPU).
