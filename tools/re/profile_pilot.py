"""Picks a starting depth preset per game by playing it headless and looking for exposed junk.

Pixel comparison decides. For each sampled frame it compares each eye against the flat picture and
looks for tall thin strips in colours that the flat picture has nowhere near those rows, where a
sprite was covering them. That's the shape of SMW's green lines and Stunt Race FX's noise. A game
gets the deepest preset with no such strip, and a game flagged at every preset goes to review rather
than being guessed at. Colour math (Super Metroid's rain) blends layers into new colours wherever their
depths differ, which is correct 3D but looks foreign here, so expect those games on the review list.
The local vision model only gives a second opinion on zoomed crops of what was flagged.

  profile_pilot.py control            known failures and known clean cases; must pass first
  profile_pilot.py run [games...]     play games and write report.md under $S3RE_ROOT/pilot
  profile_pilot.py resume all         same, keeping games already in results.json
  profile_pilot.py zero               replay flagged games at zero depth; any flag there isn't parallax
  profile_pilot.py review             ask the local model about each flagged crop
  profile_pilot.py summarize          have the local text model write summary.md

A pass only means nothing was found in the scenes the input reached, on the libretro core.
Output holds game pictures, so it stays outside the repository: $S3RE_ROOT/pilot, or ~/s3re/pilot
when S3RE_ROOT is unset. Set SNES_ROM_DIR to the folder that holds the SNES ROMs.
"""
import concurrent.futures
import base64
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import zlib

ROOT = (os.environ.get("S3RE_ROOT") or os.path.join(os.path.expanduser("~"), "s3re")).replace("\\", "/").rstrip("/") + "/pilot"
BIN = ROOT + "/bin"
ROMDIR = (os.environ.get("SNES_ROM_DIR") or "").replace("\\", "/").rstrip("/") + "/"
MODEL = "qwen2.5vl:7b"

FRAMES, STEP = 9000, 150
WORKERS = 8
MIN_STRIP_ROWS = 8      # a strip this tall stops looking like an ordinary sprite edge
FLAG_PIXELS = 16        # strip pixels in one eye of one frame before a frame is flagged
SPRITE_Z = {36, 40, 44, 48}     # the probe core's per-pixel layer values for sprite priorities 0-3

#            BG1    BG2    BG3    BG4    OBJ 0-3    BG3 front
PRESETS = [
    ("default", "0,0,2,2,4,0,5,4,2,0,0,0,0", "3,3,5,5,0,0,0,0,4,1,1,1,0"),
    ("tied", "0,0,2,0,4,0,5,4,2,0,0,0,0", "3,5,5,5,0,0,0,0,4,1,1,5,0"),
    ("shallow", "0,0,1,0,2,0,2,2,1,0,0,0,0", "1,2,2,2,0,0,0,0,2,0,0,2,0"),
]
FALLBACK = "review"

GAMES = {
    "smw": "Super Mario World (USA).sfc",
    "stunt": "Stunt Race FX (USA) (Rev 1).sfc",
    "metroid": "Super Metroid (Japan, USA) (En,Ja).sfc",
    "fzero": "F-Zero (USA).sfc",
    "zelda": "Legend of Zelda, The - A Link to the Past (USA).sfc",
    "mmx": "Mega Man X (USA).sfc",
    "ct": "Chrono Trigger (USA).sfc",
    "smk": "Super Mario Kart (USA).sfc",
    "dkc": "Donkey Kong Country (USA) (Rev 2).sfc",
    "yi": "Super Mario World 2 - Yoshi's Island (USA).sfc",
    "starfox": "Star Fox (USA).sfc",
    "smrpg": "Super Mario RPG - Legend of the Seven Stars (USA).sfc",
    "kss": "Kirby Super Star (USA).sfc",
    "pilot": "Pilotwings (USA).sfc",
    "ff3": "Final Fantasy III (USA).sfc",
    "contra": "Contra III - The Alien Wars (USA).sfc",
    "scv4": "Super Castlevania IV (USA).sfc",
    "earthbound": "EarthBound (USA).sfc",
    "actraiser": "ActRaiser (USA).sfc",
    "demonscrest": "Demon's Crest (USA).sfc",
}


