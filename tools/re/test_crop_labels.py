"""Tests for the classifier in crop_labels.py. Run: python test_crop_labels.py"""
import unittest

from crop_labels import fit, occlusion_features, predict

GREEN, RED, MAGENTA = 0x00AA00, 0xFF0000, 0xFF00FF
BG, OBJ3, PLUG, BACKDROP = 42, 48, 35, 1


def scene(eye_cols):
    """10 rows, 16 wide. Flat: a BG everywhere, a priority-3 sprite on columns 6-7.
    eye_cols maps eye columns 6-7 to (colour, owner); the sprite moves to columns 8-9 in the eye."""
    flat = [[RED if x in (6, 7) else GREEN for x in range(16)] for _ in range(10)]
    flat_z = bytes(sum(([OBJ3 if x in (6, 7) else BG for x in range(16)] + [0] * 240 for _ in range(10)), []))
    eye, eye_z = [], []
    for _ in range(10):
        cols = [(RED, OBJ3) if x in (8, 9) else eye_cols if x in (6, 7) else (GREEN, BG) for x in range(16)]
        eye.append([c for c, _ in cols])
        eye_z += [z for _, z in cols] + [0] * 240
    return flat, eye, flat_z, bytes(eye_z)


class Occlusion(unittest.TestCase):
    def test_layer_hidden_under_a_sprite(self):
        f = occlusion_features(*scene((MAGENTA, PLUG))[:2], 2, *scene((MAGENTA, PLUG))[2:])
        self.assertEqual((f["occ_hidden"], f["occ_hidden_by_sprite"], f["occ_layer_absent_row"]), (1, 1, 1))

    def test_backdrop_showing_through(self):
        flat, eye, fz, ez = scene((MAGENTA, BACKDROP))
        f = occlusion_features(flat, eye, 2, fz, ez)
        self.assertEqual((f["occ_backdrop"], f["occ_hidden"], f["occ_backdrop_absent_row"]), (1, 0, 1))

    def test_same_layer_in_a_new_colour(self):
        flat, eye, fz, ez = scene((MAGENTA, BG))
        f = occlusion_features(flat, eye, 2, fz, ez)
        self.assertEqual((f["occ_same_layer"], f["occ_hidden"]), (1, 0))


def row(label, colours, contrast):
    return {"label": label, "features": {"colours_per_pixel": colours, "contrast": contrast}}


class Classifier(unittest.TestCase):
    def test_separates_noise_from_smooth_strips(self):
        rows = [row("junk", 0.8 + i / 100, 0.5) for i in range(10)] + [row("fine", 0.05 + i / 100, 0.1) for i in range(10)]
        model = fit(rows, ["colours_per_pixel", "contrast"])
        self.assertGreater(predict(model, {"colours_per_pixel": 0.85, "contrast": 0.5}), 0.5)
        self.assertLess(predict(model, {"colours_per_pixel": 0.07, "contrast": 0.1}), 0.5)

    def test_constant_feature_does_not_divide_by_zero(self):
        rows = [row("junk", 1.0, 0.3), row("fine", 0.1, 0.3)]
        model = fit(rows, ["colours_per_pixel", "contrast"], steps=50)
        self.assertEqual(model["spread"][1], 1)


if __name__ == "__main__":
    unittest.main()
