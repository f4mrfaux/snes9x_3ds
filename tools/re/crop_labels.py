"""Labels flagged crops from profile_pilot.py and trains a small classifier on them.

  crop_labels.py sample [N]     pick N crops spread across games (default 100) into the queue
  crop_labels.py serve          labelling page at http://127.0.0.1:8765 (J junk, F fine, U unsure)
  crop_labels.py features       replay every queued frame and measure each flagged strip
  crop_labels.py train [names]  cross-validate by game, then fit on every label (all measurements, or the named ones)
  crop_labels.py rank           order the review list by the layer-missing-from-row signal (review_ranked.md)

The classifier never sees the images, only what the scorer measured, so it trains in seconds on the
CPU. Splitting by game matters: crops from one game look alike, so a split by crop would flatter it.
Files live in the labels folder under profile_pilot.py's output folder ($S3RE_ROOT/pilot/labels), outside
the repository.
"""
import concurrent.futures
import http.server
import json
import math
import os
import random
import shutil
import sys
import urllib.parse

import profile_pilot as pp

LABELS = pp.ROOT + "/labels"
QUEUE, ANSWERS, FEATURES, MODEL_FILE = (LABELS + "/" + n for n in ("queue.json", "labels.json", "features.json", "classifier.json"))
CHOICES = {"junk", "fine", "unsure"}


def flagged_items():
    """Every flagged frame in results.json, with what's needed to find its crop and replay it."""
    items = []
    for r in json.load(open(pp.ROOT + "/results.json")):
        for t in r["tried"]:
            slots = next(p[1] + "," + p[2] for p in pp.PRESETS if p[0] == t["preset"])
            for kind, frame, pixels, crop in t["flagged"]:
                if crop and os.path.exists(crop):
                    items.append({"id": os.path.basename(crop)[:-4], "game": r["game"], "rom": r["rom"], "preset": t["preset"],
                                  "slots": slots, "kind": kind, "frame": frame, "pixels": pixels, "crop": crop})
    return items