def holds(kind, frames):
    """Button holds as hold=frame:button arguments. idle leaves attract demos running."""
    if kind == "idle":
        return []
    h = [(x, 3) for x in range(300, frames, 300)] + [(x, 8) for x in range(450, frames, 300)]
    if kind == "walk":
        h += [(x, 7) for x in range(300, frames, 4)]
    return ["hold=%d:%d" % p for p in h]


def play(rom, out, frames, dumps, kind, slots=None):
    """Runs the probe core with a clean output folder. Returns the host's exit code."""
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    text = ",".join(map(str, dumps))
    env = dict(os.environ, S9X_PROBE_DIR=out, S9X_PROBE_FRAMES=text)
    if slots:
        env["S9X_PROBE_SLOTS"] = slots
    args = [BIN + "/stereo_host.exe", BIN + "/snes9x_libretro.dll", ROMDIR + rom, out, str(frames),
            "1" if slots else "0", "4", text] + holds(kind, frames)
    flags = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
    try:
        return subprocess.run(args, capture_output=True, env=env, cwd=out, timeout=1500, creationflags=flags).returncode
    except subprocess.TimeoutExpired:
        return -1


def read_ppm(path):
    d = open(path, "rb").read()
    q = d.split(None, 4)
    w, h = int(q[1]), int(q[2])
    return w, h, d[len(d) - w * h * 3:]


def to_rows(w, h, rgb):
    rows = []
    for y in range(h):
        r = rgb[y * w * 3:(y + 1) * w * 3]
        rows.append([r[i] << 16 | r[i + 1] << 8 | r[i + 2] for i in range(0, len(r), 3)])
    return rows


def foreign_pixels(flat, eye, reach):
    """Marks eye pixels no shift of the flat row explains, in colours the flat picture has nowhere near that row.

    A revealed background usually continues in colours already on its row, which is ordinary
    parallax. What's left is content from somewhere the row never shows.
    """
    fw, fh = len(flat[0]), len(flat)
    ew = len(eye[0])
    crop = (fw - ew) // 2
    colours = [set(row) for row in flat]
    marks = []
    for y in range(min(fh, len(eye))):
        same = flat[y]
        near = colours[y] | colours[max(y - 1, 0)] | colours[min(y + 1, fh - 1)]
        row = eye[y]
        for x in range(ew):
            c = row[x]
            if c in near:
                continue
            sx = x + crop
            if c in same[max(sx - reach, 0):sx + reach + 1]:
                continue
            marks.append((x, y))
    return marks


def find_strips(marks, min_rows=MIN_STRIP_ROWS):
    """Tall runs of marked pixels per column, tolerating one-row gaps. Returns (x, y0, y1, count)."""
    columns = {}
    for x, y in marks:
        columns.setdefault(x, []).append(y)
    strips = []
    for x, ys in columns.items():
        ys.sort()
        start = prev = ys[0]
        count = 1
        for y in ys[1:]:
            if y - prev <= 2:
                count += 1
            else:
                if prev - start + 1 >= min_rows:
                    strips.append((x, start, prev, count))
                start = y
                count = 1
            prev = y
        if prev - start + 1 >= min_rows:
            strips.append((x, start, prev, count))
    return strips


def score_eye(flat, eye, reach, flat_z=None):
    """Returns (pixels, strips) for strips a sprite was covering, plus the pixels of any other strips.

    Every junk exposure confirmed so far sat where the flat picture has a sprite. A strip between two
    backgrounds is usually a background legitimately continuing, so it's counted but doesn't flag.
    """
    strips = find_strips(foreign_pixels(flat, eye, reach))
    if flat_z is None:
        return sum(s[3] for s in strips), strips, 0
    crop = (len(flat[0]) - len(eye[0])) // 2
    backed, other = [], 0
    for s in strips:
        rows = range(s[1], s[2] + 1)
        hits = sum(any(flat_z[y * 256 + sx] in SPRITE_Z for sx in range(max(s[0] + crop - reach, 0), min(s[0] + crop + reach + 1, 256)))
                   for y in rows if y * 256 + 255 < len(flat_z))
        if hits * 2 >= len(rows):
            backed.append(s)
        else:
            other += s[3]
    return sum(s[3] for s in backed), backed, other


