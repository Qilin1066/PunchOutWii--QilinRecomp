#!/usr/bin/env python3
"""Compare our renderer with Dolphin's on the same frames, pixel for pixel.

    tools/check/picture_check.py --game <build>/punchout --disc <extracted disc>
                                 --dolphin <dolphin-emu-nogui> --dolphin-user <dir>
                                 [--save <user folder>] [--out <dir>] [--slow] [--only name,...]
                                 [--colour console|true|both]

For each scene in picture_scenes.json (beside this script), the game runs
headless from a fresh save (or, for scenes marked "needs_save", a copy of
the saves in --save: a user folder with a profile in it, such as your own
qilin/punchout; it's only read), driven there by the scene's scripted input, and
captures one frame as a Dolphin FIFO log (PUNCHOUT_DFF). Both renderers then
draw that log from the same start (an empty EFB, the log's registers and
memory): ours with `punchout --render-dff`, Dolphin with its FIFO player, each
at the console's resolution with the console's copy filter, and the two
pictures are compared as they are, with no resampling. Each scene is compared
in the console's 18-bit colour and in true colour (Dolphin's "Force 24-Bit
Color", our `true_color`), or one of them (--colour).

A scene passes when the share of pixels more than 8 of 255 off in any channel
is within its `max_over8`, and the share more than 48 off within `max_over48`
(percent; the pictures are otherwise expected to be equal but for rounding in
filtering, which differs from one GPU to the next).

A frame that samples a texture the GPU copied from the picture in an earlier
frame is drawn by both from what RAM held there (the log has no such copy),
so it still compares, but neither picture is the game's: choose scenes drawn
afresh each frame where there is a choice.

The out folder gets each scene's log, both pictures, a difference picture,
and report.html with them side by side. The exit code is 0 when every scene
passes, 1 when one fails, 2 when one couldn't be captured or rendered.

Needs Python 3 with Pillow, the game built from your own disc, and a Dolphin
build with a user folder of its own, in whose Dolphin.ini and GFX.ini this
sets what the comparison needs (the Vulkan backend, frame dumps as images at
1x and as drawn, a frame for each copy to the screen, the console's copy
filter on, the colour depth). Dolphin itself is only run, never changed.
Nothing here is run by CI: it needs the disc.
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

from PIL import Image, ImageChops

HERE = Path(__file__).resolve().parent


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def capture(scene, args, out):
    """Run the game until it has written the scene's log."""
    dff = out / f"{scene['name']}.dff"
    prims = out / f"{scene['name']}.dff.prims"  # written after the log, so the log is whole by then
    for p in (dff, prims):
        p.unlink(missing_ok=True)
    save = Path(tempfile.mkdtemp(prefix="picture_check_save_"))
    if scene.get("needs_save"):
        shutil.copytree(Path(args.save) / "nand", save / "nand")
        ini = Path(args.save) / "punchout.ini"
        if ini.exists():
            shutil.copyfile(ini, save / "punchout.ini")
    env = dict(os.environ)
    env.update(scene.get("env", {}))
    env.update({"PUNCHOUT_SAVE_DIR": str(save), "PUNCHOUT_DFF": f"{dff}@{scene['seconds']}"})
    log = open(out / f"{scene['name']}.game.log", "w")
    proc = subprocess.Popen([args.game, "--headless", args.disc], env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + args.timeout
    try:
        while time.monotonic() < deadline:
            if dff.exists() and prims.exists() and prims.stat().st_size > 0:
                time.sleep(1)
                return dff, None
            if proc.poll() is not None:
                return None, f"the game stopped (exit {proc.returncode}) before capturing; see {log.name}"
            time.sleep(0.5)
        return None, f"no capture within {args.timeout} s; see {log.name}"
    finally:
        stop(proc)
        log.close()
        shutil.rmtree(save, ignore_errors=True)


def render_ours(dff, args, out, name, true_colour):
    """Our renderer's picture of the log, at the console's resolution."""
    ppm = out / f"{name}.ours.ppm"
    ppm.unlink(missing_ok=True)
    env = dict(os.environ)
    env.pop("PUNCHOUT_TRUE_COLOR", None)
    env.pop("PUNCHOUT_SCALE", None)
    if true_colour:
        env["PUNCHOUT_TRUE_COLOR"] = "1"
    log = open(out / f"{name}.ours.log", "w")
    try:
        code = subprocess.run([args.game, "--render-dff", str(dff), str(ppm), "1"], env=env, stdout=log,
                              stderr=subprocess.STDOUT, timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
        return None, f"our renderer took over {args.timeout} s; see {log.name}"
    finally:
        log.close()
    if code != 0 or not ppm.exists():
        return None, f"our renderer stopped (exit {code}); see {log.name}"
    return ppm, None


def set_ini(path, values):
    """Set keys in a Dolphin .ini file ({section: {key: value}}), keeping the rest of it."""
    lines = path.read_text().splitlines() if path.exists() else []
    for section, keys in values.items():
        start = next((i for i, l in enumerate(lines) if l.strip() == f"[{section}]"), None)
        if start is None:
            lines.append(f"[{section}]")
            start = len(lines) - 1
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        for key, value in keys.items():
            at = next((i for i in range(start + 1, end) if lines[i].split("=")[0].strip() == key), None)
            if at is None:
                lines.insert(end, f"{key} = {value}")
                end += 1
            else:
                lines[at] = f"{key} = {value}"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def render_dolphin(dff, args, out, name, true_colour):
    """Dolphin's picture of the log: its first frame dump."""
    user = Path(args.dolphin_user)
    # In the user folder's own files: this Dolphin doesn't take every one of them from its command line.
    set_ini(user / "Config" / "Dolphin.ini", {"Core": {"GFXBackend": "Vulkan"}, "Interface": {"UsePanicHandlers": "False"},
                                              "Movie": {"DumpFrames": "True", "DumpFramesSilent": "True"}})
    set_ini(user / "Config" / "GFX.ini", {
        "Settings": {"DumpFramesAsImages": "True", "InternalResolution": "1",
                     "FrameDumpsResolutionType": "2"},  # the picture as drawn, not as shown
        "Enhancements": {"ForceTrueColor": "True" if true_colour else "False",
                         "DisableCopyFilter": "False"},  # (Dolphin leaves the console's copy filter out by default)
        "Hacks": {"ImmediateXFBEnable": "True"}})  # a frame for each copy to the screen, not for each field of a made-up display mode
    frames = user / "Dump" / "Frames"
    if frames.exists():
        shutil.rmtree(frames)
    frames.mkdir(parents=True)
    first = frames / "framedump_1.png"
    log = open(out / f"{name}.dolphin.log", "w")
    proc = subprocess.Popen([args.dolphin, "-u", str(user), "-p", "headless", "-e", str(dff)], stdout=log, stderr=subprocess.STDOUT)
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
    """The shares of pixels (percent) equal, more than 8 off and more than 48 off in any channel."""
    ours = Image.open(ours_path).convert("RGB")
    theirs = Image.open(theirs_path).convert("RGB")
    if ours.size != theirs.size:
        return None, f"the pictures' sizes differ: ours {ours.size[0]}x{ours.size[1]}, Dolphin's {theirs.size[0]}x{theirs.size[1]}"
    ours.save(out / f"{name}.ours.png")
    diff = ImageChops.difference(ours, theirs)
    worst = ImageChops.lighter(ImageChops.lighter(diff.getchannel(0), diff.getchannel(1)), diff.getchannel(2))
    hist, n = worst.histogram(), ours.size[0] * ours.size[1]
    diff.point(lambda v: min(255, v * 16)).save(out / f"{name}.diff.png")
    return (hist[0] / n * 100, sum(hist[9:]) / n * 100, sum(hist[49:]) / n * 100), None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, help="the punchout executable")
    ap.add_argument("--disc", required=True, help="the extracted disc's folder (sys/, files/)")
    ap.add_argument("--dolphin", required=True, help="dolphin-emu-nogui")
    ap.add_argument("--dolphin-user", required=True, help="a user folder for Dolphin, used for nothing else (its settings are changed)")
    ap.add_argument("--out", default="picture-check", help="where the pictures and report go")
    ap.add_argument("--save", help="a user folder with a profile (its nand/ is copied for scenes that need one)")
    ap.add_argument("--scenes", default=str(HERE / "picture_scenes.json"))
    ap.add_argument("--only", help="comma-separated scene names")
    ap.add_argument("--slow", action="store_true", help="include scenes marked slow")
    ap.add_argument("--colour", choices=("console", "true", "both"), default="both",
                    help="the console's 18-bit colour, true colour, or each in turn")
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
    modes = {"console": [False], "true": [True], "both": [False, True]}[args.colour]
    results, worst_code = [], 0
    for s in scenes:
        if s.get("needs_save") and not (args.save and (Path(args.save) / "nand").is_dir()):
            print(f"{s['name']}: skipped (needs --save, a user folder with a profile)")
            continue
        print(f"{s['name']}: capturing at {s['seconds']} s of game time...", flush=True)
        dff, why = capture(s, args, out)
        if dff is None:
            results.append((s, s["name"], None, why)); worst_code = 2
            print(f"  {s['name']}: {why}")
            continue
        for true_colour in modes:
            name = f"{s['name']}.{'true' if true_colour else 'console'}"
            ours, why = render_ours(dff, args, out, name, true_colour)
            theirs, why2 = (None, None) if ours is None else render_dolphin(dff, args, out, name, true_colour)
            nums, why3 = (None, None) if theirs is None else compare(ours, theirs, out, name)
            if nums is None:
                why = why or why2 or why3
                results.append((s, name, None, why)); worst_code = 2
                print(f"  {name}: {why}")
                continue
            equal, over8, over48 = nums
            ok = over8 <= s["max_over8"] and over48 <= s["max_over48"]
            results.append((s, name, nums, None if ok else "over the limits"))
            if not ok:
                worst_code = max(worst_code, 1)
            print(f"  {name}: {'ok' if ok else 'FAIL'}: {equal:.2f}% of pixels equal, {over8:.3f}% more than 8 off "
                  f"(at most {s['max_over8']}), {over48:.3f}% more than 48 off (at most {s['max_over48']})", flush=True)
    rows = []
    for s, name, nums, why in results:
        n = html.escape(name)
        verdict = "ok" if why is None else html.escape(why)
        text = "" if nums is None else (f"{nums[0]:.2f}% equal, {nums[1]:.3f}% more than 8 off (≤ {s['max_over8']}), "
                                        f"{nums[2]:.3f}% more than 48 off (≤ {s['max_over48']})")
        imgs = "" if nums is None else (f'<img src="{n}.ours.png" title="ours"><img src="{n}.dolphin.png" title="Dolphin">'
                                        f'<img src="{n}.diff.png" title="difference x16">')
        rows.append(f"<h2>{n}: {verdict}</h2><p>{text}</p><p>{imgs}</p>")
    (out / "report.html").write_text("<!DOCTYPE html><meta charset=utf-8><title>Picture check</title>"
                                     "<style>body{font-family:sans-serif;background:#222;color:#eee}img{width:32%;margin:2px}</style>"
                                     "<h1>Ours, Dolphin's, and the difference (x16)</h1>" + "".join(rows))
    print(f"report: {out / 'report.html'}")
    return worst_code


if __name__ == "__main__":
    sys.exit(main())
