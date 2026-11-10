"""Tests for the pixel scorer in profile_pilot.py. Run: python -m unittest test_profile_pilot"""
import unittest

from profile_pilot import find_strips, foreign_pixels, score_eye

GROUND, HILL, TAN, GREEN = 0x204020, 0x60A060, 0xD0C0A0, 0x00FF00


def flat_scene():
    """A sky over ground, with a hill, 256 wide and 64 tall."""
    rows = []
    for y in range(64):
        row = [TAN] * 256 if y < 32 else [GROUND] * 256
        if y >= 32:
            for x in range(100, 140):
                row[x] = HILL
        rows.append(row)
    return rows


def shifted(rows, by, crop):
    w = len(rows[0])
    return [[row[min(max(x - by, 0), w - 1)] for x in range(crop, w - crop)] for row in rows]


class Scorer(unittest.TestCase):
    def test_rigid_shift_is_clean(self):
        self.assertEqual(score_eye(flat_scene(), shifted(flat_scene(), 5, 5), 5)[0], 0)

    def test_foreign_strip_is_flagged(self):
        eye = shifted(flat_scene(), 3, 3)
        for y in range(4, 28):
            eye[y][50] = GREEN
            eye[y][51] = GREEN
        pixels, strips, _ = score_eye(flat_scene(), eye, 3)
        self.assertEqual(pixels, 48)
        self.assertEqual(sorted(s[0] for s in strips), [50, 51])

    def test_short_mark_is_not_a_strip(self):
        eye = shifted(flat_scene(), 3, 3)
        for y in range(4, 9):
            eye[y][50] = GREEN
        self.assertEqual(score_eye(flat_scene(), eye, 3)[0], 0)

    def test_nearby_colour_is_ordinary_parallax(self):
        # Ground revealed beside the hill is a colour the flat picture has right there.
        eye = shifted(flat_scene(), 3, 3)
        for y in range(40, 60):
            eye[y][94] = GROUND
            eye[y][95] = HILL
        self.assertEqual(score_eye(flat_scene(), eye, 3)[0], 0)

    def test_colour_elsewhere_on_the_row_is_ordinary_parallax(self):
        flat = flat_scene()
        for y in range(40, 60):
            flat[y][200] = GREEN
        eye = shifted(flat, 3, 3)
        for y in range(40, 60):
            eye[y][50] = GREEN
        self.assertEqual(score_eye(flat, eye, 3)[0], 0)

    def test_strip_counts_only_where_a_sprite_was(self):
        flat = flat_scene()
        eye = shifted(flat, 3, 3)
        for y in range(4, 28):
            eye[y][50] = GREEN
        no_sprite = bytes(256 * 64)
        self.assertEqual(score_eye(flat, eye, 3, no_sprite)[:1], (0,))
        self.assertEqual(score_eye(flat, eye, 3, no_sprite)[2], 24)
        sprite = bytearray(256 * 64)
        for y in range(4, 28):
            sprite[y * 256 + 53] = 48
        self.assertEqual(score_eye(flat, eye, 3, bytes(sprite))[0], 24)

    def test_one_row_gaps_stay_in_one_strip(self):
        marks = [(10, y) for y in range(0, 12) if y != 5]
        self.assertEqual(find_strips(marks), [(10, 0, 11, 11)])

    def test_two_row_gap_splits_a_strip(self):
        marks = [(10, y) for y in list(range(0, 6)) + list(range(9, 20))]
        self.assertEqual(find_strips(marks), [(10, 9, 19, 11)])

    def test_cropped_eye_lines_up_with_flat(self):
        # Colour at screen x=60 only: found through the crop offset, so it isn't foreign.
        flat = [[TAN] * 256 for _ in range(10)]
        flat[5][60] = GREEN
        eye = [row[5:251] for row in flat]
        self.assertEqual(foreign_pixels(flat, eye, 0), [])


if __name__ == "__main__":
    unittest.main()