def reach_of(slots):
    return max(abs(int(v)) for v in slots.split(","))


def png_bytes(w, h, rows):
    raw = b"".join(bytes([0]) + b"".join(v.to_bytes(3, "big") for v in row) for row in rows)

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))

    head = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    return head + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")


def save_crop(path, left, right, strips, margin=16, zoom=4):
    """Both eyes around the flagged strips, zoomed, for the model and for a person."""
    x0 = max(min(s[0] for s in strips) - margin, 0)
    x1 = min(max(s[0] for s in strips) + margin, len(left[0]) - 1)
    y0 = max(min(s[1] for s in strips) - margin, 0)
    y1 = min(max(s[2] for s in strips) + margin, len(left) - 1)
    rows = []
    for y in range(y0, y1 + 1):
        line = left[y][x0:x1 + 1] + [0xFF00FF] * 2 + right[y][x0:x1 + 1]
        wide = [v for v in line for _ in range(zoom)]
        rows += [wide] * zoom
    open(path, "wb").write(png_bytes(len(rows[0]), len(rows), rows))


def score_run(name, flat_dir, eye_dir, frames, reach, crops):
    """Returns (checked, skipped, flagged frames, other strip pixels). A flagged frame is (frame, pixels, crop path)."""
    checked = skipped = other = 0
    flagged = []
    for f in frames:
        pm, pl, pr = [os.path.join(d, "f%05d_%s.ppm" % (f, e)) for d, e in ((flat_dir, "M"), (eye_dir, "L"), (eye_dir, "R"))]
        if not (os.path.exists(pm) and os.path.exists(pl) and os.path.exists(pr)):
            skipped += 1
            continue
        mw, mh, m = read_ppm(pm)
        lw, lh, l = read_ppm(pl)
        rw, rh, r = read_ppm(pr)
        if mw != 256 or lw > 256 or rw > 256:   # hires isn't scored
            skipped += 1
            continue
        flat, left, right = to_rows(mw, mh, m), to_rows(lw, lh, l), to_rows(rw, rh, r)
        checked += 1
        zpath = os.path.join(flat_dir, "z_%05d_L.bin" % f)
        flat_z = open(zpath, "rb").read() if os.path.exists(zpath) else None
        lp, ls, lo = score_eye(flat, left, reach, flat_z)
        rp, rs, ro = score_eye(flat, right, reach, flat_z)
        other += lo + ro
        if max(lp, rp) >= FLAG_PIXELS:
            crop = None
            if crops:
                os.makedirs(crops, exist_ok=True)
                crop = "%s/%s_f%05d.png" % (crops, name, f)
                save_crop(crop, left, right, ls + rs)
            flagged.append((f, max(lp, rp), crop))
    return checked, skipped, flagged, other


def distinct_scenes(flat_dir, frames):
    """A rough coverage count: frames whose coarse thumbnails differ."""
    seen = set()
    for f in frames:
        p = os.path.join(flat_dir, "f%05d_M.ppm" % f)
        if os.path.exists(p):
            w, h, m = read_ppm(p)
            seen.add(bytes(v >> 5 for v in m[::w * 3 * 16 + 3 * 16]))
    return len(seen)


