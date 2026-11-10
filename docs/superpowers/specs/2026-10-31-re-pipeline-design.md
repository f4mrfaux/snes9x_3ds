# Reverse-Engineering Pipeline Design

**Date:** 2026-10-31
**Status:** design. Nothing in it is built yet.
**Goal:** per-scene depth from an unmodified ROM. The output is data, never game code, and no ROMs or dumps enter the repo. The boundaries in the 2026-10-20 REA intentions doc still apply.

## The idea

A hand-tuned profile has two unsolved problems: knowing what scene is on screen, and knowing what each layer means in that scene. Jorge's matcher guesses the scene from PPU registers. The game already knows, in RAM. So each pipeline below produces or checks one thing: a profile mapping a RAM value to 13 slot depths.

```
ROM (own dump) -> find scene variable -> label layers per scene -> profile -> emulator -> check on hardware
```

## Pipeline 1: scene variable finder (dynamic, do first)

Needs no Ghidra, and works on games with no decomp.

1. Run the ROM headless through scripted input in the libretro core, per frame dumping WRAM and PPU state (`$2105`, TM/TS, on-screen OBJ priorities).
2. Mark scene changes from PPU signature jumps.
3. Rank WRAM addresses by how well they change at those moments and stay stable between them.
4. Confirm by hand: does the top address match what's on screen?

**Built:** `tools/re/wram_host.cpp` records per-frame WRAM changes and picture stats from any libretro SNES core, and `tools/re/rank_scene_vars.py` ranks addresses. Keep output outside the repo, because it holds game RAM. Step 2 currently uses picture cuts and fades, because libretro doesn't expose PPU state.

**Results, 2026-11-01** (scripted input; "known" addresses recalled from community RAM maps, not independently checked):

| Game | Known scene variable | Rank | Note |
|---|---|---|---|
| Super Mario World | `$7E0100` | 2 of 22,620 | Tuned on this game: the held-value test was added after seeing it, so this isn't evidence |
| Super Metroid | `$7E0998` | 425 of 70,732 | Blind. History looks right: 8 changes through the title and game states |
| A Link to the Past | `$7E0010` | 2,848 of 57,940 | Blind. History looks right: flips as the script opens and closes the menu |

**Conclusion: the recording works, the ranking doesn't yet.** Don't use the ranked list to choose a profile address. The likely weakness is event detection: only 12 to 27 picture events were found, and scene changes without a cut or fade are missed. That's a guess, not a tested cause.

**PPU export added, 2026-11-01.** `tools/re/core-ppu-export.patch` adds a read-only `snes9x_re_ppu_state()` to the libretro core, and `wram_host` writes it to `ppu.csv`: BG mode, layers on main and sub screen, colour math, tilemap and tile bases, and on-screen sprites per priority. Applied to `fae2fea` in a separate clone (`s9x-re`). Stock and patched cores gave byte-identical RAM and picture logs on three games. Values are read at the end of each frame, so registers a game changes mid-frame show only their last value.

Using PPU setup changes as the scene events did **not** fix the ranking:

| Game | Variable | Picture events | PPU events | Blind? |
|---|---|---|---|---|
| Yoshi's Island | `$7E0118` | not run | 17,252 of 34,309 | Yes, answer fixed first |
| Super Mario Kart | `$7E0036` | not run | 27 of 107,109 | Yes, answer fixed first |
| Super Metroid | `$7E0998` | 425 | 2,059 of 70,732 | No |
| A Link to the Past | `$7E0010` | 2,848 | 27,377 of 57,936 | No |
| Super Mario World | `$7E0100` | 2 | 362 of 22,568 | No; a longer run with different input than the picture result |

**What this shows:**
- Event counts look wrong in both directions: 1 event for Yoshi's Island, 93 for SMW. SMW's come about every 85 frames, which matches the script's button timing, so button mashing may be driving them. That isn't confirmed.
- These runs can't tell a weak method from a weak test, because mashed input barely reaches distinct scenes.

