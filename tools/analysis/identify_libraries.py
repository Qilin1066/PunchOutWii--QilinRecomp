#!/usr/bin/env python3
"""Find the libraries in Punch-Out!!'s main.dol and where each one starts and ends.

There is no decomp of Punch-Out!!, so each piece of evidence below labels
some functions (`qilin cfg`'s), and the text is then cut into contiguous
segments, one library each:

  1. Size sequences against the Rhythm Heaven Fever decomp (third_party/rhf,
     CC0), library by library: its functions' sizes, in link order, are
     voted into place by runs of three equal sizes, then aligned with
     difflib inside that window. A match inside a run of at least three
     equal sizes ("anchored") labels the function and gives it the decomp's
     name. Each library keeps only its largest cluster of anchors, so a
     coincidental run elsewhere doesn't count. NW4R isn't linked by this
     game: its matches count only inside the HOME Button Menu, which carries
     a private copy (nw4hbm).
  2. C++ RTTI: each vtable starts with a pointer to its class's type info,
     whose first word points at the class name. Wwise's classes (AK::, CAk,
     Ak, IAk) and the HOME Button Menu's (homebutton::, nw4hbm::) label
     their virtual functions.
  3. SDK version banners ("<< RVL_SDK - THP ..."): each library's init
     function loads its __XXXVersion pointer, which names the library.
  4. A few strings that name their library (file names in asserts, device
     paths, the debugger stub's banner), each a whole string, not a pointer
     into the middle of one.

Between two labelled functions of different libraries, the boundary goes
where the unlabelled functions' calls agree best (each prefers the side its
callers and callees are on). Code before the first label is the game and its
engine. `.init` is split by the decomp's names, as in BeatTheBeat-QilinComp.

Usage:
  identify_libraries.py --qilin build/qilin --dol main.dol [--names-out pal_names.txt]

Prints the library table (Markdown) and the boundary checks. The names file is
derived from the game binary: keep it out of git.
"""
import argparse
import bisect
import collections
import difflib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RHF = os.path.join(ROOT, "third_party", "rhf", "SOME01")

