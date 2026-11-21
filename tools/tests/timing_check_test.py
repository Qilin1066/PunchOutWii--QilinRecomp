import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

MODULE = Path(__file__).resolve().parents[1] / "check" / "timing_check.py"
spec = importlib.util.spec_from_file_location("timing_check", MODULE)
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)


class TimingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.checks = {"logic_hz": 60, "intervals": [{"name": "cue to attack", "from": "cue", "to": "attack", "tolerance_frames": 0}]}

    def tearDown(self):
        self.temp.cleanup()

    def capture(self, name, data):
        path = self.root / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return timing.load_capture(path)

    def test_intervals_ignore_boot_offset(self):
        ref = self.capture("ref.json", {"fps": 60, "events": {"cue": {"frame": 100}, "attack": {"frame": 118}}})
        new = self.capture("new.json", {"fps": 60, "events": {"cue": {"frame": 500}, "attack": {"frame": 518}}})
        self.assertTrue(timing.compare(ref, new, self.checks)["passed"])
        new["events"]["attack"]["frame"] += 1
        report = timing.compare(ref, new, self.checks)
        self.assertFalse(report["passed"])
        self.assertEqual(report["intervals"][0]["delta_frames"], 1)

    def test_different_capture_rates(self):
        ref = self.capture("ref.json", {"fps": 60, "events": {"cue": {"frame": 10}, "attack": {"frame": 28}}})
        new = self.capture("new.json", {"fps": 120, "events": {"cue": {"frame": 20}, "attack": {"frame": 56}}})
        self.assertTrue(timing.compare(ref, new, self.checks)["passed"])

    def test_bad_inputs_fail(self):
        data = self.capture("x.json", {"events": {"cue": {"frame": 1}}})
        with self.assertRaises(ValueError):
            timing.compare(data, data, self.checks)
        with self.assertRaises(ValueError):
            self.capture("bad.json", {"fps": 0})
        with self.assertRaises(ValueError):
            self.capture("bad.json", {"fps": "NaN"})
        data["events"]["attack"] = {"frame": 0}
        with self.assertRaises(ValueError):
            timing.compare(data, data, self.checks)

    def test_video_edges_and_occurrence(self):
        from PIL import Image
        frames = self.root / "frames"
        frames.mkdir()
        for index in range(12):
            colour = (255, 0, 0) if index in (3, 4, 8, 9) else (0, 0, 0)
            Image.new("RGB", (4, 4), colour).save(frames / f"{index:04d}.png")
        data = self.capture("x.json", {"fps": 60, "frames": "frames"})
        probe = {"name": "cue", "source": "video", "roi": [0, 0, 4, 4], "rgb": [255, 0, 0], "occurrence": 2}
        self.assertEqual(timing.video_event(data, probe) * 60, 8)
        probe["roi"] = [0, 0, 5, 4]
        with self.assertRaises(ValueError):
            timing.video_event(data, probe)

    def test_missing_frame_index_rejected(self):
        from PIL import Image
        frames = self.root / "indexed"
        frames.mkdir()
        for index in range(3):
            Image.new("RGB", (2, 2), (255, 0, 0)).save(frames / f"{index}.png")
        (self.root / "frames.csv").write_text("file,vi_retrace\n0.png,10\n1.png,11\n2.png,13\n")
        with self.assertRaisesRegex(ValueError, "missing retraces"):
            self.capture("x.json", {"frames": "indexed", "frame_index": "frames.csv"})

    def test_audio_onset(self):
        with wave.open(str(self.root / "audio.wav"), "wb") as out:
            out.setparams((1, 2, 1000, 0, "NONE", "not compressed"))
            out.writeframes(struct.pack("<" + "h" * 100, *([0] * 40 + [16000] * 20 + [0] * 40)))
        data = self.capture("x.json", {"audio": "audio.wav"})
        result = timing.audio_event(data, {"name": "sound", "window_ms": 5, "rms": .1})
        self.assertEqual(result, timing.Fraction(40, 1000))

    def test_cli_missing_events_cannot_pass(self):
        for name in ("ref.json", "new.json"):
            self.capture(name, {"events": {}})
        checks = self.root / "checks.json"
        checks.write_text(json.dumps(self.checks))
        result = subprocess.run([sys.executable, str(MODULE), "--reference", str(self.root / "ref.json"), "--candidate",
                                 str(self.root / "new.json"), "--checks", str(checks), "--out", str(self.root / "report.json")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("missing event", result.stderr)
        self.assertFalse(json.loads((self.root / "report.json").read_text())["passed"])


if __name__ == "__main__":
    unittest.main()
