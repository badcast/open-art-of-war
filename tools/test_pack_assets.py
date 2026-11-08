"""Regression checks for manifest/archive consistency (no compression required)."""
from pathlib import Path
import tempfile
import unittest

from pack_assets import check_manifest


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


if __name__ == "__main__":
    unittest.main()
