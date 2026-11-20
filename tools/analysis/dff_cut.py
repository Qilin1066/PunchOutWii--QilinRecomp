#!/usr/bin/env python3
"""Cut a Dolphin FIFO log (from PUNCHOUT_DFF) right after one primitive, so
Dolphin's FIFO player renders the frame only up to that draw: the EFB as it
stands then, copied to the frame buffer. Comparing that with PUNCHOUT_PROBE's
value after the same draw finds the first draw where our renderer and Dolphin
part ways.

  dff_cut.py <frame.dff> <primitive serial> <out.dff> [writes]

<frame.dff>.prims (written beside it) says where each primitive is: in the
command stream (cut there), or inside a display list (the call is replaced by
the list's commands up to the primitive, inlined, with the log's memory
updates up to the call applied). Then the frame's own XFB copy registers end
it. `writes` ("E0=8FF0FF,xf100E=0000060B", hex) are BP (or, with "xf", XF)
register writes put just before the primitive's call, to test what a register
does to the picture.
"""
import struct
import sys


def main() -> int:
    src, serial, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    # Optional BP writes ("E0=FF6EFF,..." in hex) put just before the call that holds the primitive,
    # to test what a register does to the drawing.
    # "xf100E=0000060F" writes an XF register instead.
    def write(r, v):
        if r.lower().startswith("xf"):
            return b"\x10" + struct.pack(">HHI", 0, int(r[2:], 16), int(v, 16))
        return b"\x61" + struct.pack(">I", (int(r, 16) << 24) | int(v, 16))
    before = b"".join(write(*w.split("=")) for w in sys.argv[4].split(",")) if len(sys.argv) > 4 else b""
    data = bytearray(open(src, "rb").read())
    prims, calls, xfb = {}, [], None
    for line in open(src + ".prims"):
        f = line.split()
        if not f or f[0] == "#":
            continue
        if f[0] == "xfb":
            xfb = [int(v, 16) for v in f[1:]]
        elif f[0] == "call":
            calls.append((int(f[2]), int(f[3], 16), int(f[4])))
        else:
            # dl primitives: address, size, and where the call that ran them is
            prims[int(f[0])] = (f[1], int(f[2], 16) if f[1] == "dl" else int(f[2]), int(f[3]), int(f[4]) if f[1] == "dl" else 0)
    kind, where, size, call_at = prims[serial]

    frame_at = struct.unpack_from("<Q", data, 60)[0]  # FileHeader.frameListOffset
    fifo_at, fifo_size = struct.unpack_from("<QI", data, frame_at)
    upd_at, upd_n = struct.unpack_from("<QI", data, frame_at + 20)  # FileFrameInfo.memoryUpdatesOffset
    fifo = bytes(data[fifo_at:fifo_at + fifo_size])
    # MEM1 and MEM2 as the frame starts, then what the GPU read as the frame went
    # on (the capture's memory updates, each at a place in the commands).
    base, updates = {}, []
    for i in range(upd_n):
        at, addr, off, n = struct.unpack_from("<IIQI", data, upd_at + 24 * i)
        if at == 0 and addr in (0, 0x10000000) and n >= 0x01800000:
            base[addr] = off
        else:
            updates.append((at, addr, off, n))

    def ram(addr, n, upto):
        addr &= 0x1FFFFFFF
        b = 0x10000000 if addr >= 0x10000000 else 0
        out = bytearray(data[base[b] + addr - b: base[b] + addr - b + n])
        for at, a, off, size in updates:  # in order, so later ones win
            if at > upto or a + size <= addr or a >= addr + n:
                continue
            lo, hi = max(a, addr), min(a + size, addr + n)
            out[lo - addr:hi - addr] = data[off + lo - a: off + hi - a]
        return bytes(out)

    if kind == "fifo":
        cut = fifo[:where] + before + fifo[where:where + size]
    else:
        call = next(c for c in calls if c[0] == call_at)
        cut = fifo[:call[0]] + before + ram(call[1], where + size - call[1], call[0])
    regs = (0x49, 0x4A, 0x4B, 0x4D, 0x4E, 0x52)
    tail = b"".join(b"\x61" + struct.pack(">I", (r << 24) | v) for r, v in zip(regs, xfb))
    cut += tail + b"\x00" * ((-len(cut) - len(tail)) % 32 + 32)

    # The new log: the same, with the commands replaced (put at the end, so the other offsets hold).
    new_at = len(data)
    data += cut
    struct.pack_into("<QI", data, frame_at, new_at, len(cut))
    open(out, "wb").write(data)
    print(f"{out}: primitive {serial} ({kind}), {len(cut)} bytes of commands")
    return 0


if __name__ == "__main__":
    sys.exit(main())