SYMBOL = re.compile(r"^(\S+) = (\.init|\.text):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
SPLIT_TEXT = re.compile(r"^\s+(\.init|\.text)\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)")

GAME = "game and Next Level Games' engine"
NEAR = 0x4000  # callers this close are the same client module
WWISE = "Audiokinetic Wwise"

# What each library is, and what happens to it (the boundary BeatTheBeat-QilinComp fixed, with
# the libraries this game adds). lift: recompiled with the game; replace: native at its API;
# drop: never runs.
PLAN = {
    ".init: OS start-up": ("replace", "the runtime starts the game"),
    ".init: EABI runtime (memcpy, __fill_mem, memset)": ("lift", ""),
    ".init: debugger stub and interrupt vectors": ("drop", "MetroTRK's entry, and templates it copies to low memory"),
    GAME: ("lift", ""),
    WWISE: ("lift", "sound engine on AX"),
    "Wwise plug-in: silence source": ("lift", ""),
    "MetroTRK (debugger stub)": ("drop", ""),
    "EABI MSL (C library)": ("lift", ""),
    "EABI runtime": ("lift", ""),
    "revolution/ai": ("lift", "its DMA and registers are emulated, with the DSP (v0.12.0)"),
    "revolution/arc (U8 archives)": ("lift", ""),
    "revolution/ax": ("lift", "Wwise drives it unchanged; the DSP runs its microcode at a high level (v0.12.0)"),
    "revolution/axfx (reverb, chorus, delay on the CPU)": ("lift", ""),
    "revolution/base (CPU registers: HID, MMCR …)": ("replace", ""),
    "revolution/db (debugger hooks)": ("replace", "no debugger"),
    "revolution/dsp": ("lift", "the mailboxes and the task protocol are emulated (v0.12.0)"),
    "revolution/dvd": ("replace", ""),
    "revolution/esp (e-ticket services)": ("replace", ""),
    "revolution/exi": ("replace", ""),
    "revolution/fs": ("replace", ""),
    "revolution/gx": ("lift", "its command stream is the boundary: the runtime decodes the FIFO, as the engine writes it directly too"),
    "revolution/ipc": ("replace", ""),
    "revolution/kpad (Wii Remote processing over WPAD)": ("lift", ""),
    "revolution/mem (heaps)": ("lift", ""),
    "revolution/mtx": ("lift", ""),
    "revolution/os": ("replace", ""),
    "revolution/pad (GameCube controller)": ("replace", ""),
    "revolution/si": ("replace", ""),
    "revolution/thp (movie decoder)": ("lift", "the decoder; its file and audio paths use replaced APIs"),
    "revolution/tpl (texture files)": ("lift", ""),
    "revolution/vi": ("replace", ""),
    "revolution/wbc (Balance Board)": ("lift", "maths over WPAD's data, like KPAD"),
    "revolution/wpad (Wii Remote)": ("replace", ""),
    "revolution/bte (Bluetooth stack, under WPAD)": ("replace", "unused once WPAD is"),
    "revolution/usb (under the Bluetooth stack)": ("replace", "unused once WPAD is"),
    "revolution/sc (console settings)": ("replace", ""),
    "revolution/nand": ("replace", ""),
    "revolution/hbm (HOME Button Menu, with nw4hbm)": ("lift", ""),
    "RVLFaceLibrary (Miis)": ("lift", ""),
    "NdevExi2A (development-kit link)": ("drop", ""),
}

# The decomp's library directories, as named in the table.
RHF_LIB = {
    "PowerPC_EABI_Support/MSL": "EABI MSL (C library)",
    "PowerPC_EABI_Support/Runtime": "EABI runtime",
    "PowerPC_EABI_Support/MetroTRK": "MetroTRK (debugger stub)",
    "NdevExi2A": "NdevExi2A (development-kit link)",
    "RVLFaceLibrary": "RVLFaceLibrary (Miis)",
    "revolution/hbm": "revolution/hbm (HOME Button Menu, with nw4hbm)",
}
for key in PLAN:
    if key.startswith("revolution/"):
        RHF_LIB.setdefault(key.split(" ")[0], key)

# For the call evidence, a library and the one only it calls count as one: MetroTRK reaches
# the development kit through NdevExi2A.
CALL_GROUP = {"NdevExi2A (development-kit link)": "MetroTRK (debugger stub)"}

BANNERS = {"HBM": "revolution/hbm", "RFL": "RVLFaceLibrary"}

# Whole strings that name their library.
STRINGS = [
    ("MetroTRK for Revolution", "MetroTRK (debugger stub)"),
    ("MetroTRK Option", "MetroTRK (debugger stub)"),
    ("GCN_Mem_Alloc.c", "EABI runtime"),
    ("arc.c", "revolution/arc (U8 archives)"),
    ("ARCInitHandle", "revolution/arc (U8 archives)"),
    ("TPL.c", "revolution/tpl (texture files)"),
    ("DBExceptionDestination", "revolution/db (debugger hooks)"),
    ("DSP is booting task", "revolution/dsp"),
    ("(doTransactionCallback)", "revolution/esp (e-ticket services)"),
    ("/shared2/test2/nanderr.log", "revolution/nand"),
    ("Illegal status is detected at %s()", "revolution/nand"),
    ("BTA_HH", "revolution/bte (Bluetooth stack, under WPAD)"),
    ("bta_hh", "revolution/bte (Bluetooth stack, under WPAD)"),
]

PRIVILEGED = {"rfi", "mfmsr", "mtmsr", "mtsr", "mtsrin", "mfsr", "mfsrin", "tlbie", "tlbsync", "dcbi"}


def is_float(m):
    return m.startswith(("f", "lf", "stf", "ps_", "psq_")) or m in ("mffs", "mtfsf", "mtfsfi", "mtfsb0", "mtfsb1")


# ---------------------------------------------------------------- inputs

class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        self.sections = []
        for i in range(18):
            off, addr, size = (struct.unpack(">I", d[o + 4 * i:o + 4 * i + 4])[0] for o in (0, 0x48, 0x90))
            if size:
                self.sections.append((i < 7, off, addr, size))

    def u32(self, a):
        for _, off, addr, size in self.sections:
            if addr <= a <= addr + size - 4:
                return struct.unpack(">I", self.data[off + a - addr:off + a - addr + 4])[0]
        return None

    def data_words(self):
        for text, off, addr, size in self.sections:
            if text:
                continue
            for k in range(0, size - 3, 4):
                yield addr + k, struct.unpack(">I", self.data[off + k:off + k + 4])[0]

    def find(self, needle):
        for text, off, addr, size in self.sections:
            i = self.data[off:off + size].find(needle)
            if i >= 0:
                return addr + i
        return None

    def in_text(self, a):
        return any(t and addr <= a < addr + size for t, _, addr, size in self.sections)

    def string(self, a, whole=True):
        """The C string at a data address, if printable; with whole, only if it starts there."""
        for text, off, addr, size in self.sections:
            if text or not addr <= a < addr + size:
                continue
            i = off + a - addr
            if whole and a > addr and self.data[i - 1] != 0:
                return None
            b = self.data[i:i + 200].split(b"\0")[0]
            if len(b) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in b):
                return b.decode()
        return None


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode not in (0, 3):  # coverage exits 3 while words are unknown
        sys.exit(f"{' '.join(cmd)} failed: {r.stderr}")
    return r.stdout


