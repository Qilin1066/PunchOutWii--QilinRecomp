#!/usr/bin/env python3
"""Compare our pictures of a few scenes with Dolphin's render of the same frames.

    tools/check/picture_check.py --game <build>/punchout --disc <extracted disc>
                                 --dolphin <dolphin-emu-nogui> --dolphin-user <dir>
                                 [--save <user folder>] [--out <dir>] [--slow] [--only name,...]

For each scene in picture_scenes.json (beside this script), the game runs
offscreen from a fresh save (or, for scenes marked "needs_save", a copy of
the saves in --save: a user folder with a profile in it, such as your own
qilin/punchout; it's only read), driven there by the scene's scripted input, and
captures one frame (PUNCHOUT_DFF): a Dolphin FIFO log of it, and our own
picture of it. Dolphin's FIFO player then renders the log, and the two
pictures are compared at Dolphin's size (856x480): the root-mean-square
difference, and the share of pixels more than 48 of 255 off in any channel.
Both must stay within the scene's limits.

A frame that draws a texture the GPU copied from the picture in an earlier
frame can't be compared: the log has the RAM as the frame starts, and our
copies stay on the GPU, so Dolphin draws whatever RAM held there. Choose
scenes drawn afresh each frame (the menus are; a dialog over a frozen
backdrop isn't).

The out folder gets each scene's log, both pictures, a difference picture,
and report.html with them side by side. The exit code is 0 when every scene
passes, 1 when one fails, 2 when one couldn't be captured or rendered.

Needs Python 3 with Pillow, the game built from your own disc, and a Dolphin
build with a user folder set up for it: Vulkan, frame dumping on as images
(Dolphin.ini [Movie] DumpFrames, DumpFramesSilent; GFX.ini [Settings]
DumpFramesAsImages, InternalResolution = 1). Dolphin is only run, never
changed. Nothing here is run by CI: it needs the disc.
"""
import argparse
import html
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

