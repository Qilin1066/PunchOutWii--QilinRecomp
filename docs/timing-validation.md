# Fight timing validation

`tools/check/timing_check.py` compares **intervals**, not boot times, between
matched Dolphin and recomp captures. It reports differences in game frames
and milliseconds, exits 1 on a mismatch and 2 for missing/invalid evidence.
Missing cues never produce a passing result. The tool and its detectors have
synthetic regression tests; **no opponent's timings have been certified by
this change**, because the disc and matched recordings were not available.

## Capture conditions

Use the same game revision, save, opponent, round, difficulty, input sequence
and camera state. Record unpaused runs. Keep simulation at its normal speed.
Use a video stream with a known presentation rate and no missing frames, and
uncompressed 16-bit PCM WAV for audio probes. Both videos need matching
geometry when using the same pixel ROI. A capture at 120 Hz has fps 120; do
not call it 60 just because the game itself runs at 60 Hz.

The existing scripted `PUNCHOUT_INPUT`, `PUNCHOUT_INPUT2` and pointer scripts
help reproduce a scene in the port. The reference needs equivalent inputs
in Dolphin. These scripts schedule by game progress; they do not establish
which guest sample first observed the input. Reaction/counter **window**
measurements require controlled input sweeps and outcome annotations, as
well as matching visual cues. A FIFO picture replay tests rendering, not
fight timing, and must not serve as a timing reference.

An offscreen port capture (`PUNCHOUT_DUMP`, `PUNCHOUT_DUMP_EVERY=1`) now writes
`frames.csv`, with the VI presentation sequence for each image. Specify this
as `frame_index` in the manifest. The tool refuses an indexed video with
missing retraces. Capture on hardware fast enough to retain every frame;
dumping readbacks and images itself costs time. The index describes virtual
presentation, not physical input-to-display latency. External recordings
without an index must preserve the full frame cadence themselves.

## Annotated events

Create one capture manifest next to each recording. Example numbers below
illustrate the file format only; they are **not a Punch-Out baseline**:

```json
{
  "fps": 60,
  "frames": "frames",
  "frame_index": "frames.csv",
  "audio": "sound.wav",
  "events": {
    "cue": {"frame": 120},
    "attack": {"frame": 138},
    "dodge_window_open": {"frame": 129},
    "dodge_window_close": {"frame": 137}
  }
}
```

`frame` is a zero-based video frame, or a VI sequence when `frame_index` is
used. Audio/manual annotations can instead use `{"seconds": 2.05}`. Only one
unit is accepted per event. Manual annotations override detectors, so record
how each annotation was established alongside its capture.

An interval specification can then test both cues and windows:

```json
{
  "logic_hz": 60,
  "intervals": [
    {"name": "cue to attack", "from": "cue", "to": "attack", "tolerance_frames": 0},
    {"name": "dodge window", "from": "dodge_window_open", "to": "dodge_window_close", "tolerance_frames": 0}
  ]
}
```

The reference and candidate may start at different times; their intervals
are compared independently. Tolerance is explicit, in game frames. Choose it
for the precision of the evidence (audio window detectors, for example,
quantise onsets by their window size). Do not widen it just to make a failure
pass. A non-zero delta still appears in the report when it is within tolerance.

```sh
python3 tools/check/timing_check.py --reference dolphin/capture.json \
  --candidate recomp/capture.json --checks intervals.json --out timing-report.json
```

## Automatic cue detection

Instead of annotating a cue manually, add it to the checks file's `events`
array. Install Pillow for video probes. Calibrate the probes on both original
and port recordings before treating a result as evidence.

```json
{
  "events": [
    {"name": "cue", "source": "video", "roi": [100, 80, 16, 16],
     "rgb": [255, 255, 255], "max_channel_error": 16, "min_fraction": 0.75,
     "search_seconds": [5, 10], "occurrence": 1},
    {"name": "sound_cue", "source": "audio", "rms": 0.25, "window_ms": 5,
     "search_seconds": [5, 10], "occurrence": 1}
  ],
  "intervals": [
    {"name": "visual/audio alignment", "from": "cue", "to": "sound_cue", "tolerance_frames": 0.5}
  ]
}
```

The video probe finds a rising edge where enough pixels in its rectangular
ROI match the chosen colour. The audio probe finds a rising RMS threshold
crossing across all channels. Both support selecting the nth occurrence and
a bounded search window. An audio threshold alone cannot identify a cue
hidden by music; use an isolated signal or a verified manual annotation in
that case. A missed cue, malformed ROI, absent file or reversed interval
fails explicitly. Store recordings and game-derived annotations locally,
not in the source repository.
