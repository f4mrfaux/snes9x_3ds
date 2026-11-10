#!/usr/bin/env python3
"""Tests for rank_scene_vars. Run: python -I tools/re/test_rank_scene_vars.py"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rank_scene_vars as r  # noqa: E402

SCENE = 0x100
TIMER = 0x13
SPRITE_X = 0x94
FADE_ONLY = 0x200
ONE_SHOT = 0x300
FADE = 0x400


def run_of_game():
    """Five scenes separated by cuts, with typical non-scene RAM churning."""
    events = [300, 700, 1100, 1500]
    scene_values = [0x01, 0x05, 0x01, 0x14, 0x05]
    history = {SCENE: [(e + 2, scene_values[i + 1]) for i, e in enumerate(events)]}
    history[TIMER] = [(f, f & 0xFF) for f in range(0, 1800)]
    history[SPRITE_X] = [(f, (f // 3) % 40) for f in range(0, 1800, 3)]
    history[ONE_SHOT] = [(5, 1), (1750, 2)]
    # Changes at one event, then again and again away from any other.
    history[FADE_ONLY] = [(301, 1)] + [(f, f % 7) for f in range(400, 650, 10)]
    return events, history


class Ranking(unittest.TestCase):
    def test_scene_variable_ranks_first(self):
        events, history = run_of_game()
        self.assertEqual(r.rank(history, events, 45)[0][0], SCENE)

    def test_counters_with_many_values_are_not_candidates(self):
        events, history = run_of_game()
        self.assertNotIn(TIMER, [a for a, _ in r.rank(history, events, 45)])

    def test_noise_scores_below_the_scene_variable(self):
        events, history = run_of_game()
        scores = dict(r.rank(history, events, 45))
        self.assertGreater(scores[SCENE]["score"], scores[SPRITE_X]["score"])
        self.assertGreater(scores[SCENE]["score"], scores[FADE_ONLY]["score"])

    def test_fade_register_ranks_below_scene_variable(self):
        # Changes at every event just as well, but only ever rests at one value:
        # it passes through the others on its way down to black and back.
        events, history = run_of_game()
        history[FADE] = []
        for e in events:
            history[FADE] += [(e + i, 15 - i) for i in range(16)]
            history[FADE] += [(e + 16 + i, i) for i in range(16)]
        ranked = [a for a, _ in r.rank(history, events, 45)]
        self.assertLess(ranked.index(SCENE), ranked.index(FADE))

    def test_counts_only_values_held_for_a_long_time(self):
        changes = [(100, 1), (101, 2), (300, 3), (305, 4), (500, 5)]
        self.assertEqual(r.long_held_values(changes, 600, 60), 3)

    def test_single_change_is_not_a_candidate(self):
        ranked = r.rank({SCENE: [(10, 1)]}, [10], 45)
        self.assertEqual(ranked, [])

    def test_no_events_scores_zero_without_crashing(self):
        ranked = r.rank({SCENE: [(10, 1), (20, 2)]}, [], 45)
        self.assertEqual(ranked[0][1]["score"], 0.0)


class Events(unittest.TestCase):
    def test_finds_fade_to_black_and_back(self):
        rows = [(f, 100.0, 0.05) for f in range(100)]
        rows += [(f, 1.0, 0.0) for f in range(100, 200)]
        rows += [(f, 100.0, 0.05) for f in range(200, 300)]
        self.assertEqual(r.find_scene_events(rows), [100, 200])

    def test_finds_a_hard_cut(self):
        rows = [(f, 100.0, 1.0 if f == 50 else 0.02) for f in range(100)]
        self.assertEqual(r.find_scene_events(rows), [50])

    def test_merges_events_that_are_close_together(self):
        rows = [(f, 100.0, 1.0 if f in (50, 55) else 0.02) for f in range(100)]
        self.assertEqual(r.find_scene_events(rows), [50])

    def test_ordinary_motion_is_not_an_event(self):
        rows = [(f, 100.0, 0.3) for f in range(100)]
        self.assertEqual(r.find_scene_events(rows), [])


def ppu_row(frame, mode=1, tm=0x17, bright=15, blank=0):
    row = {k: 0 for k in r.PPU_SIGNATURE}
    row.update(frame=frame, mode=mode, tm=tm, bright=bright, blank=blank)
    return row


class PpuEvents(unittest.TestCase):
    def test_finds_a_settled_change_dated_when_it_began(self):
        rows = [ppu_row(f) for f in range(100)] + [ppu_row(f, mode=7) for f in range(100, 200)]
        self.assertEqual(r.find_ppu_events(rows), [100])

    def test_first_setup_is_not_an_event(self):
        self.assertEqual(r.find_ppu_events([ppu_row(f) for f in range(100)]), [])

    def test_brief_register_flicker_is_ignored(self):
        rows = [ppu_row(f, tm=0x13 if 50 <= f < 55 else 0x17) for f in range(200)]
        self.assertEqual(r.find_ppu_events(rows), [])

    def test_a_fade_through_black_is_not_a_change_on_its_own(self):
        rows = [ppu_row(f) for f in range(100)]
        rows += [ppu_row(f, bright=0) for f in range(100, 150)]
        rows += [ppu_row(f) for f in range(150, 250)]
        self.assertEqual(r.find_ppu_events(rows), [])

    def test_blanked_frames_do_not_interrupt_a_new_setup(self):
        rows = [ppu_row(f) for f in range(100)]
        rows += [ppu_row(f, blank=1, mode=3) for f in range(100, 150)]
        rows += [ppu_row(f, mode=7) for f in range(150, 250)]
        self.assertEqual(r.find_ppu_events(rows), [150])

    def test_returning_to_an_earlier_setup_is_an_event(self):
        rows = [ppu_row(f) for f in range(100)] + [ppu_row(f, mode=7) for f in range(100, 200)]
        rows += [ppu_row(f) for f in range(200, 300)]
        self.assertEqual(r.find_ppu_events(rows), [100, 200])


class DeltaFile(unittest.TestCase):
    def write(self, frames, size=16, trim=0):
        data = struct.pack("<I", size) + bytes(size)
        for frame, changes in frames:
            data += struct.pack("<II", frame, len(changes))
            for address, value in changes:
                data += struct.pack("<I", address | value << 24)
        path = Path(self.tmp.name) / "wram.delta"
        path.write_bytes(data[:-trim] if trim else data)
        return path

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_round_trip(self):
        path = self.write([(0, [(3, 0xAB)]), (1, []), (2, [(3, 0xCD), (5, 0x01)])])
        initial, history, frames = r.read_delta(path)
        self.assertEqual(initial, bytes(16))
        self.assertEqual(frames, 3)
        self.assertEqual(history, {3: [(0, 0xAB), (2, 0xCD)], 5: [(2, 0x01)]})

    def test_address_above_16_bits_survives(self):
        path = self.write([(0, [(0x1FFFF, 0xFF)])])
        self.assertEqual(r.read_delta(path)[1], {0x1FFFF: [(0, 0xFF)]})

    def test_truncated_file_is_an_error(self):
        path = self.write([(0, [(3, 0xAB)])], trim=2)
        with self.assertRaises(ValueError):
            r.read_delta(path)


if __name__ == "__main__":
    unittest.main()
