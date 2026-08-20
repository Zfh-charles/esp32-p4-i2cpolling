from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from dialogue_pack_contract import EMOTIONS, MOUTH_LEVELS, validate_pack


class DialoguePackContractTest(unittest.TestCase):
    def make_pack(self, root: Path) -> None:
        clips = {}
        stream = b"\xff\xd8test\xff\xd9"
        hold = bytes(480 * 480 * 2)
        for emotion in EMOTIONS:
            folder = root / emotion
            (folder / "mouth").mkdir(parents=True)
            (folder / "frames.mjpeg").write_bytes(stream)
            (folder / "hold_base.rgb565").write_bytes(hold)
            (folder / "hold_base.jpg").write_bytes(b"preview")
            levels = {}
            for level in MOUTH_LEVELS:
                (folder / "mouth" / f"{level}.rgb565").write_bytes(bytes(8 * 8 * 2))
                (folder / "mouth" / f"{level}.jpg").write_bytes(b"preview")
                levels[level] = {"frame": 0, "file": f"mouth/{level}.jpg",
                                 "rgb565": f"mouth/{level}.rgb565"}
            manifest = {
                "schema": "dialogue-emotion-clip-v2",
                "emotion": emotion,
                "source": {"file": "frames.mjpeg", "bytes": len(stream),
                           "sha256": hashlib.sha256(stream).hexdigest(),
                           "width": 480, "height": 480, "frame_count": 1},
                "render": {"mode": "layered", "fallback": "full_frame_clip",
                           "mouth_roi": {"x": 200, "y": 220, "width": 8, "height": 8},
                           "hold_base": {"file": "hold_base.jpg", "rgb565": "hold_base.rgb565",
                                         "width": 480, "height": 480},
                           "mouth_levels": levels,
                           "pose_bank": {"count": 1, "poses": [{"id": 0, "root": "."}]},
                           "life_layer": None},
                "frames": [{"frame": 0, "offset": 0, "size": len(stream),
                            "duration_ms": 160, "sha256": hashlib.sha256(stream).hexdigest()}],
            }
            (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            clips[emotion] = f"{emotion}/manifest.json"
        pack = {
            "schema": "dialogue-emotion-pack-v2", "version": 2,
            "runtime_contract": {
                "screen": {"width": 480, "height": 480, "pixel_format": "rgb565-le"},
                "max_rows_per_tick": 48, "max_layers_per_tick": 1,
            },
            "capabilities": ["full_frame_clip", "fixed_roi_clip", "layered", "static"],
            "legacy_fallback": {"enabled": True, "root": "/sdcard/mjpeg",
                                "files": [f"{e}.mjpeg" for e in EMOTIONS]},
            "clips": clips,
        }
        (root / "pack_manifest.json").write_text(json.dumps(pack), encoding="utf-8")

    def test_valid_minimal_pack(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            report = validate_pack(root)
            self.assertTrue(report["ok"], report["errors"])

    def test_rejects_path_escape(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            pack = json.loads((root / "pack_manifest.json").read_text())
            pack["clips"]["happy"] = "../outside.json"
            (root / "pack_manifest.json").write_text(json.dumps(pack))
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("escapes pack root" in e for e in report["errors"]))

    def test_rejects_out_of_bounds_roi(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            path = root / "happy" / "manifest.json"
            manifest = json.loads(path.read_text())
            manifest["render"]["mouth_roi"]["y"] = 479
            path.write_text(json.dumps(manifest))
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("ROI outside" in e for e in report["errors"]))

    def test_rejects_wrong_patch_size(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            (root / "happy" / "mouth" / "large.rgb565").write_bytes(b"bad")
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("RGB565 size" in e for e in report["errors"]))


if __name__ == "__main__":
    unittest.main()
