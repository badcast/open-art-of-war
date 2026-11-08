"""Regression checks for manifest/archive consistency (no compression required)."""
from pathlib import Path
import tempfile
import unittest

from pack_assets import check_manifest, check_mod, mod_key


class ManifestTests(unittest.TestCase):
    def test_manifest_requires_archived_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.yaml").write_text("map: map.json\n")
            (root / "map.json").write_text("{}")
            self.assertEqual(check_manifest(directory, ["main.yaml", "map.json"]), 1)
            # Existence on disk does not guarantee inclusion in the blob.
            with self.assertRaises(SystemExit):
                check_manifest(directory, ["main.yaml"])

    def test_manifest_cannot_reference_external_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            (root / "outside.json").write_text("{}")
            for name in ("../outside.json", str(root / "outside.json")):
                (assets / "main.yaml").write_text(f"map: '{name}'\n")
                with self.assertRaises(SystemExit):
                    check_manifest(str(assets), ["main.yaml"])


class ModTests(unittest.TestCase):
    def test_mod_needs_id_and_names_its_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "mod.yaml").write_text("id: tanks\nversion: '1.0'\nmanifest:\n  maps: ['maps/new.json']\n")
            self.assertEqual(check_mod(directory, ["maps/new.json"])["id"], "tanks")
            with self.assertRaises(SystemExit):
                check_mod(directory, [])  # the map is neither in the mod nor in the game
            (root / "mod.yaml").write_text("id: Bad Id\nversion: 1\n")
            with self.assertRaises(SystemExit):
                check_mod(directory, [])

    def test_mod_paths_stay_inside(self):
        self.assertEqual(mod_key("assets/sprites/x.png"), "sprites/x.png")
        self.assertEqual(mod_key("./data/../data/y.yaml"), "data/y.yaml")
        for bad in ("../x", "/etc/passwd", "C:/x", "a\\b"):
            self.assertIsNone(mod_key(bad))


if __name__ == "__main__":
    unittest.main()
