"""Phase 3: trains the NNUE evaluation (768 -> N x 2 -> 1) on self-play data, on the CPU.

Start:   python train/train.py --data data/gen1 --out nets/run1 [--epochs 20] [--hidden 256]
Resume:  python train/train.py --out nets/run1          (settings are remembered in checkpoint.pt)
Check:   python train/train.py --data data/gen1 --check-data   (decodes data with numpy only)

Stopping is safe at any time. A checkpoint is saved every --save-minutes (default 10), at the end of
each epoch and on Ctrl+C, so turning the laptop off loses at most a few minutes of training; the run
continues from the exact batch where it stopped. Each finished epoch also exports a quantized net,
nets/run1/epoch-N.nnue, and appends a line to nets/run1/train-log.csv.

Data: 32-byte records from cpp/src/datagen.h, read in groups of about 2M positions that are
shuffled in memory, so RAM use stays around 1-2 GB however large the data set is.

Network: each side's 768 inputs (2 colors x 6 piece types x 64 squares, from that side's point of
view: board flipped vertically for Black) feed one shared layer of N neurons; the side to move's
half comes first. Clipped ReLU [0, 1] then one output. Target: lambda * sigmoid(score / 400) +
(1 - lambda) * game result, both from the side to move's point of view.

.nnue export (little-endian):
  char[4] "SNNU", int32 version 1, int32 N
  int16 ft_weights[768][N]   (x 255)       int16 ft_bias[N]   (x 255)
  int16 out_weights[2N]      (x 64)        int32 out_bias     (x 255 * 64)
  eval cp = (out_bias + sum clamp(acc, 0, 255) * out_w) * 400 / (255 * 64)
"""
import argparse
import csv
import glob
import os
import signal
import struct
import sys
import time

import numpy as np

RECORD = 32
PAD = 768  # feature index used for "no piece" padding
QA, QB, SCALE = 255, 64, 400


def list_files(dirs):
    files = []
    for d in dirs:
        files += sorted(glob.glob(os.path.join(d, "chunk-*.bin")))
    return files


def decode(raw):
    """raw: (B, 32) uint8 records -> (us, them) int64 feature indices (B, 32), score_stm, result_stm."""
    b = raw.shape[0]
    occ_bits = np.unpackbits(raw[:, :8], axis=1, bitorder="little").astype(bool)  # (B, 64), bit i = square i
    count = occ_bits.sum(axis=1)
    # Squares of the occupied bits in ascending order: a stable sort puts set bits first.
    squares = np.argsort(~occ_bits, axis=1, kind="stable")[:, :32]
    nib = np.empty((b, 32), np.int64)
    nib[:, 0::2] = raw[:, 8:24] & 15
    nib[:, 1::2] = raw[:, 8:24] >> 4
    valid = np.arange(32)[None, :] < count[:, None]
    color, ptype = nib >> 3, nib & 7
    white_view = color * 384 + ptype * 64 + squares
    black_view = (1 - color) * 384 + ptype * 64 + (squares ^ 56)
    white_view[~valid] = PAD
    black_view[~valid] = PAD
    stm_black = raw[:, 27].astype(bool)
    us = np.where(stm_black[:, None], black_view, white_view)
    them = np.where(stm_black[:, None], white_view, black_view)
    score = raw[:, 24:26].copy().view(np.int16)[:, 0].astype(np.float32)
    result = raw[:, 26].astype(np.float32) / 2  # White's result: 0, 0.5, 1
    score = np.where(stm_black, -score, score)
    result = np.where(stm_black, 1 - result, result)
    return us, them, score, result


def groups(files, epoch, seed, group_positions):
    """Deterministic shuffled order of file groups for an epoch (so a resumed run sees the same order)."""
    rng = np.random.default_rng(seed + epoch)
    order = rng.permutation(len(files))
    out, cur, size = [], [], 0
    for i in order:
        cur.append(files[i])
        size += os.path.getsize(files[i]) // RECORD
        if size >= group_positions:
            out.append(cur)
            cur, size = [], 0
    if cur:
        out.append(cur)
    return out


def load_group(group, rng):
    raw = np.concatenate([np.fromfile(f, np.uint8).reshape(-1, RECORD) for f in group])
    return raw[rng.permutation(len(raw))]


