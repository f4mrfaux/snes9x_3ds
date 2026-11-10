# Definition of Done

**Date:** 2026-11-01
**Decided by:** the maintainer, answering whether safe depth for all games plus a tested profile list is the goal: "that seems done? id also lik minimal graphical artifacts like the green lines in mario, the edges in metroid, and that wierd ass 4x4 game we used to test snes9x".
**The 4x4 game** is Stunt Race FX, confirmed by the maintainer the same day. It's also the hardware evidence in the 2026-05-27 edge design.

Everything below is judged on the maintainer's New 3DS XL. Azahar and the libretro core can find faults and explain causes, but they can't pass a game. Old 3DS stays unverified unless a tester reports. The console was in pieces on 2026-11-02, with a repair planned for that weekend, so work continues against Azahar until then.

## Done means

1. **Every game gets safe depth.** It boots and runs with conservative defaults, and with the 3D slider at 0 it looks flat.
2. **The named artifacts are gone.** They pass at full slider with default settings, as in the table below.
3. **Artifact types we understand are handled by default.** A new game shouldn't show one of them without a recorded reason.
4. **A named list of games has tuned profiles.** Each one is tested on hardware. Other games keep the defaults.

## Named artifacts

| Artifact | Where to look | Cause as understood | What should handle it | Pass |
|---|---|---|---|---|
| Green lines above the lives counter | SMW world map, top left | Parallax between the OBJ priority 3 plug and BG3 front exposes the map. Measured on libretro. | Keep both slots at the same depth by default, and in any SMW profile | No line at any slider position, on any map |
| Screen edge strips | Super Metroid, rooms that scroll. Also Crateria rain, where the old build's right eye showed a rolling black strip | Layer shift exposes off-screen tile columns and an unfilled gap. The rain's cause under the new per-eye path is unknown | Crop screen edges, on by default | No strip or shimmer at either edge of either eye, including during rain |
| Garbage at the picture's sides | Stunt Race FX, Speed Trax race. It sits inside the race picture, not at the screen edge | A sprite plug: priority 3 bars cover garbage at the sides of BG1, and parallax between them exposes it. Measured on libretro | A Stunt Race FX profile, because crop doesn't reach it: `OBJ prio 3` and `BG1 prio 1` at 0, `BG2 prio 1` at 5 | No garbage at either side of the picture at full slider |

None of these has passed on hardware with this branch. In Azahar on the 3DS code path, plus the test shader patch, by eye: the SMW line closes when tied; Metroid's rain edges are straight with crop on, in one frame each; Stunt Race FX stays clean whenever `OBJ prio 3` equals `BG1 prio 1`. From code reading, Crop screen edges clips tiles to the screen before shifting them, then removes the widest shift from all four edges of both eyes. The Stunt Race FX profile keeps depth only in the sky band, because the car, road and frame all share BG1.

## Accepted limits

These are tracked in the ledger and don't block done:

- Scenery painted into one layer can't be separated, as in Street Fighter II. It should stay flat rather than break.
- Depth can't reveal pixels the game never drew, so gap fill and crop only hide those strips.
- The crop costs a few pixels of picture at every edge while depth is on.
- Windows at screen edges and Mode 7 line windows may still show small errors if a port of the libretro clip fix isn't needed or isn't possible.

## Not part of done

- Precise depth on every ROM by automation. The ledger records why it isn't feasible.
- Profiles for the whole library, or TAS coverage of it.
- Runtime plug detection. The evidence so far is SMW and Stunt Race FX, with different layer pairs and with the sprites on opposite sides of their layer, and the geometry rule mostly fired on false positives.

## Order of work

1. Merged build boots: Azahar smoke test of `bc7b2e1`, then the console.
2. Console test of the three named artifacts. This decides whether the edge crop can be kept as it is.
3. Replace the unsourced default depths, using what those tests show.
4. The maintainer picks the profile list. Starting suggestion: SMW, Super Metroid and Stunt Race FX, then the games in Step 3 of the hardware test plan.
5. Build per-game profiles, and per-scene ones only where a listed game needs them.

Per-game starting depths exist as a small table in `source/3dssettings.cpp`, started before step 2 because Azahar now shows this path rendering the named games correctly. Super Metroid's entry uses rcmz's README values and is untested. Don't build per-scene code before step 2.
