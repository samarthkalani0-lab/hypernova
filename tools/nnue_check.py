"""Reference NNUE evaluation in numpy, to check the engine's integer inference.

python tools/nnue_check.py <net.nnue> <chunk.bin> [count]
Prints "<fen>\t<eval>" per position (eval in centipawns, side to move's view), computed exactly like
cpp/src/nnue.cpp. Compare with the engine's "eval" command on the same FENs.
"""
import struct
import sys

import numpy as np


def load_net(path):
    data = open(path, "rb").read()
    assert data[:4] == b"SNNU"
    version, n = struct.unpack_from("<ii", data, 4)
    off = 12
    ftw = np.frombuffer(data, "<i2", 768 * n, off).reshape(768, n).astype(np.int64); off += 768 * n * 2
    ftb = np.frombuffer(data, "<i2", n, off).astype(np.int64); off += n * 2
    ow = np.frombuffer(data, "<i2", 2 * n, off).astype(np.int64); off += 4 * n
    ob = struct.unpack_from("<i", data, off)[0]
    return ftw, ftb, ow, ob


def positions(path, count):
    raw = np.fromfile(path, np.uint8).reshape(-1, 32)[:count]
    for rec in raw:
        occ = int.from_bytes(rec[:8].tobytes(), "little")
        squares = [s for s in range(64) if occ >> s & 1]
        nibs = [(rec[8 + i // 2] >> (4 * (i & 1))) & 15 for i in range(len(squares))]
        yield {sq: (int(nb) >> 3, int(nb) & 7) for sq, nb in zip(squares, nibs)}, int(rec[27])


def fen(pieces, stm_black):
    rows = []
    for r in range(7, -1, -1):
        row, empty = "", 0
        for f in range(8):
            p = pieces.get(r * 8 + f)
            if p is None:
                empty += 1
                continue
            if empty:
                row += str(empty)
                empty = 0
            ch = "pnbrqk"[p[1]]
            row += ch if p[0] else ch.upper()
        rows.append(row + (str(empty) if empty else ""))
    return "/".join(rows) + (" b" if stm_black else " w") + " - - 0 1"


def evaluate(net, pieces, stm_black):
    ftw, ftb, ow, ob = net
    n = len(ftb)
    acc = []
    for persp in (0, 1):
        a = ftb.copy()
        for sq, (color, ptype) in pieces.items():
            own = 0 if color == persp else 1
            a += ftw[own * 384 + ptype * 64 + (sq ^ 56 if persp else sq)]
        acc.append(a)
    us, them = (acc[1], acc[0]) if stm_black else (acc[0], acc[1])
    total = int(np.clip(us, 0, 255) @ ow[:n] + np.clip(them, 0, 255) @ ow[n:]) + ob
    q = abs(total) * 400 // (255 * 64)  # C++ integer division truncates toward zero
    return q if total >= 0 else -q


def main():
    net = load_net(sys.argv[1])
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    for pieces, stm in positions(sys.argv[2], count):
        print(f"{fen(pieces, stm)}\t{max(-20000, min(20000, evaluate(net, pieces, stm)))}")


if __name__ == "__main__":
    main()