def sample(n):
    """Up to n crops, round-robin across games so one noisy game can't fill the queue."""
    by_game = {}
    for item in flagged_items():
        by_game.setdefault(item["game"], []).append(item)
    rng = random.Random(1108)
    for items in by_game.values():
        rng.shuffle(items)
    picked = []
    while len(picked) < n and any(by_game.values()):
        for game in sorted(by_game):
            if by_game[game] and len(picked) < n:
                picked.append(by_game[game].pop())
    rng.shuffle(picked)
    os.makedirs(LABELS, exist_ok=True)
    json.dump(picked, open(QUEUE, "w"), indent=1)
    print("queued %d crops from %d games" % (len(picked), len({p["game"] for p in picked})))


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>Crop labels</title>
<style>
body{font:16px system-ui,sans-serif;background:#111;color:#eee;margin:16px}
img{image-rendering:pixelated;max-width:100%;max-height:70vh;border:1px solid #444;background:#000}
button{font-size:18px;margin:6px;padding:10px 18px;border-radius:6px;border:0;cursor:pointer}
#j{background:#c33;color:#fff}#f{background:#3a3;color:#fff}#u{background:#666;color:#fff}
.small{color:#aaa;font-size:14px}
</style></head><body>
<p class="small">Two zoomed views, left eye and right eye, split by a magenta line. Is there junk in one view only: a stray
line, a strip of noise or wrong tiles? Normal 3D shifting and see-through effects are fine.</p>
<p id="where" class="small"></p><img id="img"><div>
<button id="j" onclick="mark('junk')">Junk (J)</button><button id="f" onclick="mark('fine')">Fine (F)</button>
<button id="u" onclick="mark('unsure')">Unsure (U)</button><button onclick="go(-1)">Back</button></div>
<script>
let items=[],labels={},i=0;
async function load(){const r=await fetch('/api/items');const d=await r.json();items=d.items;labels=d.labels;
 i=items.findIndex(x=>!(x.id in labels));if(i<0)i=0;show();}
function show(){if(!items.length){where.textContent='Nothing queued.';return}
 const x=items[i],done=Object.keys(labels).length;
 where.textContent=(i+1)+' of '+items.length+' - '+done+' labelled'+(labels[x.id]?' - this one: '+labels[x.id]:'');
 img.src='/crop/'+encodeURIComponent(x.id);}
async function mark(l){const x=items[i];await fetch('/api/label',{method:'POST',body:JSON.stringify({id:x.id,label:l})});
 labels[x.id]=l;go(1);}
function go(d){i=Math.max(0,Math.min(items.length-1,i+d));show();}
document.onkeydown=e=>{const k=e.key.toLowerCase();if(k=='j')mark('junk');if(k=='f')mark('fine');if(k=='u')mark('unsure');
 if(k=='arrowleft')go(-1);if(k=='arrowright')go(1);};
load();
</script></body></html>"""


def read_answers():
    return json.load(open(ANSWERS)) if os.path.exists(ANSWERS) else {}


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, code, kind, body):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        items = json.load(open(QUEUE)) if os.path.exists(QUEUE) else []
        if self.path == "/":
            return self.send(200, "text/html; charset=utf-8", PAGE.encode())
        if self.path == "/api/items":
            # The game names and model verdicts stay hidden, so they can't sway the label.
            body = {"items": [{"id": x["id"]} for x in items], "labels": read_answers()}
            return self.send(200, "application/json", json.dumps(body).encode())
        if self.path.startswith("/crop/"):
            wanted = urllib.parse.unquote(self.path[6:])
            for x in items:
                if x["id"] == wanted:
                    return self.send(200, "image/png", open(x["crop"], "rb").read())
        self.send(404, "text/plain", b"not found")

    def do_POST(self):
        if self.path != "/api/label":
            return self.send(404, "text/plain", b"not found")
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        if body.get("label") not in CHOICES:
            return self.send(400, "text/plain", b"bad label")
        answers = read_answers()
        answers[body["id"]] = body["label"]
        json.dump(answers, open(ANSWERS, "w"), indent=1)
        self.send(200, "application/json", b"{}")

    def log_message(self, *args):
        pass


def serve():
    print("labelling page: http://127.0.0.1:8765  (Ctrl+C to stop)")
    http.server.ThreadingHTTPServer(("127.0.0.1", 8765), Handler).serve_forever()


def strip_features(flat, eye, reach, flat_z):
    """What the scorer saw in one eye, as numbers. All zero when nothing sprite-backed was found."""
    pixels, strips, other = pp.score_eye(flat, eye, reach, flat_z)
    if not strips:
        return None
    columns = sorted({s[0] for s in strips})
    groups = 1 + sum(1 for a, b in zip(columns, columns[1:]) if b - a > 1)
    colours = set()
    contrast = []
    for x, y0, y1, _ in strips:
        for y in range(y0, y1 + 1):
            c = eye[y][x]
            colours.add(c)
            beside = eye[y][x - 1] if x > 0 else c
            contrast.append(sum(abs((c >> s & 255) - (beside >> s & 255)) for s in (0, 8, 16)) / 765)
    width = len(eye[0])
    return {
        "pixels": pixels,
        "other": other,
        "strips": len(strips),
        "groups": groups,
        "columns_per_group": len(columns) / groups,
        "tallest": max(s[2] - s[1] + 1 for s in strips),
        "colours": len(colours),
        "colours_per_pixel": len(colours) / pixels,
        "contrast": sum(contrast) / len(contrast),
        "edge": min(min(columns), width - 1 - max(columns)) / width,
        "top": min(s[1] for s in strips) / len(eye),
        "reach": reach,
    }


BACKDROP_Z = 1      # the backdrop fills wherever no layer drew (tile.c DrawBackdrop*)


def occlusion_features(flat, eye, reach, flat_z, eye_z):
    """Where each flagged pixel's content comes from, from the layer maps, as fractions of those pixels.

    backdrop: no layer drew there, so the backdrop colour shows. backdrop_absent_row: and the flat row never
    shows the backdrop, so the game kept it covered (Timon & Pumbaa's magenta).
    hidden: the eye's layer isn't within reach on that row of the flat picture, so something covered it there.
    hidden_by_sprite: hidden, and a sprite was the cover. layer_absent_row: hidden from the whole flat row.
    same_layer: that layer is nearby in the flat picture, just in a new colour, which is usually colour math.
    eye_sprite: the pixel belongs to a sprite in the eye.
    """
    _, strips, _ = pp.score_eye(flat, eye, reach, flat_z)
    if not strips:
        return None
    marks = set(pp.foreign_pixels(flat, eye, reach))
    crop = (len(flat[0]) - len(eye[0])) // 2
    n = {"backdrop": 0, "backdrop_absent_row": 0, "hidden": 0, "hidden_by_sprite": 0, "layer_absent_row": 0,
         "same_layer": 0, "eye_sprite": 0}
    total = 0
    for x, y0, y1, _ in strips:
        for y in range(y0, y1 + 1):
            if (x, y) not in marks:
                continue
            total += 1
            sx, row = x + crop, y * 256
            owner = eye_z[row + sx]
            near = set(flat_z[row + max(sx - reach, 0):row + min(sx + reach + 1, 256)])
            if owner == BACKDROP_Z:
                n["backdrop"] += 1
                n["backdrop_absent_row"] += BACKDROP_Z not in flat_z[row:row + 256]
            elif owner in pp.SPRITE_Z:
                n["eye_sprite"] += 1
            elif owner in near:
                n["same_layer"] += 1
            else:
                n["hidden"] += 1
                n["hidden_by_sprite"] += flat_z[row + sx] in pp.SPRITE_Z
                n["layer_absent_row"] += owner not in flat_z[row:row + 256]
    return {"occ_" + k: v / total for k, v in n.items()}


def measure(job):
    """Replays one game, preset and input up to its last queued frame and measures each queued crop."""
    n, (rom, slots, kind), items = job
    frames = sorted({x["frame"] for x in items})
    work = "%s/work/labels/%d" % (pp.ROOT, n)
    # Stopping just past the last frame keeps every earlier button hold, so the frames still match.
    pp.play(rom, work + "/flat", frames[-1] + 2, frames, kind)
    pp.play(rom, work + "/eye", frames[-1] + 2, frames, kind, slots)
    rows = []
    for x in items:
        f = x["frame"]
        try:
            flat = pp.to_rows(*pp.read_ppm("%s/flat/f%05d_M.ppm" % (work, f)))
            left = pp.to_rows(*pp.read_ppm("%s/eye/f%05d_L.ppm" % (work, f)))
            right = pp.to_rows(*pp.read_ppm("%s/eye/f%05d_R.ppm" % (work, f)))
            flat_z = open("%s/flat/z_%05d_L.bin" % (work, f), "rb").read()
            eye_zs = [open("%s/eye/z_%05d_%s.bin" % (work, f, e), "rb").read() for e in "LR"]
        except OSError:
            print("missing frame", x["id"], flush=True)
            continue
        reach = pp.reach_of(slots)
        per_eye = []
        for picture, eye_z in zip((left, right), eye_zs):
            e = strip_features(flat, picture, reach, flat_z)
            if e:
                e.update(occlusion_features(flat, picture, reach, flat_z, eye_z))
                per_eye.append(e)
        if not per_eye:
            print("no strips on replay, frames may not match:", x["id"], flush=True)
            continue
        merged = {k: max(e[k] for e in per_eye) for k in per_eye[0]}
        merged["eyes"] = len(per_eye)
        rows.append({"id": x["id"], "game": x["game"], "features": merged})
    shutil.rmtree(work, ignore_errors=True)
    return rows


def features():
    """Measures every queued crop, labelled or not, so changing an answer only needs train again.
    Replays are deterministic, so the frames match the ones that were labelled."""
    queue = json.load(open(QUEUE))
    groups = {}
    for x in queue:
        groups.setdefault((x["rom"], x["slots"], x["kind"]), []).append(x)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(pp.WORKERS) as pool:
        for part in pool.map(measure, [(n, k, v) for n, (k, v) in enumerate(groups.items())]):
            rows += part
    json.dump(rows, open(FEATURES, "w"), indent=1)
    print("measured %d of %d queued crops" % (len(rows), len(queue)))


def fit(rows, names, steps=3000, rate=0.1, l2=0.01):
    """Plain logistic regression on log-scaled, standardised features. Small and easy to read back."""
    xs = [[math.log1p(max(r["features"][n], 0)) for n in names] for r in rows]
    mean = [sum(c) / len(c) for c in zip(*xs)]
    spread = [math.sqrt(sum((v - m) ** 2 for v in c) / len(c)) or 1 for c, m in zip(zip(*xs), mean)]
    xs = [[(v - m) / s for v, m, s in zip(x, mean, spread)] for x in xs]
    ys = [1 if r["label"] == "junk" else 0 for r in rows]
    w, b = [0.0] * len(names), 0.0
    for _ in range(steps):
        gw, gb = [0.0] * len(names), 0.0
        for x, y in zip(xs, ys):
            p = 1 / (1 + math.exp(-(b + sum(a * c for a, c in zip(w, x)))))
            gb += p - y
            gw = [g + (p - y) * c for g, c in zip(gw, x)]
        b -= rate * gb / len(xs)
        w = [a - rate * (g / len(xs) + l2 * a) for a, g in zip(w, gw)]
    return {"names": names, "mean": mean, "spread": spread, "weights": w, "bias": b}


def predict(model, feats):
    x = [(math.log1p(max(feats[n], 0)) - m) / s for n, m, s in zip(model["names"], model["mean"], model["spread"])]
    return 1 / (1 + math.exp(-(model["bias"] + sum(a * c for a, c in zip(model["weights"], x)))))


def cross_validate(rows, names, folds=4, shuffles=5):
    """Each crop is predicted by a model that never saw its game. Returns (junk caught, junk, fine called junk, fine)
    averaged over shuffles of which games share a fold."""
    games = sorted({r["game"] for r in rows})
    caught = wrong = 0
    for seed in range(shuffles):
        random.Random(seed).shuffle(games)
        for k in range(folds):
            held = set(games[k::folds])
            model = fit([r for r in rows if r["game"] not in held], names, steps=1500)
            for r in rows:
                if r["game"] in held and predict(model, r["features"]) >= 0.5:
                    caught += r["label"] == "junk"
                    wrong += r["label"] != "junk"
    junk = sum(r["label"] == "junk" for r in rows)
    return caught / shuffles, junk, wrong / shuffles, len(rows) - junk


def train(names=None):
    answers = read_answers()
    rows = [dict(r, label=answers[r["id"]]) for r in json.load(open(FEATURES)) if answers.get(r["id"]) in ("junk", "fine")]
    every = sorted(rows[0]["features"])
    sets = {"strip shape": [n for n in every if not n.startswith("occ_")],
            "occlusion": [n for n in every if n.startswith("occ_")], "both": every}
    if names:
        sets = {"chosen": names}
    junk = sum(r["label"] == "junk" for r in rows)
    print("labelled: %d junk, %d fine, from %d games. Always answering fine is right %.0f%% of the time."
          % (junk, len(rows) - junk, len({r["game"] for r in rows}), 100 * (len(rows) - junk) / len(rows)))
    print("cross-validated by game, every crop predicted by a model that never saw its game:")
    for label, ns in sets.items():
        tp, pos, fp, neg = cross_validate(rows, ns)
        print("  %-12s junk caught %.1f of %d, fine called junk %.1f of %d, right %.0f%%"
              % (label, tp, pos, fp, neg, 100 * (tp + neg - fp) / len(rows)))
    names = names or every
    model = fit(rows, names)
    json.dump(model, open(MODEL_FILE, "w"), indent=1)
    for n, w in sorted(zip(names, model["weights"]), key=lambda t: -abs(t[1])):
        print("  %-18s %+.2f" % (n, w))


def covered(f):
    """A strip shows a layer, or the backdrop, that the whole flat row keeps covered."""
    return f["occ_layer_absent_row"] > 0 or f["occ_backdrop_absent_row"] > 0


def rank(per_game=10):
    """Orders the review list by the one signal that held up on the maintainer's labels: a strip showing a layer or the
    backdrop that the whole row of the flat picture keeps covered. Samples up to per_game of each review
    game's frames flagged at default. A ranking for people, not a verdict."""
    slots = next(p[1] + "," + p[2] for p in pp.PRESETS if p[0] == "default")
    review = [r for r in json.load(open(pp.ROOT + "/results.json")) if r["preset"] == "review"]
    jobs = []
    for r in review:
        flagged = r["tried"][0]["flagged"]
        by_kind = {}
        for kind, frame, _, _ in flagged[::max(1, len(flagged) // per_game)][:per_game]:
            by_kind.setdefault(kind, []).append({"id": "%s_%s_f%05d" % (r["game"], kind, frame), "game": r["game"], "frame": frame})
        jobs += [(len(jobs) + i, (r["rom"], slots, kind), items) for i, (kind, items) in enumerate(by_kind.items())]
    with concurrent.futures.ProcessPoolExecutor(pp.WORKERS) as pool:
        rows = [x for part in pool.map(measure, jobs) for x in part]
    table = []
    for r in review:
        mine = [x["features"] for x in rows if x["game"] == r["game"]]
        absent = sum(covered(f) for f in mine)
        hidden = sum(f["occ_hidden"] for f in mine) / len(mine) if mine else 0
        table.append((r["game"], len(mine), absent, hidden, "/".join(str(len(t["flagged"])) for t in r["tried"])))
    table.sort(key=lambda t: (-(t[2] / t[1] if t[1] else -1), -t[3]))
    json.dump(table, open(pp.ROOT + "/review_ranked.json", "w"), indent=1)
    lines = ["# Review list, ranked", "",
             "Sorted by the share of sampled flagged frames where a strip shows a layer or the backdrop that the whole row of",
             "the flat picture keeps covered. See the ledger for how often that was junk on the maintainer's labels; it misses most junk,",
             "so a low rank doesn't mean clean. Games with 0 frames measured had nothing scorable (hires).", "",
             "| Game | Frames measured | Covered layer or backdrop shown | Hidden-layer share | Flags default/tied/shallow |",
             "|---|---|---|---|---|"]
    lines += ["| %s | %d | %d | %.2f | %s |" % t for t in table]
    open(pp.ROOT + "/review_ranked.md", "w").write("\n".join(lines) + "\n")
    print("ranked %d review games from %d frames: review_ranked.md" % (len(table), len(rows)))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "sample":
        sample(int(sys.argv[2]) if len(sys.argv) > 2 else 100)
    elif what == "serve":
        serve()
    elif what == "features":
        features()
    elif what == "train":
        train(sys.argv[2:])
    elif what == "rank":
        rank()
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