**Don't use the ranked list to choose profile addresses.** Yoshi's Island and Super Mario Kart can't serve as blind tests again.

**Next options, in order:**
1. Use the data as it stands: for a game with a community RAM map, join the known variable to `ppu.csv` to get which layers and sprite priorities each scene uses. That's what a profile needs, and it requires no ranking.
2. Record a short hand-played session, so scenes are really visited and can be labelled.
3. Try scoring how well an address's value predicts the PPU setup, instead of timing changes against events. Untested idea, and it needs new hold-out games.

**TAS pilot, 2026-11-01.** `wram_host` now takes `movie=<file>[:<offset>]` and `shot=<frame>`. A Snes9x 1.43 SMW movie desynced in its first level on the current core, so direct replay isn't a coverage route on this evidence. Playing a BizHawk movie in BizHawk with `tools/re/bizhawk_smw_record.lua` reached all 96 exits and the movie's full length. That route gives RAM, VRAM, CGRAM and sprites across a whole game, but no PPU registers, so it can't by itself repeat the render-based plug test or fill `ppu.csv`. Movies and output stay outside the repo. See the ledger.

**Known weakness:** it needs input that actually reaches different scenes. The Super Metroid run reached the first room change, and little beyond that.

## Pipeline 2: decomps and RAM maps (cheap where they exist)

For Super Metroid and others with public disassemblies, read the scene variable and layer roles from those instead of hunting. Use this as ground truth for Pipeline 1: if the finder can't rediscover a known room variable, don't trust it on unknown games. Addresses and numbers only go into profiles.

## Pipeline 3: static analysis with REA (for what the others can't answer)

Use for why-questions: which routine writes a scroll register, how far ahead a game streams tile columns, whether off-screen columns are valid.

**Prerequisites, none done:**
- `npx rea-agents setup` is interactive and asks for approval before changing agent config, so it's the maintainer's step.
- Ghidra with a third-party 65816 loader. REA's raw-binary lane doesn't admit 65816, so whether REA can drive such a loader must be tested first. Its Windows Ghidra support is marked experimental, and this PC has Java 8 only.

**Fallback:** if that doesn't work, use Ghidra directly or the public disassemblies from Pipeline 2. REA isn't required for the goal.

The same tool can point at our own `.elf`, which is the safest first use and needs no ROM.

## Pipeline 4: depth check (closes the loop)

Run a profile in the headless host and measure each slot's disparity between eyes, per scene. The RetroArch session already measures disparity this way. It catches wrong-direction and floating errors before a console is involved. It does not replace judging on hardware, because it can't tell whether something looks right.

**Status:** the RetroArch session has run this with the 13-slot defaults (reported results are in the ledger). Its spike now supports per-priority BG shifts and span-based Mode 7, so slot values carry over.

**Caveat:** the libretro core and the 3DS render differently, and its Mode 7 reach lags a frame. Pixel results still need confirming on 3DS.

## Profile format (draft)

One text file per game, keyed by the ROM's internal name and checksum:

```
[scene]
name = level
ram = 0x7E0100        ; address
value = 0x14          ; value, or a list
depth01 = 0,0, 2,2, 4,0, 5,4, 2,0,0,0, 0
depth27 = 3,3, 5,5, 0,0, 0,0, 4,1,1,1, 0
```

Slot order matches `DEPTH3D_SLOT`. Hand-set per-game values still win. The address above is an example, not a real one.

**Open:** switching between scenes needs hysteresis so a brief RAM change doesn't jump depth. Jorge's matcher uses 30 frames of hysteresis and a 20-frame blend per the 2026-09-02 plan; check it before designing another.

## What success looks like

- **First game:** SMW, against matbo87's three reports, as the 2026-10-20 doc planned.
- **First set:** a small list of games with profiles, tested on hardware. Other games keep the general defaults.

## Not solved by this

Scenery painted into one layer, disocclusion and colour math at wrong depths. Profiles choose depths; they don't create layers.
