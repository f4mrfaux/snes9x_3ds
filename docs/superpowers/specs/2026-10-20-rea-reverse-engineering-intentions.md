# Intentions — Using REA to Reverse Engineer SNES Games for Proper Stereo Depth

**Date:** 2026-10-20
**Status:** INTENTION — no code yet. Nothing here lands without a spec + hostile self-audit per CLAUDE.md RULES.
**Tool:** REA "Reverse Engineer Anything" — [morluto/rea](https://github.com/morluto/rea) v6.7.0 (MIT), cloned locally and registered as an MCP server for Claude Code.

## Why

Every depth decision we make today is inferred from the PPU at runtime: `snesDepthTable` maps
`PPU.BGMode` + DRAW_* d0/d1 to a factor, and the engine has no idea what a layer *means*. That is the
root of all three of matbo's SMW reports:

- **BG1 pops forward** because the engine can't tell "level tiles" (should sit at screen plane) from "scenery".
- **Mario floats** because OBJ priority is a draw-order hint, not a depth.
- **World-map top-left** is OBJ disocclusion — pixels that don't exist in the mono frame.

The game itself knows all of this. Its code decides which layer is the HUD, which is the playfield, which
is parallax sky, and what game mode it's in. Reverse engineering the game gives us *semantic* depth
instead of guessed depth.

## Intentions

### 1. Semantic depth from the ROM, not heuristics
Use REA (with a Ghidra 65816/SNES loader — verify which extension REA's Ghidra provider can drive) to map,
per target game:
- the game-mode / scene variable in WRAM (e.g. SMW's game-mode byte), so we know overworld vs level vs
  title vs boss without PPU-signature guessing;
- which BG layer carries HUD, playfield, and far background in each mode;
- where the game writes scroll registers and HDMA tables for parallax, so depth can follow the game's own
  parallax ratios (a layer that scrolls at 1/2 speed is "farther" than one at 1/1).

Output is **data**, not emulator code: a per-game depth profile (WRAM address + value → per-layer depth),
loaded the same way per-game settings are today. This is the evidence-backed version of Jorge's
PPU-signature scene profiles, and it maps naturally onto rcmz's slot model for upstream PRs.

### 2. Close the disocclusion class properly
M2's 3D Classics solve edge/disocclusion by rendering past the visible 256px. SNES tilemaps are usually
512px wide and already in VRAM, so the data often exists — what's missing is knowing, per game, whether
the off-screen columns are valid (some games stream tiles in just before they scroll on). RE of each
game's tile-streaming routine tells us how many columns are safe to overdraw.

### 3. Learn M2's *technique* from our own 3D Classics dumps — clean-room only
Analysing how M2's 3DS titles do layer separation is fine for understanding the approach
(interoperability/education). **We do not copy M2/Nintendo code or assets** into this repo. Any idea taken
from that analysis gets written up as a mechanism in a spec doc first, then implemented fresh — the same
audit discipline the RULES require for peer-fork adoption.

### 4. Optimize our own binary
Point REA at our own `.elf`/`.3dsx` to inspect what devkitARM actually emitted for hot paths
(`gpu3dsDrawLayers`, the stereo draw loop, tile vertex building) and confirm the per-eye replay adds no
hidden CPU work. Our code, our binary — no restrictions.

## Boundaries

- Only ROMs and 3DS titles dumped from cartridges/consoles I own. No ROMs, dumps, or extracted assets
  ever get committed or distributed.
- Profiles we ship contain addresses and depth numbers only — no game code or data.
- Follows REA's own `DISCLAIMER.md`: no DRM/protection circumvention, no reproducing proprietary code.

## First experiment (when ready)

1. `npx rea-agents setup` for Claude Code only; install Ghidra + a 65816 loader.
2. Load SMW (own dump). Locate the game-mode byte and the routines that set BG1/BG2/BG3 roles on the
   overworld vs in a level.
3. Hand-write a one-game profile and HW-test it against matbo's three reports.
4. If it beats Auto on all three, write the profile format spec before generalising.

## Known setup gaps

- REA's bundled `unblob` submodule can't check out its pinned commit on Windows (test fixtures have
  NTFS-invalid names); it sits at `cfb6c5a`. Only matters for firmware unpacking, not SNES work.
