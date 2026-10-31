# Comparative Spec: Building on rcmz's `parallax3d`

**Date:** 2026-10-31
**Status:** adoption decided by the maintainer on 2026-10-31. Build and hardware results go in the ledger, not here.
**Required by:** the RULES in the project `CLAUDE.md`: no peer-fork code without a comparative spec citing the source commits.

## Sources compared

| Tree | Commit | Date |
|---|---|---|
| rcmz `parallax3d` | `9f6f2ba` | 2026-09-08 |
| Ours, shipped: `release/stereo-edition-2026.05.27` | `1a1b9f2` | 2026-05-27 |
| Ours, unshipped WIP on `feat/hud-seam-and-lateral-crop` | saved as `analysis/wip-snapshots/2026-10-31-hud-seam-uncommitted.patch` | not committed |
| matbo87 `master` | `a295d02` | 2026-09-11 |
| JorgeHSantana `master` (read only, nothing adopted here) | `cce1ac9` | 2026-09-26 |

## Why this history

The branch starts from rcmz's commits unchanged, so 64 commits stay under their authorship (`rcmz0`) and `git blame` credits them. This is the "rebase on rcmz" mechanic the 2026-09-02 arena chose. Our earlier work stays reachable on `fork/release/stereo-edition-2026.05.27` and its tags, so the first-to-ship history (`4c6a21f`, 2026-02-27) is not lost.

`upstream/master` was merged in, not `upstream/develop`. `develop` adds 5 more conflicting files, and its new updater (`3dsupdater.cpp`) fetches releases from `matbo87/snes9x_3ds`, which would replace a stereo build with a mono one. Taking `develop` later means repointing or removing that updater first.

## Mechanism by mechanism

| Mechanism | rcmz | Ours | Decision |
|---|---|---|---|
| **Depth model** | 13 plane-and-priority slots in 2 mode families. Pixel shifts come from a vertex shader table (`slotOffset[17]`). | 7 per-layer gauges plus per-tile depth from the geometry shader (`stereoOffset`). | **Take rcmz.** Priority is what separates Super Metroid's HUD from its rain, on the same BG3. The arena also rated our fan-out a marginal refinement. |
| **Out-of-box behaviour** | `Depth3DEnabled = false`, every slot 0. The user tunes each game. | Auto depth from the emulator's compositing values. | **Keep ours in concept, but recalibrated.** See "Auto defaults". |
| **Mode 7** | Depth shape measured from each scanline's scale. A plane with no perspective stays flat. | Depth from screen Y, whether or not the plane has perspective. | **Take rcmz.** It measures something real, and top-down planes no longer get a false ramp. |
| **Priority gaps** | `Depth3DFillGaps` extends the further-back priority into the gap. | None. | **Take rcmz.** |
| **Screen edges** | `Depth3DCropEdges` cuts tiles back to the screen and crops the widest shift. | Lateral crop on the unshipped WIP only. | **Take rcmz.** Drop our crop. |
| **Right-eye VRAM failure** | Stereo reports unsupported and the game keeps running. | Exits to the Homebrew Launcher. | **Take rcmz.** |
| **Tuning UI** | Per-slot sliders with live previews of each slot's contents. | Gauges without previews. | **Take rcmz.** |
| **Status bar seam** | A separate `BG3_PRIO1_FRONT` slot can be set to match the map. | WIP re-anchor to BG0, never hardware-validated. | **Express it as a default** on rcmz's slot. Don't port the code. |
| **Tile-edge overlap** | None. | WIP widens each quad. | **Do not port.** The project records small in-place fixes as already tried without success, and the overlap is unvalidated. Patch kept for reference. |
| **SuperFX performance** | Not included. | Wyatt-James's series, `ef11ac0` and `729a461`. | **Do not port yet.** Wyatt's work merged upstream to `develop` in #85 and is a later version; it arrives when `develop` does. |
| **Game Screen BG depth gauge** | Inherits upstream's. | `dfc670c`. | **Drop.** matbo87 shipped their own for #65. |
| **Mosaic, NDSP, Mode 7 smoothing** | Inherited from upstream. | Our PRs #58, #59, #67, #68. | **Already present.** They came through upstream. |

## Auto defaults

This is what we add. rcmz's stated position is that hand-set depth can't infer wrongly. Ours is that a feature off by default, at zero depth, doesn't reach people. Both can hold: hand-set slots win, and Auto only supplies a starting point.

Porting our old Auto unchanged would reintroduce matbo87's two calibration reports, so the seed changes:

- **Playfield at the screen plane.** Old Auto popped BG1 forward, which read as "backgrounds come forward".
- **Sprites anchored.** Old Auto had no zero point for OBJ, so sprites floated. Sprite priority 1 (depth 6) now sits at the plane.
- **Same data source.** rcmz's `depth3dSlotDepth()` already holds the compositing values, so no second table is needed.

The numbers are a guess until tested on hardware. The ledger lists which reports they are meant to address.

## Hostile self-audit

- **Unverified code:** the merge and Auto seed are AI-written. Nothing here counts as working until it has been built and run on a console.
- **Narrow-ROM validation:** the test matrix must include games that don't suit layer depth (Street Fighter II, A Link to the Past), not only the flattering ones.
- **Dead code:** `SMenuTab::VisibleItems()` lost its callers in this merge and should be removed after the first clean compile.
- **Dependency risk:** rcmz has not committed since 2026-09-08. We can carry this branch alone if needed, because the code is in our history.
- **Not solved by any of this:** disocclusion, sub-screen colour math, windowed effects, and scenery painted into a single layer. See the ledger.
