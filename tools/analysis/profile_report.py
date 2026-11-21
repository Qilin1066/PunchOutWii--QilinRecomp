#!/usr/bin/env python3
"""Resolve a PUNCHOUT_PROFILE sample file against the executable and print the hottest functions.

    profile_report.py <executable> <samples> [top]

The file (profile.cpp) starts with "base <hex>", the executable's load
address; "thread <n> <tid> <name>" lines name the threads sampled, "map
<start> <end> <offset> <path>" lines the executable mappings (for samples in
libraries), and the rest are "<n>:<hexaddress> <count>". Addresses are mapped
to symbols with `nm -C` (libraries: their dynamic symbols). Lifted guest
functions are reported by name; a summary groups them as "lifted guest code",
and each thread gets its own total and top functions."""
import bisect, os, subprocess, sys

exe, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40


def symbols(path, dynamic):
    out = []
    cmd = ['nm', '-C', '--defined-only'] + (['-D'] if dynamic else []) + [path]
    for line in subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwWiI':
            out.append((int(parts[0], 16), parts[2]))
    out.sort()
    return out, [a for a, _ in out]


syms, starts = symbols(exe, False)
base, threads, maps, raw = 0, {}, [], []
for line in open(samples):
    parts = line.split()
    if parts[0] == 'base':
        base = int(parts[1], 16)
    elif parts[0] == 'thread':
        threads[int(parts[1])] = ' '.join(parts[3:]) + f' (tid {parts[2]})'
    elif parts[0] == 'map':
        maps.append((int(parts[1], 16), int(parts[2], 16), int(parts[3], 16), parts[4]))
    else:
        t, a = parts[0].split(':') if ':' in parts[0] else ('0', parts[0])
        raw.append((int(t), int(a, 16), int(parts[1])))
lib_syms = {}


def name_of(a):
    r = a - base
    i = bisect.bisect_right(starts, r) - 1
    if i >= 0 and r >= 0 and r - starts[i] < 1 << 20 and (not maps or any(m[0] <= a < m[1] and os.path.samefile(m[3], exe) for m in maps if os.path.exists(m[3]))):
        return syms[i][1]
    for lo, hi, off, path in maps:
        if lo <= a < hi and os.path.exists(path):
            if path not in lib_syms:
                lib_syms[path] = symbols(path, True)
            s, st = lib_syms[path]
            # The mapping's file offset is its address in the library for the usual layout.
            r = a - lo + off
            i = bisect.bisect_right(st, r) - 1
            return f'[{os.path.basename(path)}] {s[i][1]}' if i >= 0 else f'[{os.path.basename(path)}]'
    return '[outside every mapping: the kernel, generated code]'


counts, per_thread, total = {}, {}, 0
for t, a, n in raw:
    name = name_of(a)
    total += n
    counts[name] = counts.get(name, 0) + n
    d = per_thread.setdefault(t, {})
    d[name] = d.get(name, 0) + n
print(f'{total} samples')
for name, n in sorted(counts.items(), key=lambda x: -x[1])[:top]:
    print(f'{100 * n / total:6.2f}%  {name[:150]}')
groups = {}
for name, n in counts.items():
    g = 'lifted guest code' if name.startswith('qilin::gen::fn_') else name
    groups[g] = groups.get(g, 0) + n
print('--- grouped')
for name, n in sorted(groups.items(), key=lambda x: -x[1])[:10]:
    print(f'{100 * n / total:6.2f}%  {name[:150]}')
print('--- by thread')
for t, d in sorted(per_thread.items(), key=lambda x: -sum(x[1].values())):
    n = sum(d.values())
    if n * 200 < total:
        continue
    lifted = sum(v for k, v in d.items() if k.startswith('qilin::gen::fn_'))
    print(f'{100 * n / total:6.2f}%  thread {t}: {threads.get(t, "?")}; lifted guest code {100 * lifted / n:.0f}% of it')
    for name, v in sorted(d.items(), key=lambda x: -x[1])[:8]:
        print(f'          {100 * v / n:6.2f}%  {name[:130]}')
