#!/usr/bin/env python3
"""Compare cue/attack/counter intervals from matched Dolphin and recomp captures.

Capture JSON: {"fps":60,"frames":"frames","audio":"sound.wav","events":{...}}.
Events may be annotated as {"frame":123} or {"seconds":2.05}; otherwise the
checks file defines a video colour probe or an audio RMS onset detector.
See docs/timing-validation.md. No game events or timing claims are invented.
"""
import argparse
import csv
from fractions import Fraction
import json
import math
from pathlib import Path
import struct
import sys
import wave


def number(value, label, minimum=0):
    if isinstance(value, bool):
        raise ValueError(f"{label} must be a number")
    try:
        result = Fraction(str(value))
    except (ValueError, ZeroDivisionError) as error:
        raise ValueError(f"invalid {label}: {value}") from error
    if result < minimum:
        raise ValueError(f"{label} must be at least {minimum}")
    return result


def load_capture(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    fps = number(data.get("fps", 60), "fps")
    if fps <= 0:
        raise ValueError("fps must be positive")
    data["_fps"], data["_base"] = fps, Path(path).resolve().parent
    if "frame_index" in data:
        video_frames(data)
    return data


def annotated(capture, name):
    event = capture.get("events", {}).get(name)
    if event is None:
        return None
    if not isinstance(event, dict) or ("frame" in event) == ("seconds" in event):
        raise ValueError(f"{name}: annotate exactly one of frame or seconds")
    return number(event["seconds"], name) if "seconds" in event else number(event["frame"], name) / capture["_fps"]


def occurrence(spec):
    value = spec.get("occurrence", 1)
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise ValueError("occurrence must be a positive integer")
    return value


def search_window(spec):
    start, end = spec.get("search_seconds", [0, 3600])
    start, end = number(start, "search start"), number(end, "search end")
    if end <= start:
        raise ValueError("search end must follow its start")
    return start, end


def video_frames(capture):
    if "_video" in capture:
        return capture["_video"]
    folder = capture["_base"] / capture["frames"]
    # Names must sort in presentation order (Dolphin framedump names are padded).
    frames = sorted(p for p in folder.iterdir() if p.suffix.lower() in (".png", ".ppm", ".bmp"))
    if not frames:
        raise ValueError(f"no frames in {folder}")
    frame_numbers = list(range(len(frames)))
    if "frame_index" in capture:
        with (capture["_base"] / capture["frame_index"]).open(newline="", encoding="utf-8") as index:
            rows = list(csv.DictReader(index))
        by_file = {row["file"]: int(row["vi_retrace"]) for row in rows}
        if len(by_file) != len(rows) or len(by_file) != len(frames):
            raise ValueError("capture index does not match its frames")
        frame_numbers = [by_file[frame.name] for frame in frames]
        if any(b != a + 1 for a, b in zip(frame_numbers, frame_numbers[1:])):
            raise ValueError("capture has missing retraces; frame-accurate timing cannot be established")
    if any(value < 0 for value in frame_numbers):
        raise ValueError("negative capture frame index")
    capture["_video"] = frames, frame_numbers
    return capture["_video"]


def video_event(capture, spec):
    from PIL import Image
    frames, frame_numbers = video_frames(capture)
    roi = spec["roi"]
    if len(roi) != 4 or any(isinstance(v, bool) or not isinstance(v, int) for v in roi):
        raise ValueError("roi must be four integers: x, y, width, height")
    x, y, width, height = roi
    if min(x, y) < 0 or min(width, height) <= 0:
        raise ValueError("invalid roi bounds")
    rgb = spec["rgb"]
    if len(rgb) != 3 or any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 255 for v in rgb):
        raise ValueError("rgb must have three values from 0 to 255")
    distance = number(spec.get("max_channel_error", 16), "colour tolerance")
    fraction = number(spec.get("min_fraction", .5), "pixel fraction")
    if fraction <= 0 or fraction > 1 or distance > 255:
        raise ValueError("pixel fraction or colour tolerance out of range")
    start, end = search_window(spec)
    found, was = 0, False
    from bisect import bisect_left
    first = max(0, bisect_left(frame_numbers, start * capture["_fps"]) - 1)
    last = min(len(frames), bisect_left(frame_numbers, end * capture["_fps"]))
    for index in range(first, last):
        with Image.open(frames[index]) as image:
            if x + width > image.width or y + height > image.height:
                raise ValueError(f"roi outside frame {frames[index]}")
            crop = image.convert("RGB").crop((x, y, x + width, y + height))
            pixels = crop.load()
            tolerance = float(distance)
            matching = sum(max(abs(pixels[px, py][c] - rgb[c]) for c in range(3)) <= tolerance
                           for py in range(height) for px in range(width))
            on = matching >= fraction * width * height
        at = Fraction(frame_numbers[index], 1) / capture["_fps"]
        if at >= start and on and not was:
            found += 1
            if found == occurrence(spec):
                return at
        was = on
    raise ValueError(f"video event {spec['name']} not found")


