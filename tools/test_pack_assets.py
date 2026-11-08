"""Regression checks for manifest/archive consistency (no compression required)."""
import json
from pathlib import Path
import tempfile
import unittest

from pack_assets import check_manifest


class ManifestTests(unittest.TestCase):
    def test_manifest_requires_archived_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.json").write_text(json.dumps({"map": "map.json"}))
            (root / "map.json").write_text("{}")
            self.assertEqual(check_manifest(directory, ["main.json", "map.json"]), 1)
            # Existence on disk does not guarantee inclusion in the blob.
            with self.assertRaises(SystemExit):
                check_manifest(directory, ["main.json"])

    def test_manifest_cannot_reference_external_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            (root / "outside.json").write_text("{}")
            for name in ("../outside.json", str(root / "outside.json")):
                (assets / "main.json").write_text(json.dumps({"map": name}))
                with self.assertRaises(SystemExit):
                    check_manifest(str(assets), ["main.json"])


if __name__ == "__main__":
    unittest.main()