def run_game(game):
    rom = GAMES[game]
    work = "%s/work/%s" % (ROOT, game)
    frames = list(range(STEP, FRAMES, STEP))
    result = {"game": game, "rom": rom, "preset": None, "tried": [], "errors": [], "scenes": {}, "checked": 0, "skipped": 0}
    for kind in ("idle", "mash", "walk"):
        rc = play(rom, "%s/flat_%s" % (work, kind), FRAMES, frames, kind)
        if rc:
            result["errors"].append("flat %s rc=%s" % (kind, rc))
        result["scenes"][kind] = distinct_scenes("%s/flat_%s" % (work, kind), frames)
    for name, fam0, fam1 in PRESETS:
        slots = fam0 + "," + fam1
        tally = {"preset": name, "checked": 0, "skipped": 0, "flagged": []}
        for kind in ("idle", "mash", "walk"):
            eye_dir = "%s/eye_%s_%s" % (work, name, kind)
            rc = play(rom, eye_dir, FRAMES, frames, kind, slots)
            if rc:
                result["errors"].append("%s %s rc=%s" % (name, kind, rc))
            c, s, fl, other = score_run("%s_%s_%s" % (game, name, kind), "%s/flat_%s" % (work, kind), eye_dir, frames,
                                        reach_of(slots), "%s/crops/%s" % (ROOT, game))
            tally["checked"] += c
            tally["skipped"] += s
            tally["other_pixels"] = tally.get("other_pixels", 0) + other
            tally["flagged"] += [(kind, *x) for x in fl]
            shutil.rmtree(eye_dir, ignore_errors=True)
        result["tried"].append(tally)
        result["checked"], result["skipped"] = tally["checked"], tally["skipped"]
        if not tally["flagged"] and tally["checked"]:
            result["preset"] = name
            break
    if not result["preset"]:
        result["preset"] = FALLBACK
    shutil.rmtree(work, ignore_errors=True)
    return result


def control():
    """Known answers. Thresholds may only be tuned against these, never against the pilot's games."""
    smw_frames = list(range(2100, 2700, 100))
    stunt_frames = [2700, 3000, 3600]
    d0, d1 = PRESETS[0][1], PRESETS[0][2]
    rigid = ",".join(["5"] * 26)
    cases = [
        ("smw default", "smw", "mash", smw_frames, d0 + "," + d1, False),
        ("smw OBJ3=1 front=0", "smw", "mash", smw_frames, "0,0,2,2,4,0,5,4,2,0,0,1,0," + d1, True),
        ("smw OBJ3=3 front=0", "smw", "mash", smw_frames, "0,0,2,2,4,0,5,4,2,0,0,3,0," + d1, True),
        ("smw OBJ3=3 front=3", "smw", "mash", smw_frames, "0,0,2,2,4,0,5,4,2,0,0,3,3," + d1, False),
        ("smw rigid +5", "smw", "mash", smw_frames, rigid, False),
        ("stunt default", "stunt", "walk", stunt_frames, d0 + "," + d1, True),
        ("stunt tied", "stunt", "walk", stunt_frames, d0 + ",3,5,5,5,0,0,0,0,4,1,1,5,0", False),
        ("stunt rigid +5", "stunt", "walk", stunt_frames, rigid, False),
    ]
    flats = {}
    bad = 0
    for label, game, kind, frames, slots, expect in cases:
        last = frames[-1] + 2
        key = (game, kind)
        flat_dir = "%s/work/control/%s_flat" % (ROOT, game)
        if key not in flats:
            flats[key] = play(GAMES[game], flat_dir, last, frames, kind)
        eye_dir = "%s/work/control/eye" % ROOT
        rc = play(GAMES[game], eye_dir, last, frames, kind, slots)
        name = re.sub(r"\W+", "_", label)
        c, s, fl, other = score_run(name, flat_dir, eye_dir, frames, reach_of(slots), ROOT + "/crops/control")
        got = bool(fl)
        ok = got == expect and c > 0 and not rc
        bad += not ok
        print("%s %-22s expect=%-5s got=%-5s checked=%d skipped=%d rc=%s other=%d flagged=%s"
              % ("ok " if ok else "BAD", label, expect, got, c, s, rc, other, [(x[0], x[1]) for x in fl][:6]))
    shutil.rmtree(ROOT + "/work/control", ignore_errors=True)
    print("control: %d wrong of %d" % (bad, len(cases)))
    return bad


def write_report(results):
    json.dump(results, open(ROOT + "/results.json", "w"), indent=1)
    lines = ["# Profile pilot", "", "A pass means nothing was found in the scenes reached, on the libretro core.", "",
             "| Game | Preset | Frames checked | Skipped (hires or missing) | Scenes idle/mash/walk | Flags by preset | Errors |",
             "|---|---|---|---|---|---|---|"]
    for r in sorted(results, key=lambda r: r["game"]):
        flags = ", ".join("%s %d" % (t["preset"], len(t["flagged"])) for t in r["tried"])
        scenes = "/".join(str(r["scenes"].get(k, 0)) for k in ("idle", "mash", "walk"))
        lines.append("| %s | %s | %d | %d | %s | %s | %s |" % (r["game"], r["preset"], r["checked"], r["skipped"], scenes, flags, "; ".join(r["errors"])))
    open(ROOT + "/report.md", "w").write("\n".join(lines) + "\n")


