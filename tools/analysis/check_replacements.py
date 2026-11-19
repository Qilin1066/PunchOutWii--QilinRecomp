#!/usr/bin/env python3
"""Find calls from lifted code that would stop the game: into replaced
functions with no replacement, and into dropped code.

A function in a "replace" range of the library plan runs only if punchout/src
provides it (a {0x8xxxxxxx, "name", fn} entry); otherwise the call stops the
game with an error naming it. This lists every such function that lifted code
calls directly, with its callers, so they can be written before the game gets
there. Calls through function pointers aren't in the call graph and aren't
checked. Lifted code calling dropped code (which isn't translated at all) is
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
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
