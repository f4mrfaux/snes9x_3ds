# Hardware Test Plan: Merged Branch vs rcmz Baseline

**Builds,** in the project folder:
- `analysis/builds-2026-11-01/snes9x3d-merged-364cb5f.3dsx`: this branch, with starting depths and profiles for Super Metroid and Stunt Race FX. Don't use the older `snes9x3d-merged-191a4c8.3dsx`, which quits during startup.
- `analysis/builds-2026-10-31/rcmz-baseline-9f6f2ba.3dsx`: rcmz unmodified. Its depths start at zero, so set them by hand to compare.

**Before you start:** both builds share `sd:/3ds/snes9x_3ds`, including per-game settings. Back that folder up first. A per-game config saved by an rcmz build keeps its saved depths and on/off state, so delete a game's config to see the new defaults.

## Step 1: does it work at all

| Check | Pass means |
|---|---|
| Boots, loads a ROM, returns to the menu | No crash or freeze |
| 3D slider at 0 | Looks identical to a flat image |
| Slider up and down during play | Depth changes with no flicker or stale frames |
| SMW title screen with the slider up, for 20 seconds or so | Hills, sky and sprites appear and stay. One Azahar capture was missing them, probably mid-fade |
| Pause menu and pause animation | Works, with no leftover image in either eye |
| 3D Depth tab | Previews appear, the list scrolls to the last row, X resets a slider |
| Options and Fast Scroll on the file list | Buttons appear and work |
| Save and load a state, take a screenshot | Both work with 3D on |
| Quit and relaunch | Settings persist, no crash on exit |

## Step 2: the three SMW reports

Take a photo or screenshot of each, since these settle disputes in the ledger.

| Scene | Question |
|---|---|
| SMW world map, top left | What exactly is the glitch: beside a sprite, beside the status bar, or at the screen edge? |
| SMW level | Do backgrounds sit behind the playfield? |
| SMW level | Does Mario stand on the ground, with no float? |

## Step 2b: the named artifacts

These decide `2026-11-01-definition-of-done.md`. Use the merged build at full 3D slider, with default settings and no saved per-game config. Close one eye at a time for the edges.

| Scene | Pass |
|---|---|
| SMW world map, top left, on two different maps | No green line above the lives counter |
| Super Metroid, a room that scrolls sideways | No strip or shimmer at either edge of either eye |
| Super Metroid, Crateria surface in the rain | No rolling black strip in either eye |
| Stunt Race FX, Speed Trax race, no saved config | No garbage at either side of the race picture, in either eye. Its profile sets `OBJ prio 3` and `BG1 prio 1` to 0 and `BG2 prio 1` to 5 |
| Same race, top of the screen, for a minute or so | No flashing bar. The maintainer saw one once in Azahar and it didn't come back, so photograph it if it shows |
| Same race, Modes 2-7: `OBJ prio 3` to 3 | Garbage should appear at the sides of the picture. Confirm it's the garbage you remember, then set it back to 0 |

If an edge fails, check that `Crop screen edges` is on in the 3D Depth tab, and photograph it.

**To see the green line on purpose,** on SMW's world map with the slider up, open the 3D Depth tab under `Modes 0-1`:
1. Set `OBJ prio 3` to 3, with `BG3 prio 1 front` at 0. A green bar should appear at the top left.
2. Set `BG3 prio 1 front` to 3 as well. It should go away.

Azahar showed exactly this on the same code plus a test shader patch. Set both back to 0 afterwards, or press X on each row to reset it.

**Menu notes from Azahar, unchecked on a console:** R switches tabs, and touch didn't. D-pad left and right adjusts a slider only on a slider row, and switches tabs on other rows. The cursor and tab stay where you left them between pauses.

## Step 3: games, including ones that should struggle

| Game | Why | Watch for |
|---|---|---|
| Super Metroid | Rain and HUD share BG3 | HUD readable, rain behind. Compare with rcmz's README preset |
| Donkey Kong Country | Parallax and colour math | Menu colours, background separation |
| F-Zero, Super Mario Kart | Mode 7 | Road recedes, karts don't look stuck on top |
| Yoshi's Island | Sub-screen colour math, SuperFX | Title logo, frame rate |
| Super Castlevania IV | BG3 priority, fog | Fog depth |
| A Link to the Past | Mixed planes on one layer | Expected to struggle. Record how badly |
| Street Fighter II | Everything in one layer | Expected to struggle. Fighters should not float |
| Any heavy HDMA game | Timing with the new frame order | Tearing, flicker |

## Step 4: performance

You only have a New 3DS XL, so Old 3DS stays unverified unless a tester reports. Compare FPS on the merged build against the baseline with the slider up, in Yoshi's Island and F-Zero. Upstream's deferred frame order is new under the per-eye draw, so look for dropped frames there first.

## Reporting back

For each failure, note the game, the scene, the slider position and a photo. Results go into the ledger.
