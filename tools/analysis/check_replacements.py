#!/usr/bin/env python3
"""Find calls from lifted code that would stop the game: into replaced
functions with no replacement, and into dropped code.

A function in a "replace" range of the library plan runs only if punchout/src
provides it (a {0x8xxxxxxx, "name", fn} entry); otherwise the call stops the
game with an error naming it. This lists every such function that lifted code
calls directly, with its callers, so they can be written before the game gets
there. Calls through function pointers aren't in the call graph; with --dol,
the pointers themselves are checked: every function whose address lifted code
builds (lis with addi or ori), and every function a data word points at, in
libraries other than those given with --internal (libraries whose tables only
their own replaced code reads). Lifted code calling dropped code (which isn't translated at all) is
reported too: such a function needs a replace range in the plan
(identify_libraries.py's overrides) and a replacement.

  qilin cfg <main.dol> --json cfg.json
  tools/analysis/check_replacements.py cfg.json [--plan data/R7PP01/plan.tsv]

The narrowest plan range holding an address decides it, as in qilin lift.
Exit status 1 if any are missing.
"""
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cfg", help="qilin cfg --json output")
    ap.add_argument("--plan", default=str(ROOT / "data/R7PP01/plan.tsv"))
    ap.add_argument("--sources", default=str(ROOT / "punchout/src"))
    ap.add_argument("--dol", help="main.dol: also check function pointers (addresses built by lifted code, data words)")
    ap.add_argument("--internal", action="append",
                    default=["revolution/bte", "revolution/wpad", "revolution/os", "revolution/vi", "revolution/nand",
                             "revolution/dvd", ".init"],
                    help="a library (prefix) whose data tables only its own replaced code reads")
    args = ap.parse_args()

    plan = []
    for line in open(args.plan):
        if line.startswith("#") or not line.strip():
            continue
        start, end, kind, library = line.rstrip("\n").split("\t")
        plan.append((int(start, 16), int(end, 16), kind, library))

    def decide(address):
        best = None
        for start, end, kind, library in plan:
            if start <= address < end and (best is None or end - start < best[1] - best[0]):
                best = (start, end, kind, library)
        return (best[2], best[3]) if best else ("lift", "")

    provided = set()
    for source in pathlib.Path(args.sources).glob("*.cpp"):
        provided.update(int(m, 16) for m in re.findall(r"\{0x(8[0-9A-Fa-f]{7})u?,\s*\"", source.read_text()))

    missing = {}
    for f in json.load(open(args.cfg))["functions"]:
        caller = int(f["start"], 16)
        if decide(caller)[0] != "lift":
            continue
        for callee in (int(c, 16) for c in f.get("calls", [])):
            kind, library = decide(callee)
            if (kind == "replace" and callee not in provided) or kind == "drop":
                missing.setdefault(callee, (f"{kind}: {library}", set()))[1].add(caller)

    print(f"{len(provided)} replacements provided; {len(missing)} functions called by lifted code are dropped or have none")
    for callee, (library, callers) in sorted(missing.items()):
        print(f"  {callee:08X}  {library}: called from {' '.join(f'{c:08X}' for c in sorted(callers))}")
    pointed = check_pointers(args, decide, provided) if args.dol else {}
    return 1 if missing or pointed else 0


def check_pointers(args, decide, provided):
    import bisect
    import struct
    dol = open(args.dol, "rb").read()
    sections = []
    for i in range(18):
        offset, address, size = (struct.unpack_from(">I", dol, base + 4 * i)[0] for base in (0, 0x48, 0x90))
        if size:
            sections.append((address, size, offset, i < 7))
    functions = sorted((int(f["start"], 16), int(f["end"], 16)) for f in json.load(open(args.cfg))["functions"])
    starts = [s for s, _ in functions]
    start_set = set(starts)

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return functions[i][0] if i >= 0 and functions[i][0] <= a < functions[i][1] else None

    def unanswered(v):
        kind, library = decide(v)
        return v in start_set and ((kind == "replace" and v not in provided) or kind == "drop")

    found = {}
    for address, size, offset, text in sections:
        high = {}
        for o in range(0, size - 3, 4):
            a, w = address + o, struct.unpack_from(">I", dol, offset + o)[0]
            if not text:
                if unanswered(w) and not any(decide(w)[1].startswith(lib) for lib in args.internal) and "debugger stub" not in decide(w)[1]:
                    found.setdefault(w, []).append(f"data {a:08X}")
                continue
            op = w >> 26
            if op == 15 and (w >> 16) & 31 == 0:
                high[(w >> 21) & 31] = ((w & 0xFFFF) << 16, a)
            elif op in (14, 24):
                ra = (w >> 16) & 31 if op == 14 else (w >> 21) & 31
                imm = (w & 0xFFFF) - (0x10000 if op == 14 and w & 0x8000 else 0)
                if ra in high and a - high[ra][1] <= 64:
                    v = (high[ra][0] + imm) & 0xFFFFFFFF
                    by = owner(a)
                    if unanswered(v) and by is not None and decide(by)[0] == "lift":
                        found.setdefault(v, []).append(f"code {a:08X}")
    print(f"{len(found)} functions lifted code or data points at are dropped or have no replacement")
    for v, where in sorted(found.items()):
        print(f"  {v:08X}  {decide(v)[0]}: {decide(v)[1]}: {' '.join(where[:6])}")
    return found


if __name__ == "__main__":
    sys.exit(main())