HERE = Path(__file__).resolve().parent
SIZE = (856, 480)  # Dolphin's frame dumps of the 16:9 picture


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def capture(scene, args, out):
    """Run the game until it has written the scene's log and our picture."""
    dff = out / f"{scene['name']}.dff"
    ppm = out / f"{scene['name']}.dff.ppm"  # the game names it after the log
    for p in (dff, ppm):
        p.unlink(missing_ok=True)
    save = Path(tempfile.mkdtemp(prefix="picture_check_save_"))
    if scene.get("needs_save"):
        shutil.copytree(Path(args.save) / "nand", save / "nand")
        ini = Path(args.save) / "punchout.ini"
        if ini.exists():
            shutil.copyfile(ini, save / "punchout.ini")
    env = dict(os.environ)
    env.update(scene.get("env", {}))
    env.update({"PUNCHOUT_SAVE_DIR": str(save), "PUNCHOUT_DFF": f"{dff}@{scene['seconds']}", "SDL_AUDIO_DRIVER": "dummy"})
    log = open(out / f"{scene['name']}.game.log", "w")
    proc = subprocess.Popen([args.game, "--offscreen", args.disc], env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + args.timeout
    try:
        while time.monotonic() < deadline:
            if ppm.exists() and ppm.stat().st_size > 0 and dff.exists():
                time.sleep(1)  # the picture is written whole by then
                return dff, ppm
            if proc.poll() is not None:
                return None, f"the game stopped (exit {proc.returncode}) before capturing; see {log.name}"
            time.sleep(0.5)
        return None, f"no capture within {args.timeout} s; see {log.name}"
    finally:
        stop(proc)
        log.close()
        shutil.rmtree(save, ignore_errors=True)


def render(dff, args, out, name):
    """Dolphin's picture of the log: its first frame dump."""
    frames = Path(args.dolphin_user) / "Dump" / "Frames"
    if frames.exists():
        shutil.rmtree(frames)
    frames.mkdir(parents=True)
    first = frames / "framedump_1.png"
    log = open(out / f"{name}.dolphin.log", "w")
    proc = subprocess.Popen([args.dolphin, "-u", args.dolphin_user, "-p", "headless",
                             "-C", "Dolphin.Interface.UsePanicHandlers=False", "-e", str(dff)],
                            stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + args.timeout
    try:
        while time.monotonic() < deadline:
            if first.exists() and (frames / "framedump_2.png").exists():  # the first is written whole
                png = out / f"{name}.dolphin.png"
                shutil.copyfile(first, png)
                return png, None
            if proc.poll() is not None:
                return None, f"Dolphin stopped (exit {proc.returncode}) without a frame; see {log.name}"
            time.sleep(0.5)
        return None, f"no frame from Dolphin within {args.timeout} s; see {log.name}"
    finally:
        stop(proc)
        log.close()


def compare(ours_path, theirs_path, out, name):
    ours = Image.open(ours_path).convert("RGB").resize(SIZE, Image.BILINEAR)
    theirs = Image.open(theirs_path).convert("RGB").resize(SIZE, Image.BILINEAR)
    ours.save(out / f"{name}.ours.png")
    diff = ImageChops.difference(ours, theirs)
    stat = ImageStat.Stat(diff)
    rmse = (sum(v * v for v in stat.rms) / 3) ** 0.5 / 255 * 100
    # A pixel is off when any channel is: the channels' maximum.
    off_mask = ImageChops.lighter(ImageChops.lighter(diff.getchannel(0), diff.getchannel(1)), diff.getchannel(2)).point(
        lambda v: 255 if v > 48 else 0)
    off = off_mask.histogram()[255] / (SIZE[0] * SIZE[1]) * 100
    diff.point(lambda v: min(255, v * 4)).save(out / f"{name}.diff.png")
    return rmse, off


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, help="the punchout executable")
    ap.add_argument("--disc", required=True, help="the extracted disc's folder (sys/, files/)")
    ap.add_argument("--dolphin", required=True, help="dolphin-emu-nogui")
    ap.add_argument("--dolphin-user", required=True, help="Dolphin's user folder (frame dumping on)")
    ap.add_argument("--out", default="picture-check", help="where the pictures and report go")
    ap.add_argument("--save", help="a user folder with a profile (its nand/ is copied for scenes that need one)")
    ap.add_argument("--scenes", default=str(HERE / "picture_scenes.json"))
    ap.add_argument("--only", help="comma-separated scene names")
    ap.add_argument("--slow", action="store_true", help="include scenes marked slow")
    ap.add_argument("--timeout", type=int, default=900, help="seconds for each capture and each render")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    scenes = json.loads(Path(args.scenes).read_text())["scenes"]
    if args.only:
        wanted = set(args.only.split(","))
        scenes = [s for s in scenes if s["name"] in wanted]
    elif not args.slow:
        scenes = [s for s in scenes if not s.get("slow")]
    results, worst_code = [], 0
    for s in scenes:
        name = s["name"]
        if s.get("needs_save") and not (args.save and (Path(args.save) / "nand").is_dir()):
            print(f"{name}: skipped (needs --save, a user folder with a profile)")
            continue
        print(f"{name}: capturing at {s['seconds']} s of game time...", flush=True)
        dff, ours = capture(s, args, out)
        if dff is None:
            results.append((s, None, None, ours)); worst_code = 2
            print(f"  {name}: {ours}")
            continue
        theirs, why = render(dff, args, out, name)
        if theirs is None:
            results.append((s, None, None, why)); worst_code = 2
            print(f"  {name}: {why}")
            continue
        rmse, off = compare(ours, theirs, out, name)
        ok = rmse <= s["max_rmse"] and off <= s["max_off"]
        results.append((s, rmse, off, None if ok else "over the limits"))
        if not ok:
            worst_code = max(worst_code, 1)
        print(f"  {name}: {'ok' if ok else 'FAIL'}: RMSE {rmse:.2f}% (at most {s['max_rmse']}), "
              f"{off:.2f}% of pixels off (at most {s['max_off']})", flush=True)
    rows = []
    for s, rmse, off, why in results:
        n = html.escape(s["name"])
        verdict = "ok" if why is None else html.escape(why)
        nums = "" if rmse is None else f"RMSE {rmse:.2f}% (≤ {s['max_rmse']}), off {off:.2f}% (≤ {s['max_off']})"
        imgs = "" if rmse is None else (f'<img src="{n}.ours.png" title="ours"><img src="{n}.dolphin.png" title="Dolphin">'
                                        f'<img src="{n}.diff.png" title="difference x4">')
        rows.append(f"<h2>{n}: {verdict}</h2><p>{nums}</p><p>{imgs}</p>")
    (out / "report.html").write_text("<!DOCTYPE html><meta charset=utf-8><title>Picture check</title>"
                                     "<style>body{font-family:sans-serif;background:#222;color:#eee}img{width:32%;margin:2px}</style>"
                                     "<h1>Ours, Dolphin's, and the difference (x4)</h1>" + "".join(rows))
    print(f"report: {out / 'report.html'}")
    return worst_code


if __name__ == "__main__":
    sys.exit(main())
