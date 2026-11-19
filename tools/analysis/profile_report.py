#!/usr/bin/env python3
"""Resolve a PUNCHOUT_PROFILE sample file against the executable and print the hottest functions.

    profile_report.py <executable> <samples> [top]

The file (profile.cpp) starts with "base <hex>", the executable's load
address, then "hexaddress count" lines. Addresses are mapped to symbols with
`nm -C`. Lifted guest functions are reported by name; a summary groups them as
"lifted guest code"."""
import bisect, os, subprocess, sys

exe, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(['nm', '-C', '--defined-only', exe], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[1] in 'tTwW':
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
starts = [a for a, _ in syms]
counts, total, base = {}, 0, 0
for line in open(samples):
    a, n = line.split()
    if a == 'base':
        base = int(n, 16)
        continue
    a, n = int(a, 16) - base, int(n)
    total += n
    i = bisect.bisect_right(starts, a) - 1
    name = syms[i][1] if i >= 0 and a - starts[i] < 1 << 20 else '[outside the executable: libraries, driver]'
    counts[name] = counts.get(name, 0) + n
groups = {}
for name, n in counts.items():
    g = 'lifted guest code' if name.startswith('qilin::gen::fn_') else name
    groups[g] = groups.get(g, 0) + n
print(f'{total} samples')
for name, n in sorted(counts.items(), key=lambda x: -x[1])[:top]:
    print(f'{100 * n / total:6.2f}%  {name[:150]}')
print('--- grouped')
for name, n in sorted(groups.items(), key=lambda x: -x[1])[:10]:
    print(f'{100 * n / total:6.2f}%  {name[:150]}')
