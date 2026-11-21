"""Launcher integration: a failed disc change must keep the previous playable build."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class LauncherTest(unittest.TestCase):
    def test_disc_change_is_committed_only_after_success(self):
        with tempfile.TemporaryDirectory(prefix="punchout launcher ") as tmp:
            root = Path(tmp)
            shutil.copyfile(Path(__file__).resolve().parents[2] / "punchout.sh", root / "punchout.sh")
            tools = root / "tools"
            tools.mkdir()
            cmake = tools / "cmake"
            cmake.write_text(f"#!{sys.executable}\n" + '''import os, sys
from pathlib import Path
if os.environ.get("FAIL_CMAKE"):
    sys.exit(1)
if "--build" in sys.argv:
    exe = Path(sys.argv[2]) / "punchout"
    exe.write_text('#!/bin/sh\\nprintf "played:%s\\\\n" "$1"\\n')
    exe.chmod(0o755)
''')
            cmake.chmod(0o755)
            dpkg = tools / "dpkg"
            dpkg.write_text("#!/bin/sh\nexit 0\n")
            dpkg.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"], CC="gcc-14", CXX="g++-14")
            discs = [root / "old disc", root / "new disc"]
            for disc in discs:
                (disc / "sys").mkdir(parents=True)
                (disc / "files").mkdir()
                (disc / "sys" / "main.dol").touch()
                (disc / "sys" / "fst.bin").touch()
            def run(*args, fail=False):
                return subprocess.run(["sh", str(root / "punchout.sh"), *map(str, args)],
                                      env=dict(env, FAIL_CMAKE="1") if fail else env,
                                      capture_output=True, text=True, timeout=15)
            result = run("build", "--disc", discs[0])
            self.assertEqual(result.returncode, 0, result.stderr)
            record = root / "build-game" / "disc-folder.txt"
            self.assertEqual(record.read_text(), str(discs[0]))
            result = run("build", "--disc", discs[1], fail=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(record.read_text(), str(discs[0]))
            result = run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("played:" + str(discs[0]), result.stdout)
            result = run("build", "--disc", discs[1])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(record.read_text(), str(discs[1]))
            shutil.rmtree(discs[1] / "files")
            result = run()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no files folder", result.stderr)


if __name__ == "__main__":
    unittest.main()
