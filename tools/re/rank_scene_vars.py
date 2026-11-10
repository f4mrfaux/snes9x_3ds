#!/usr/bin/env python3
"""Rank work-RAM addresses by how much they behave like a game's scene variable.

Reads the output of wram_host. A scene variable changes when the scene does (from
ppu.csv if recorded, otherwise picture cuts and fades), holds still in between, and goes back to the same values when
the game returns to a scene. Timers, positions and animation counters fail at
least one of those, so they drop down the list.

This only ranks candidates. A person still has to confirm the top ones against
what is on screen.

    rank_scene_vars.py <outdir> [--top N] [--window FRAMES] [--check ADDR]
                       [--events auto|ppu|picture]
"""
import argparse
import csv
import struct
from pathlib import Path

MIN_CHANGES = 2         # one change can't show a pattern
MAX_VALUES = 64         # a scene id has few values; a counter walks through many
BLACK_LUMA = 8.0
CUT_CHANGED = 0.6
HELD_FRAMES = 60        # about a second: longer than any fade passes through a value
FULL_HELD_VALUES = 3    # a scene variable rests at several values, a fade at one


def read_delta(path):
    """Returns (initial RAM, {address: [(frame, value), ...]}, frames recorded)."""
    with open(path, "rb") as f:
        size = struct.unpack("<I", f.read(4))[0]
        initial = f.read(size)
        if len(initial) != size:
            raise ValueError("wram.delta is truncated in its first snapshot")
        history = {}
        frames = 0
        while True:
            head = f.read(8)
            if not head:
                break
            if len(head) != 8:
                raise ValueError("wram.delta is truncated in a frame header")
            frame, count = struct.unpack("<II", head)
            body = f.read(count * 4)
            if len(body) != count * 4:
                raise ValueError(f"wram.delta is truncated at frame {frame}")
            for change in struct.iter_unpack("<I", body):
                address = change[0] & 0xFFFFFF
                history.setdefault(address, []).append((frame, change[0] >> 24))
            frames = frame + 1
    return initial, history, frames


def read_frames(path):
    with open(path, newline="") as f:
        return [(int(r["frame"]), float(r["luma"]), float(r["changed"])) for r in csv.DictReader(f)]


def find_scene_events(rows, gap=30):
    """Frames where the picture cuts or goes to or from black, merged if close."""
    events = []
    was_black = None
    for frame, luma, changed in rows:
        black = luma < BLACK_LUMA
        if (was_black is not None and black != was_black) or changed >= CUT_CHANGED:
            if not events or frame - events[-1] >= gap:
                events.append(frame)
        was_black = black
    return events


def long_held_values(changes, frames, held_frames=HELD_FRAMES):
    """How many distinct values the address stayed at for at least held_frames."""
    held = set()
    for i, (frame, value) in enumerate(changes):
        until = changes[i + 1][0] if i + 1 < len(changes) else frames
        if until - frame >= held_frames:
            held.add(value)
    return len(held)


# What a scene is set up from. Brightness, blanking and sprite counts are left out
# because they move during a scene and during fades.
PPU_SIGNATURE = ("mode", "bg3prio", "tm", "ts", "cgwsel", "cgadsub", "sc0", "name0", "size0",
                 "sc1", "name1", "size1", "sc2", "name2", "size2", "sc3", "name3", "size3",
                 "objname", "objsize")
STABLE_FRAMES = 30      # a signature must last this long to count as a scene


def read_ppu(path):
    with open(path, newline="") as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def find_ppu_events(rows, stable=STABLE_FRAMES):
    """Frames where the PPU settles into a different setup.

    Blanked and fully dark frames say nothing, so they are skipped. A signature
    only counts once it has lasted, so a game that changes registers partway
    through a transition produces one event, dated when the new setup began.
    """
    events = []
    current = None
    candidate = None
    since = 0
    run = 0
    for row in rows:
        if row["blank"] or row["bright"] == 0:
            continue
        signature = tuple(row[k] for k in PPU_SIGNATURE)
        if signature != candidate:
            candidate, since, run = signature, row["frame"], 0
        run += 1
        if run >= stable and candidate != current:
            if current is not None:
                events.append(since)
            current = candidate
    return events


def score_address(changes, events, window, frames):
    """Returns the score and its parts. Near an event means within window frames."""
    near = sum(1 for frame, _ in changes if any(abs(frame - e) <= window for e in events))
    precision = near / len(changes)
    covered = sum(1 for e in events if any(abs(frame - e) <= window for frame, _ in changes))
    recall = covered / len(events) if events else 0.0
    values = {value for _, value in changes}
    revisits = len(changes) - len(values)
    timing = 0.0 if precision + recall == 0 else 2 * precision * recall / (precision + recall)
    held = long_held_values(changes, frames)
    score = timing * min(held, FULL_HELD_VALUES) / FULL_HELD_VALUES
    return {
        "score": score,
        "precision": precision,
        "recall": recall,
        "changes": len(changes),
        "values": len(values),
        "revisits": revisits,
        "held": held,
    }


def rank(history, events, window, frames=None):
    if frames is None:
        frames = max(f for changes in history.values() for f, _ in changes) + 1
    ranked = []
    for address, changes in history.items():
        if len(changes) < MIN_CHANGES or len({v for _, v in changes}) > MAX_VALUES:
            continue
        ranked.append((address, score_address(changes, events, window, frames)))
    ranked.sort(key=lambda item: (-item[1]["score"], -item[1]["revisits"], item[0]))
    return ranked


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("outdir", type=Path)
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--window", type=int, default=45, help="frames around an event that count as near it")
    parser.add_argument("--check", type=lambda s: int(s, 0), action="append", default=[],
                        help="also report this address's rank and value history (WRAM offset)")
    parser.add_argument("--events", choices=("auto", "ppu", "picture"), default="auto",
                        help="where scene changes come from; auto uses ppu.csv if present")
    args = parser.parse_args()

    _, history, frames = read_delta(args.outdir / "wram.delta")
    ppu_path = args.outdir / "ppu.csv"
    source = args.events if args.events != "auto" else ("ppu" if ppu_path.exists() else "picture")
    if source == "ppu":
        events = find_ppu_events(read_ppu(ppu_path))
    else:
        events = find_scene_events(read_frames(args.outdir / "frames.csv"))
    ranked = rank(history, events, args.window, frames)

    print(f"event source: {source}")
    print(f"frames={frames} addresses_changed={len(history)} candidates={len(ranked)} events={len(events)}")
    print("events at frames:", " ".join(map(str, events)))
    print(f"{'rank':>4} {'addr':>8} {'score':>6} {'prec':>5} {'recall':>6} {'chg':>5} {'vals':>4} {'held':>4}")
    for i, (address, s) in enumerate(ranked[:args.top], 1):
        print(f"{i:>4} {0x7E0000 + address:>8X} {s['score']:>6.3f} {s['precision']:>5.2f} {s['recall']:>6.2f} "
              f"{s['changes']:>5} {s['values']:>4} {s['held']:>4}")

    positions = {address: i for i, (address, _) in enumerate(ranked, 1)}
    for address in args.check:
        changes = history.get(address, [])
        place = positions.get(address, "not a candidate")
        shown = " ".join(f"{frame}:{value:02X}" for frame, value in changes[:40])
        print(f"check {0x7E0000 + address:X}: rank {place} of {len(ranked)}, {len(changes)} changes: {shown}")


if __name__ == "__main__":
    main()