PROMPT = (
    "This image shows two zoomed views of one part of a 2D video game frame, side by side, separated by a magenta "
    "line. Most frames like this are completely fine and the views differ only by a small sideways shift. Report a "
    "glitch only if you can see a stray line or strip of random coloured pixels in one view that has no counterpart "
    'in the other. Reply with JSON only: {"glitch": true or false}'
)


def review():
    """Asks the local model about each flagged crop. It never changes a game's preset."""
    verdicts = {}
    for path in sorted(glob.glob(ROOT + "/crops/*/*.png")):
        image = base64.b64encode(open(path, "rb").read()).decode()
        body = json.dumps({"model": MODEL, "prompt": PROMPT, "images": [image], "stream": False, "keep_alive": "1m",
                           "options": {"temperature": 0, "num_predict": 40}}).encode()
        req = urllib.request.Request("http://localhost:11434/api/generate", body, {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=300) as r:
            text = json.load(r)["response"]
        m = re.search(r'"glitch"\s*:\s*(true|false)', text, re.I)
        verdicts[os.path.relpath(path, ROOT).replace("\\", "/")] = m.group(1).lower() == "true" if m else None
    json.dump(verdicts, open(ROOT + "/review.json", "w"), indent=1)
    yes = sum(v is True for v in verdicts.values())
    print("model reviewed %d crops: %d glitch, %d no glitch, %d unreadable"
          % (len(verdicts), yes, sum(v is False for v in verdicts.values()), sum(v is None for v in verdicts.values())))


SUMMARY_MODEL = "qwen3:30b-a3b-instruct-2507-q4_K_M"


def summarize():
    """Has the local text model write a plain-English morning summary from the results.

    report.md stays the record: the model only restates numbers it's given, and summary.md says so.
    """
    results = json.load(open(ROOT + "/results.json"))
    verdicts = json.load(open(ROOT + "/review.json")) if os.path.exists(ROOT + "/review.json") else {}
    counts = {}
    for r in results:
        counts[r["preset"]] = counts.get(r["preset"], 0) + 1
    lines = []
    for r in sorted(results, key=lambda r: r["game"]):
        if r["preset"] == "default" and not r["errors"]:
            continue
        mine = [v for k, v in verdicts.items() if k.split("/")[1] == r["game"]]
        flags = ", ".join("%s %d" % (t["preset"], len(t["flagged"])) for t in r["tried"])
        lines.append("%s: preset %s; flagged frames by preset: %s; local vision model said glitch on %d of %d crops; errors: %s"
                     % (r["game"], r["preset"], flags, sum(v is True for v in mine), len(mine), "; ".join(r["errors"]) or "none"))
    facts = "Games run: %d. Presets chosen: %s.\nGames not on default:\n%s" % (
        len(results), ", ".join("%s %d" % kv for kv in sorted(counts.items())), "\n".join(lines))
    prompt = (
        "You are summarising an overnight test run for the maintainer of a 3D mode for a SNES emulator. Each game was "
        "played headless and given the deepest depth preset (default, then tied, then shallow) with no visual junk found; "
        "games flagged at every preset are marked review. A pass only means nothing was found in the scenes reached. "
        "Write a short plain-English summary: the totals, the games marked review, the games with errors, and the games "
        "where the vision model disagreed with the flags. Use only the facts below. Do not guess causes and do not add "
        "numbers that are not below.\n\nFACTS:\n" + facts)
    body = json.dumps({"model": SUMMARY_MODEL, "prompt": prompt, "stream": False, "keep_alive": "1m",
                       "options": {"temperature": 0, "num_ctx": 16384}}).encode()
    req = urllib.request.Request("http://localhost:11434/api/generate", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=1800) as r:
        text = json.load(r)["response"]
    open(ROOT + "/summary.md", "w", encoding="utf-8").write(
        "# Overnight summary\n\nWritten by the local model %s from results.json. Check numbers against report.md.\n\n%s\n\n"
        "## Facts it was given\n\n```\n%s\n```\n" % (SUMMARY_MODEL, text.strip(), facts))
    print("wrote summary.md")


def zero_game(r):
    """Replays the input kinds that flagged at default with stereo on and every slot at 0."""
    work = "%s/work/zero/%s" % (ROOT, r["game"])
    frames = list(range(STEP, FRAMES, STEP))
    res = {"game": r["game"], "default": len(r["tried"][0]["flagged"]), "zero": 0, "checked": 0, "rc": []}
    for kind in sorted({k for k, *_ in r["tried"][0]["flagged"]}):
        flat, eye = "%s/flat_%s" % (work, kind), "%s/eye_%s" % (work, kind)
        res["rc"] += [play(r["rom"], flat, FRAMES, frames, kind), play(r["rom"], eye, FRAMES, frames, kind, ",".join(["0"] * 26))]
        c, _, fl, _ = score_run("%s_zero_%s" % (r["game"], kind), flat, eye, frames, 0, "%s/crops_zero/%s" % (ROOT, r["game"]))
        res["checked"] += c
        res["zero"] += len(fl)
    shutil.rmtree(work, ignore_errors=True)
    return res


def zero():
    """Eyes at zero depth must match the flat picture. A flag here means the stereo path or the
    emulation drifted from the flat run, so that game's flags can't be blamed on depth."""
    todo = [r for r in json.load(open(ROOT + "/results.json")) if r["preset"] != "default" and r["tried"][0]["flagged"]]
    out = []
    with concurrent.futures.ThreadPoolExecutor(WORKERS) as pool:
        for res in pool.map(zero_game, todo):
            out.append(res)
            print(res["game"], "default", res["default"], "zero", res["zero"], "rc", res["rc"], flush=True)
    json.dump(out, open(ROOT + "/zero_check.json", "w"), indent=1)
    bad = [x["game"] for x in out if x["zero"] or any(x["rc"])]
    print("zero depth: %d games, %d frames, flagged or failed: %s" % (len(out), sum(x["checked"] for x in out), bad or "none"))
    return not bad


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "run"
    if what not in ("review", "summarize") and not os.environ.get("SNES_ROM_DIR"):
        sys.exit("Set SNES_ROM_DIR to the folder that holds the SNES ROMs.")
    if what == "control":
        return 1 if control() else 0
    if what == "zero":
        return 0 if zero() else 1
    if what == "review":
        review()
        return 0
    if what == "summarize":
        summarize()
        return 0
    games = sys.argv[2:] or list(GAMES)
    if games == ["all"]:
        # Every ROM in the folder; the pilot's short names stay for the games they cover.
        known = set(GAMES.values())
        for rom in sorted(os.listdir(ROMDIR)):
            if rom.lower().endswith((".sfc", ".smc")) and rom not in known:
                GAMES[re.sub(r"\W+", "_", os.path.splitext(rom)[0]).strip("_")[:60]] = rom
        games = list(GAMES)
    missing = [g for g in games if not os.path.exists(ROMDIR + GAMES[g])]
    if missing:
        print("missing ROMs:", missing)
        games = [g for g in games if g not in missing]
    if os.name == "nt":
        # Ask Windows not to idle-sleep while this process runs; it lapses when the process exits.
        import ctypes
        ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001)
    results = []
    if what == "resume" and os.path.exists(ROOT + "/results.json"):
        # Keep finished games, so an interrupted run picks up where it stopped.
        results = json.load(open(ROOT + "/results.json"))
        done = {r["game"] for r in results}
        games = [g for g in games if g not in done]
        print("resuming: %d done, %d to go" % (len(done), len(games)), flush=True)
    with concurrent.futures.ThreadPoolExecutor(WORKERS) as pool:
        for r in pool.map(run_game, games):
            results.append(r)
            print(r["game"], r["preset"], "checked", r["checked"], "skipped", r["skipped"], "errors", len(r["errors"]), flush=True)
            write_report(results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