def check_data(files):
    total = 0
    for f in files:
        raw = np.fromfile(f, np.uint8).reshape(-1, RECORD)
        us, them, score, result = decode(raw)
        n_us, n_them = (us != PAD).sum(1), (them != PAD).sum(1)
        assert (n_us == n_them).all(), f"{f}: feature counts differ between perspectives"
        # Exactly one own king (type 5, color 0 from the mover's view) and one enemy king per position.
        own_king = ((us >= 5 * 64) & (us < 6 * 64)).sum(1)
        enemy_king = ((us >= 384 + 5 * 64) & (us < 384 + 6 * 64)).sum(1)
        assert (own_king == 1).all() and (enemy_king == 1).all(), f"{f}: bad king features"
        assert ((result == 0) | (result == 0.5) | (result == 1)).all(), f"{f}: bad results"
        total += len(raw)
    print(f"{total} positions in {len(files)} files decode correctly")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", nargs="*", default=[])
    ap.add_argument("--out", default="nets/run1")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--batch", type=int, default=16384)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--lr-drop-epoch", type=int, default=None, help="epoch at which lr drops 10x (default: 75%% of epochs)")
    ap.add_argument("--wdl", type=float, default=0.25, help="weight of the game result in the target (1 - lambda)")
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--save-minutes", type=float, default=10)
    ap.add_argument("--group-positions", type=int, default=2_000_000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--check-data", action="store_true")
    args = ap.parse_args()

    if args.check_data:
        check_data(list_files(args.data))
        return

    import torch  # only needed for training

    os.makedirs(args.out, exist_ok=True)
    ckpt_path = os.path.join(args.out, "checkpoint.pt")
    state = torch.load(ckpt_path, weights_only=True) if os.path.exists(ckpt_path) else None
    if state:
        cfg = state["config"]
        cfg["threads"] = args.threads  # may change between sessions
        cfg["save_minutes"] = args.save_minutes
        if args.epochs != ap.get_default("epochs"):
            cfg["epochs"] = args.epochs  # allow extending a run
        print(f"resuming {args.out} at epoch {state['epoch']} group {state['group']} batch {state['batch']}")
    else:
        if not args.data:
            sys.exit("--data is required for a new run")
        cfg = {k.replace("-", "_"): v for k, v in vars(args).items() if k != "check_data"}
        cfg["lr_drop_epoch"] = args.lr_drop_epoch or max(1, int(args.epochs * 0.75))

    files = list_files(cfg["data"])
    if not files:
        sys.exit("no chunk files found in " + " ".join(cfg["data"]))
    # Hold out ~1% of files (every 100th) for validation.
    val_files = files[::100] if len(files) >= 100 else files[:1]
    val_set = set(val_files)
    train_files = [f for f in files if f not in val_set] or files
    torch.set_num_threads(cfg["threads"])
    torch.manual_seed(cfg["seed"])

    hidden = cfg["hidden"]

    class Net(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.ft = torch.nn.EmbeddingBag(769, hidden, mode="sum", padding_idx=PAD)
            self.ft_bias = torch.nn.Parameter(torch.zeros(hidden))
            self.out = torch.nn.Linear(2 * hidden, 1)
            with torch.no_grad():
                self.ft.weight.normal_(0, 0.05)
                self.ft.weight[PAD].zero_()

        def forward(self, us, them):
            a = torch.clamp(self.ft(us) + self.ft_bias, 0, 1)
            b = torch.clamp(self.ft(them) + self.ft_bias, 0, 1)
            return self.out(torch.cat([a, b], 1)).squeeze(1)  # in units of SCALE centipawns

    net = Net()
    opt = torch.optim.Adam(net.parameters(), lr=cfg["lr"])
    epoch, group_i, batch_i, step = 0, 0, 0, 0
    if state:
        net.load_state_dict(state["model"])
        opt.load_state_dict(state["optimizer"])
        epoch, group_i, batch_i, step = state["epoch"], state["group"], state["batch"], state["step"]

    def set_lr():
        lr = cfg["lr"] * (0.1 if epoch >= cfg["lr_drop_epoch"] else 1)
        for g in opt.param_groups:
            g["lr"] = lr
        return lr

    def save():
        tmp = ckpt_path + ".tmp"
        torch.save({"config": cfg, "model": net.state_dict(), "optimizer": opt.state_dict(),
                    "epoch": epoch, "group": group_i, "batch": batch_i, "step": step}, tmp)
        os.replace(tmp, ckpt_path)  # atomic: a crash mid-save keeps the previous checkpoint

    def export(path):
        with torch.no_grad():
            ftw = torch.round(net.ft.weight[:768] * QA).to(torch.int16).numpy()
            ftb = torch.round(net.ft_bias * QA).to(torch.int16).numpy()
            ow = torch.round(net.out.weight[0] * QB).to(torch.int16).numpy()
            ob = int(round(float(net.out.bias[0]) * QA * QB))
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(b"SNNU" + struct.pack("<ii", 1, hidden))
            f.write(ftw.astype("<i2").tobytes() + ftb.astype("<i2").tobytes() + ow.astype("<i2").tobytes())
            f.write(struct.pack("<i", ob))
        os.replace(tmp, path)

    def batches(raw):
        for i in range(0, len(raw) - cfg["batch"] + 1, cfg["batch"]):
            us, them, score, result = decode(raw[i:i + cfg["batch"]])
            target = (1 - cfg["wdl"]) * torch.sigmoid(torch.from_numpy(score) / SCALE) + cfg["wdl"] * torch.from_numpy(result)
            yield torch.from_numpy(us), torch.from_numpy(them), target

    def validate():
        net.eval()
        total, n = 0.0, 0
        # One array for all validation files: a single chunk can be smaller than a batch.
        raw = np.concatenate([np.fromfile(f, np.uint8).reshape(-1, RECORD) for f in val_files])
        with torch.no_grad():
            for us, them, target in batches(raw):
                total += torch.mean((torch.sigmoid(net(us, them)) - target) ** 2).item() * len(target)
                n += len(target)
        net.train()
        return total / max(n, 1)

    stop = {"now": False}

    def on_signal(*_):
        stop["now"] = True
        print("\nstopping after this batch; saving checkpoint...")

    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, on_signal)

    log_path = os.path.join(args.out, "train-log.csv")
    last_save = time.time()
    print(f"{len(train_files)} training files, {len(val_files)} validation files, hidden {hidden}, {cfg['threads']} threads")
    while epoch < cfg["epochs"]:
        lr = set_lr()
        epoch_groups = groups(train_files, epoch, cfg["seed"], cfg["group_positions"])
        t0, seen, loss_sum = time.time(), 0, 0.0
        while group_i < len(epoch_groups):
            raw = load_group(epoch_groups[group_i], np.random.default_rng((cfg["seed"], epoch, group_i)))
            for k, (us, them, target) in enumerate(batches(raw)):
                if k < batch_i:
                    continue  # already trained on before the last stop
                loss = torch.mean((torch.sigmoid(net(us, them)) - target) ** 2)
                opt.zero_grad(set_to_none=True)
                loss.backward()
                opt.step()
                with torch.no_grad():
                    net.out.weight.clamp_(-127 / QB, 127 / QB)  # keeps quantized output weights small
                    net.ft.weight[PAD].zero_()
                batch_i, step = k + 1, step + 1
                seen += len(target)
                loss_sum += loss.item() * len(target)
                if step % 100 == 0:
                    rate = seen / max(time.time() - t0, 1e-9)
                    print(f"epoch {epoch + 1}/{cfg['epochs']}  group {group_i + 1}/{len(epoch_groups)}  "
                          f"loss {loss_sum / max(seen, 1):.5f}  {rate / 1000:.0f}k pos/s", end="\r")
                if stop["now"] or time.time() - last_save > cfg["save_minutes"] * 60:
                    save()
                    last_save = time.time()
                    if stop["now"]:
                        print(f"\ncheckpoint saved at epoch {epoch + 1}, group {group_i + 1}, batch {batch_i}; rerun to resume")
                        return
            group_i, batch_i = group_i + 1, 0
        val = validate()
        epoch, group_i, batch_i = epoch + 1, 0, 0
        export(os.path.join(args.out, f"epoch-{epoch}.nnue"))
        save()
        last_save = time.time()
        new_log = not os.path.exists(log_path)
        with open(log_path, "a", newline="") as f:
            w = csv.writer(f)
            if new_log:
                w.writerow(["time", "epoch", "lr", "train_loss", "val_loss", "positions"])
            w.writerow([time.strftime("%Y-%m-%d %H:%M:%S"), epoch, lr, f"{loss_sum / max(seen, 1):.6f}", f"{val:.6f}", seen])
        print(f"\nepoch {epoch} done: train {loss_sum / max(seen, 1):.5f}  val {val:.5f}  -> epoch-{epoch}.nnue")
    print("training complete")


if __name__ == "__main__":
    main()