def load_rhf():
    funcs = []
    for line in open(os.path.join(RHF, "symbols.txt")):
        m = SYMBOL.match(line)
        if m:
            funcs.append((int(m[3], 16), int(m[4], 16), m[1]))
    funcs.sort()
    units, unit = [], None
    for line in open(os.path.join(RHF, "splits.txt")):
        if line[:1] not in ("", " ", "\t") and line.rstrip().endswith(":") and not line.startswith("Sections"):
            unit = line.rstrip()[:-1]
        m = SPLIT_TEXT.match(line)
        if m and unit:
            units.append((int(m[2], 16), int(m[3], 16), unit))
    units.sort()
    starts = [u[0] for u in units]

    def library(a):
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or not units[i][0] <= a < units[i][1]:
            return None
        parts = units[i][2].split("/")
        return "/".join(parts[:2]) if len(parts) > 2 else parts[0]

    by_lib = collections.defaultdict(list)
    for a, size, name in funcs:
        lib = library(a)
        if lib and not lib.startswith("Game"):
            by_lib[lib].append((size, name))
    return by_lib


# ---------------------------------------------------------------- evidence

def size_matches(funcs, by_lib):
    """{function index: (library, decomp name)} from anchored size matches."""
    sizes = [e - s for s, e in funcs]
    index = collections.defaultdict(list)
    for i in range(len(sizes) - 2):
        index[tuple(sizes[i:i + 3])].append(i)
    out = {}
    for lib, fs in by_lib.items():
        s = [size for size, _ in fs]
        votes = collections.Counter()
        for i in range(len(s) - 2):
            hits = index.get(tuple(s[i:i + 3]), [])
            if len(hits) <= 8:
                for p in hits:
                    votes[p - i] += 1
        good = [(o, v) for o, v in votes.items() if v >= 2]
        if not good:
            continue
        offsets = sorted(o for o, v in good for _ in range(v))
        median = offsets[len(offsets) // 2]
        keep = [o for o, _ in good if abs(o - median) <= len(s) * 1.5 + 20]
        lo, hi = max(0, min(keep) - 20), min(len(sizes), max(keep) + len(s) + 20)
        matcher = difflib.SequenceMatcher(None, s, sizes[lo:hi], autojunk=False)
        anchors, pairs = [], []
        for block in matcher.get_matching_blocks():
            run = [(lo + block.b + k, fs[block.a + k][1]) for k in range(block.size)]
            if block.size >= 3:
                anchors += run
            elif block.size == 2:
                pairs += run
        # The largest cluster of anchors only.
        anchors.sort()
        clusters, cur = [], []
        for x in anchors:
            if cur and x[0] - cur[-1][0] > 200:
                clusters.append(cur)
                cur = []
            cur.append(x)
        if cur:
            clusters.append(cur)
        best = max(clusters, key=len) if clusters else []
        for i, name in best:
            out.setdefault(i, (lib, name, "anchored"))
        # Two equal sizes in a row count near the cluster (40 functions either side): versions differ.
        if best:
            first, last = best[0][0] - 40, best[-1][0] + 40
            for i, name in pairs:
                if first <= i <= last:
                    out.setdefault(i, (lib, name, "pair"))
    return out


def rtti_labels(dol, funcs):
    starts = {s: i for i, (s, _) in enumerate(funcs)}
    words = dict(dol.data_words())
    referenced = collections.defaultdict(list)
    for a, v in words.items():
        referenced[v].append(a)
    out = {}
    classes = 0
    for ti, name_ptr in words.items():
        name = dol.string(name_ptr, whole=False)
        if not name or not re.match(r"^[A-Za-z_][A-Za-z0-9_:<>, *&]*$", name):
            continue
        if name.startswith(("AK::", "CAk", "Ak", "IAk")):
            lib = WWISE
        elif name.startswith(("homebutton::", "nw4hbm::")):
            lib = PLAN_KEY["revolution/hbm"]
        else:
            continue
        for vt in referenced.get(ti, []):
            k = vt + 8
            if not dol.in_text(words.get(k, 0) or 0):
                continue
            classes += 1
            while dol.in_text(words.get(k, 0) or 0):
                f = words[k]
                if f in starts:
                    out.setdefault(starts[f], (lib, name))
                k += 4
    return out, classes


def string_refs(funcs, insns, dol, r2, r13):
    """{function index: [whole strings it builds the address of]}."""
    def sx(v):
        return v - 0x10000 if v & 0x8000 else v
    out = collections.defaultdict(list)
    for i, (s, e) in enumerate(funcs):
        hi = {}
        for a in range(s, e, 4):
            w = insns.get(a, (0, ""))[0]
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                hi[rd] = imm << 16
                continue
            value = None
            if op in (14, 24) and ra in hi:
                value = (hi[ra] + sx(imm)) & 0xFFFFFFFF if op == 14 else hi[ra] | imm
            elif op in (14, 32) and ra in (2, 13):  # addi or lwz from a small-data base
                base = (r2 if ra == 2 else r13) + sx(imm)
                value = base if op == 14 else dol.u32(base)
            if value is not None:
                text = dol.string(value)
                if text:
                    out[i].append(text)
    return out


def small_data_bases(insns, entry):
    regs = {}
    for a in range(entry, entry + 0x400, 4):
        w = insns.get(a, (0, ""))[0]
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0 and rd in (2, 13):
            regs[rd] = imm << 16
        elif op == 24 and rd == ra and ra in (2, 13) and ra in regs:
            regs[ra] |= imm
    if 2 not in regs or 13 not in regs:
        sys.exit("couldn't find _SDA_BASE_ and _SDA2_BASE_ in __start")
    return regs[2], regs[13]


PLAN_KEY = {key.split(" ")[0]: key for key in PLAN}


# Narrower ranges inside a replaced library that are lifted after all: code with
# no hardware access whose exact behaviour matters more than a native version
# (the heap allocator decides the game's memory layout). PAL addresses; each
# range is whole functions. Read by qilin lift --plan, where the narrowest
# range holding a function decides.
EXCEPTIONS = [
    (0x80200520, 0x802008F0, "lift", "revolution/os: OSAlloc (heaps) and OSArena (arena bounds), no hardware access"),
    (0x80201C50, 0x80201CDC, "lift", "revolution/os: OSReport (formats through MSL's vprintf)"),
    (0x8020C674, 0x8020C728, "lift", "revolution/os: __init_user, __init_cpp and exit (the EABI's constructors and destructors)"),
    # The WBC/WPAD boundary the evidence left open (v0.10.0): the seven functions before
    # __wpadNoAlloc keep WBC's state (r13-12808) and do its calibration maths, so WBC runs on
    # to 8021698C, and is lifted as planned.
    (0x8021636C, 0x8021698C, "lift", "revolution/wbc: its API (WBCSetupCalibration, WBCRead ...) before WPAD's first function"),
    # The NAND/HBM boundary (±3, v0.11.0): the last two functions before the HOME Button
    # Menu read a compression header and run an LZ decoder, with no calls: the CX library.
    (0x8025E6C0, 0x8025E840, "lift", "revolution/cx: CXGetUncompressedSize, CXUncompressLZ (pure code after NAND)"),
    # The MTX/OS boundary: the first three functions of the OS range are vector maths in
    # paired singles (scale, dot product, ...), MTX's, ending where __OSFPRInit starts.
    (0x801FE598, 0x801FE770, "lift", "revolution/mtx: vector functions before the OS's first (__OSFPRInit)"),
    # MSL's __write_console sends console output to the debugger too, after the UART: that
    # one MetroTRK function is reached in normal running, so it's replaced (by nothing).
    (0x801C93D8, 0x801C9494, "replace", "MetroTRK: __TRK_write_console, called by MSL's __write_console"),
]


def plan_key(rhf_lib):
    if rhf_lib in RHF_LIB:
        return RHF_LIB[rhf_lib]
    return PLAN_KEY.get(rhf_lib)


# ---------------------------------------------------------------- segmentation

def segment(funcs, seeds, source, edges, calls, first_text):
    """Cut the functions into contiguous runs, one library each. Returns the label of
    every function, and for each boundary between labelled functions how many
    positions the call evidence leaves open.

    A function in a gap leans towards the side its callers and callees outside
    the gap are on; on a tie, towards the side whose functions are called from
    the same client code (callers within NEAR bytes of each other: a library
    is used by a module, not by one function)."""
    callers = collections.defaultdict(set)
    for i, targets in calls.items():
        for t in targets:
            callers[t].add(i)
    n = len(funcs)
    label = [None] * n
    for i, lib in seeds.items():
        label[i] = lib
    if label[first_text] is None:
        label[first_text] = GAME
    # Drop short runs of size-only labels inside another library's labels: coincidences.
    idx = [i for i in range(n) if label[i]]
    runs = []
    for i in idx:
        if runs and label[runs[-1][-1]] == label[i]:
            runs[-1].append(i)
        else:
            runs.append([i])
    for k in range(1, len(runs) - 1):
        run = runs[k]
        if len(run) <= 3 and all(source.get(i) == "size" for i in run) and \
                label[runs[k - 1][0]] == label[runs[k + 1][0]] != label[run[0]]:
            for i in run:
                label[i] = None
    boundaries = []
    assigned = list(label)
    for round_ in range(3):  # recompute splits with the previous round's labels
        boundaries = []
        idx = [i for i in range(n) if label[i]]
        nxt = list(label)
        for a, b in zip(idx, idx[1:]):
            left, right = label[a], label[b]
            gap = range(a + 1, b)
            if left == right:
                for m in gap:
                    nxt[m] = left
                continue

            near = {}
            for lib in (left, right):
                users = sorted(funcs[c][0] for t in range(n) if assigned[t] == lib and not a < t < b
                               for c in callers[t] if not a < c < b and assigned[c] != lib)
                near[lib] = users

            def used_near(c, lib):
                users = near[lib]
                k = bisect.bisect_left(users, funcs[c][0] - NEAR)
                return k < len(users) and users[k] <= funcs[c][0] + NEAR

            def affinity(m, lib):
                direct = sum(1 for o in edges[m] if not a < o < b and CALL_GROUP.get(assigned[o], assigned[o]) == lib)
                shared = sum(1 for c in callers[m] if not a < c < b and used_near(c, lib))
                return (direct, shared)
            pre = [(0, 0)]
            for m in gap:
                x = affinity(m, left)
                pre.append((pre[-1][0] + x[0], pre[-1][1] + x[1]))
            suf = [(0, 0)]
            for m in reversed(gap):
                x = affinity(m, right)
                suf.append((suf[-1][0] + x[0], suf[-1][1] + x[1]))
            suf.reverse()
            scores = [(pre[k][0] + suf[k][0], pre[k][1] + suf[k][1]) for k in range(len(gap) + 1)]
            best = max(scores)
            ks = [k for k, v in enumerate(scores) if v == best]
            # A tie: libraries stay as small as the evidence allows next to the game's code;
            # between two libraries, the middle of the tied positions.
            if left == GAME:
                k = ks[-1]
            elif right == GAME:
                k = ks[0]
            else:
                k = ks[len(ks) // 2]
            split = a + 1 + k
            boundaries.append((funcs[split][0] if split < n else funcs[-1][1], left, right, len(ks) - 1))
            for m in gap:
                nxt[m] = left if m < split else right
        for i in range(idx[-1] + 1, n):
            nxt[i] = label[idx[-1]]
        assigned = nxt
    return assigned, boundaries


# ---------------------------------------------------------------- main

def analyse(qilin, dol_path):
    """Everything the library table is built from, for this script and the others in tools/analysis."""
    dol = Dol(dol_path)
    with tempfile.TemporaryDirectory() as tmp:
        cfg_path = os.path.join(tmp, "cfg.json")
        run([qilin, "cfg", dol_path, "--json", cfg_path])
        cfg = json.load(open(cfg_path))
        listing = os.path.join(tmp, "insns.txt")
        run([qilin, "insns", dol_path, "-o", listing])
        insns = {}
        for line in open(listing):
            p = line.split(None, 2)
            text = p[2].strip() if len(p) > 2 else ""
            insns[int(p[0], 16)] = (int(p[1], 16), text.split()[0] if text else "", text)
    coverage = run([qilin, "coverage", dol_path, "--list"])
    unknown = [(int(m[1], 16), int(m[2], 16)) for m in re.finditer(r"^\s+([0-9A-F]{8})-([0-9A-F]{8})", coverage, re.M)]

    fdefs = sorted(cfg["functions"], key=lambda f: int(f["start"], 16))
    funcs = [(int(f["start"], 16), int(f["end"], 16)) for f in fdefs]
    pos = {s: i for i, (s, _) in enumerate(funcs)}
    edges = collections.defaultdict(set)
    calls = collections.defaultdict(set)
    for i, f in enumerate(fdefs):
        for t in f.get("calls", []) + f.get("tails", []):
            j = pos.get(int(t, 16))
            if j is not None and j != i:
                edges[i].add(j)
                edges[j].add(i)
                calls[i].add(j)
    text_sections = sorted((addr, addr + size) for t, _, addr, size in dol.sections if t)
    init_end = text_sections[0][1]
    first_text = next(i for i, (s, _) in enumerate(funcs) if s >= init_end)

    by_lib = load_rhf()
    sized = size_matches(funcs, by_lib)
    names = {}
    seeds = {}
    hbm_lo = min((i for i, (lib, *_) in sized.items() if lib == "revolution/hbm"), default=None)
    hbm_hi = max((i for i, (lib, *_) in sized.items() if lib == "revolution/hbm"), default=None)
    source = {}
    for i, (lib, name, kind) in sized.items():
        if lib.startswith("nw4r/"):
            if hbm_lo is None or not hbm_lo <= i <= hbm_hi:
                continue
            lib = "revolution/hbm"
        key = plan_key(lib)
        if key:
            seeds[i] = key
            names[i] = name
            source[i] = "size"
    rtti, classes = rtti_labels(dol, funcs)
    for i, (lib, _) in rtti.items():
        seeds[i] = lib
        source[i] = "rtti"
    r2, r13 = small_data_bases(insns, struct.unpack(">I", dol.data[0xE0:0xE4])[0])
    refs = string_refs(funcs, insns, dol, r2, r13)
    banner = re.compile(r"^<< RVL_SDK - (\w+)")
    for i, texts in refs.items():
        for t in texts:
            m = banner.match(t)
            if m:
                lib = BANNERS.get(m[1], "revolution/" + m[1].lower())
                key = plan_key(lib)
                if key:
                    seeds[i] = key
                    source[i] = "banner"
            for needle, key in STRINGS:
                if t.startswith(needle):
                    seeds[i] = key
                    source[i] = "string"
    # The silence source plug-in is Wwise's, linked with the game's code.
    for i, (lib, cls) in rtti.items():
        if funcs[i][0] < funcs[first_text][0] + 0x200000 and "Silence" in cls:
            seeds[i] = "Wwise plug-in: silence source"

    labels, boundaries = segment(funcs, {i: v for i, v in seeds.items() if i >= first_text}, source, edges, calls, first_text)

    # .init: first the debugger stub's code, its name string and its interrupt vectors (each
    # ends in rfi); then the start-up code, with the EABI runtime's memcpy (a byte copy, forwards
    # or backwards), __fill_mem and memset (the decomp's sizes, 0xB4 and 0x30, in a row).
    vectors_end = max((e for s, e in funcs[:first_text]
                       if any(insns.get(a, (0, ""))[1] == "rfi" for a in range(s, e, 4))), default=0)
    for i in range(first_text):
        labels[i] = ".init: debugger stub and interrupt vectors" if funcs[i][0] < vectors_end else ".init: OS start-up"
    runtime = ".init: EABI runtime (memcpy, __fill_mem, memset)"
    for i in range(first_text - 1):
        if funcs[i][1] - funcs[i][0] == 0xB4 and funcs[i + 1][1] - funcs[i + 1][0] == 0x30 and labels[i] == ".init: OS start-up":
            labels[i] = labels[i + 1] = runtime
            mnemonics = [insns.get(a, (0, ""))[1] for a in range(*funcs[i - 1])]
            if "lbzu" in mnemonics and "stbu" in mnemonics:
                labels[i - 1] = runtime

    # Segments.
    segs = []
    for i, (s, e) in enumerate(funcs):
        if segs and segs[-1]["lib"] == labels[i]:
            segs[-1]["end"] = e
            segs[-1]["funcs"].append(i)
        else:
            segs.append({"lib": labels[i], "start": s, "end": e, "funcs": [i]})
    for seg in segs:
        words = fp = ps = priv = 0
        for i in seg["funcs"]:
            s, e = funcs[i]
            for a in range(s, e, 4):
                m = insns.get(a, (0, ""))[1]
                if not m or m.startswith("."):
                    continue
                words += 1
                fp += is_float(m)
                ps += m.startswith(("ps_", "psq_"))
                priv += m in PRIVILEGED
        seg.update(words=words, fp=fp, ps=ps, priv=priv)
        seg["matched"] = sum(1 for i in seg["funcs"] if i in names)
        seg["evidence"] = sum(1 for i in seg["funcs"] if i in seeds)

    return dict(dol=dol, insns=insns, fdefs=fdefs, funcs=funcs, labels=labels, segs=segs, names=names, seeds=seeds,
                unknown=unknown, boundaries=boundaries, sized=sized, classes=classes, refs=refs, banner=banner,
                first_text=first_text, edges=edges, calls=calls)


def segment_of(result, address):
    """The segment holding a text address, or None."""
    for seg in result["segs"]:
        if seg["start"] <= address < seg["end"]:
            return seg
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--names-out", help="write the decomp names matched to PAL addresses (derived from the game: keep out of git)")
    ap.add_argument("--plan-out", help="write the library plan for qilin lift --plan (start, end, plan, library; tab-separated)")
    args = ap.parse_args()
    r = analyse(args.qilin, args.dol)
    if args.plan_out:
        with open(args.plan_out, "w", newline="\n") as f:
            f.write("# The library plan: which code is lifted, replaced or dropped (docs/code-analysis.md).\n"
                    "# Written by tools/analysis/identify_libraries.py --plan-out; read by qilin lift --plan.\n"
                    "# start\tend\tplan\tlibrary\n")
            for seg in r["segs"]:
                f.write(f"{seg['start']:08X}\t{seg['end']:08X}\t{PLAN.get(seg['lib'], ('?',))[0]}\t{seg['lib']}\n")
            f.write("# Exceptions: narrower ranges that override the library they sit in.\n")
            for lo, hi, plan, note in EXCEPTIONS:
                if not any(lo == fs for fs, _ in r["funcs"]) or not any(hi == fe for _, fe in r["funcs"]):
                    raise SystemExit(f"exception {lo:08X}-{hi:08X} doesn't start and end at function boundaries")
                f.write(f"{lo:08X}\t{hi:08X}\t{plan}\t{note}\n")
    segs, funcs, names, seeds, unknown, boundaries = r["segs"], r["funcs"], r["names"], r["seeds"], r["unknown"], r["boundaries"]
    sized, classes, refs, banner = r["sized"], r["classes"], r["refs"], r["banner"]

    plan_words = collections.Counter()
    print("| Library | PAL range | Functions | Decomp names | Words | fp | ps | priv | Plan |")
    print("|---|---|---|---|---|---|---|---|---|")
    for seg in segs:
        plan = PLAN.get(seg["lib"], ("?", ""))
        plan_words[plan[0]] += seg["words"]
        note = f" ({plan[1]})" if plan[1] else ""
        print(f"| {seg['lib']} | `{seg['start']:08X}`–`{seg['end']:08X}` | {len(seg['funcs']):,} | {seg['matched']:,} | "
              f"{seg['words']:,} | {seg['fp']:,} | {seg['ps']:,} | {seg['priv']} | {plan[0]}{note} |")
    total = sum(plan_words.values())
    print()
    print("Totals: " + ", ".join(f"{k} {v:,} words ({100 * v / total:.0f}%)" for k, v in plan_words.most_common()))
    print(f"Evidence: {len(sized)} anchored size matches, {classes} vtables, "
          f"{sum(1 for t in refs.values() for x in t if banner.match(x))} banner loads, {len(seeds)} labelled functions of {len(funcs)}")

    open_ = [(a, l, r, w) for a, l, r, w in boundaries if w]
    if open_:
        print("Boundaries the calls leave open (positions): " +
              ", ".join(f"{a:08X} {l.split(' ')[0]}|{r.split(' ')[0]} ±{w}" for a, l, r, w in open_))

    # Checks.
    problems = 0
    for seg in segs:
        plan = PLAN.get(seg["lib"], ("?", ""))[0]
        if plan == "lift" and seg["priv"]:
            print(f"CHECK: {seg['priv']} privileged instructions in lifted {seg['lib']}")
            problems += 1
        for lo, hi in unknown:
            if plan == "lift" and seg["start"] < hi and lo < seg["end"]:
                print(f"CHECK: unknown words {lo:08X}-{hi:08X} in lifted {seg['lib']}")
                problems += 1
        if plan == "?":
            print(f"CHECK: no plan for {seg['lib']}")
            problems += 1
    seen = collections.Counter(seg["lib"] for seg in segs)
    for lib, n in seen.items():
        if n > 1 and not lib.startswith(".init"):  # .init interleaves start-up and runtime code
            print(f"CHECK: {lib} is in {n} pieces")
    print("checks: " + ("all passed" if not problems else f"{problems} problems"))

    if args.names_out:
        with open(args.names_out, "w") as f:
            for i in sorted(names):
                f.write(f"{funcs[i][0]:08X} {funcs[i][1] - funcs[i][0]:X} {names[i]}\n")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