def audio_event(capture, spec):
    threshold = number(spec.get("rms", .05), "audio threshold")
    if threshold <= 0 or threshold > 1:
        raise ValueError("audio threshold exceeds full scale")
    start, end = search_window(spec)
    with wave.open(str(capture["_base"] / capture["audio"]), "rb") as source:
        if source.getsampwidth() != 2 or source.getcomptype() != "NONE":
            raise ValueError("audio must be uncompressed 16-bit PCM WAV")
        rate, channels = source.getframerate(), source.getnchannels()
        window_ms = number(spec.get("window_ms", 5), "audio window")
        if window_ms <= 0 or window_ms > 1000:
            raise ValueError("audio window must be greater than zero and at most 1000 ms")
        window = max(1, round(window_ms * rate / 1000))
        begin = max(0, math.floor(start * rate) - window)
        stop = min(source.getnframes(), math.ceil(end * rate))
        source.setpos(min(begin, source.getnframes()))
        found, was = 0, False
        for sample in range(begin, stop, window):
            raw = source.readframes(min(window, stop - sample))
            if not raw:
                break
            values = struct.unpack("<" + "h" * (len(raw) // 2), raw)
            # All channels contribute; detectors must be calibrated on the actual mix.
            rms = math.sqrt(sum(value * value for value in values) / len(values)) / 32768
            on = rms >= float(threshold)
            at = Fraction(sample, rate)
            if at >= start and on and not was:
                found += 1
                if found == occurrence(spec):
                    return at
            was = on
    raise ValueError(f"audio event {spec['name']} not found")


def event_time(capture, name, detectors):
    manual = annotated(capture, name)
    if manual is not None:
        return manual
    if name not in detectors:
        raise ValueError(f"missing event {name}; provide an annotation or detector")
    spec = detectors[name]
    if spec.get("source") == "video":
        return video_event(capture, spec)
    if spec.get("source") == "audio":
        return audio_event(capture, spec)
    raise ValueError(f"unknown detector source for {name}")


def compare(reference, candidate, config):
    hz = number(config.get("logic_hz", 60), "logic_hz")
    if hz <= 0:
        raise ValueError("logic_hz must be positive")
    specs = config.get("events", [])
    detectors = {spec["name"]: spec for spec in specs}
    if len(detectors) != len(specs):
        raise ValueError("duplicate event detectors")
    intervals = config.get("intervals", [])
    if not intervals:
        raise ValueError("at least one interval is required")
    names = {interval[key] for interval in intervals for key in ("from", "to")}
    times = [{name: event_time(capture, name, detectors) for name in names} for capture in (reference, candidate)]
    results = []
    for interval in intervals:
        durations = [timing[interval["to"]] - timing[interval["from"]] for timing in times]
        if min(durations) < 0:
            raise ValueError(f"reversed interval {interval['from']} -> {interval['to']}")
        tolerance = number(interval.get("tolerance_frames", 0), "frame tolerance")
        delta = (durations[1] - durations[0]) * hz
        results.append({"name": interval.get("name", interval["from"] + " -> " + interval["to"]),
                        "reference_frames": float(durations[0] * hz), "candidate_frames": float(durations[1] * hz),
                        "delta_frames": float(delta), "delta_ms": float(delta * 1000 / hz),
                        "tolerance_frames": float(tolerance), "passed": abs(delta) <= tolerance})
    return {"passed": all(item["passed"] for item in results), "logic_hz": float(hz), "intervals": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--checks", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        report = compare(load_capture(args.reference), load_capture(args.candidate), json.loads(args.checks.read_text(encoding="utf-8")))
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            temporary = args.out.with_name(args.out.name + ".tmp")
            temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            temporary.replace(args.out)
        for item in report["intervals"]:
            print(f"{'PASS' if item['passed'] else 'FAIL'} {item['name']}: {item['delta_frames']:+.3f} frames ({item['delta_ms']:+.3f} ms)")
        return 0 if report["passed"] else 1
    except (ValueError, KeyError, TypeError, OSError, ImportError, wave.Error) as error:
        if args.out:
            try:
                args.out.parent.mkdir(parents=True, exist_ok=True)
                args.out.write_text(json.dumps({"passed": False, "error": str(error), "intervals": []}, indent=2) + "\n", encoding="utf-8")
            except OSError as write_error:
                print(f"timing report could not be written: {write_error}", file=sys.stderr)
        print(f"timing check: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
