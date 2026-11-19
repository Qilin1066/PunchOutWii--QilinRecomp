#!/usr/bin/env python3
"""The runtime's floating-point helpers against exact rational arithmetic.

fp_eval (C++) applies rt::fp as lifted code does for each instruction; this
script computes what each result must be with Python's Fraction, rounding by
the rule the instruction has on Broadway, and compares the bits. Nothing here
uses the host's floating-point arithmetic for a result: values are exact
fractions, rounded to nearest-even at the target precision, with gradual
underflow and overflow to infinity, then the NI-mode flush and the
instruction's own rules:
  - NaN operands: the first NaN of a, b, c (in the manual's order), made
    quiet; an invalid operation (inf-inf, 0*inf, 0/0, inf/inf) gives the
    default NaN, positive.
  - Double results (fadd ... fmsub): a subnormal result becomes a signed zero.
  - Single results: anything below the smallest normal single before rounding
    becomes a signed zero, then round to single (fadds etc. round the double
    result: the double rounding Broadway's single ops are modelled with, as in
    Dolphin; fmadds/fmsubs round the exact result to single only once).
  - fmuls and the single multiply-adds round frC to 25 significant bits first
    (24 fraction bits), ties away from zero.
  - fctiw rounds to nearest-even, fctiwz truncates; both saturate.
  - stfs truncates (the manual's conversion); lfs is exact.
  - fres and frsqrte: the special cases and an accuracy of 1/4096 from the
    manual, plus frsqrte's hardware results for Dolphin's 57 test inputs
    (Source/UnitTests/Common/FloatUtilsTest.cpp, measured on a Wii).
  - Paired-single quantisation: dequantise exactly; quantise rounds to single,
    scales, clamps and truncates toward zero; NaN stores 0.

  fp_check.py <fp_eval> [--cases N] [--seed S]
"""
import argparse
import random
import subprocess
import sys
from fractions import Fraction as Q

SIGN, EXP, FRAC, QUIET = 1 << 63, 0x7FF << 52, (1 << 52) - 1, 1 << 51
DNAN = 0x7FF8 << 48
INF = "inf"


# ---------------------------------------------------------------- exact values
def is_nan(x): return (x & ~SIGN) > EXP
def is_inf(x): return (x & ~SIGN) == EXP
def neg(x): return x >> 63


def value(x):
    """A finite double's exact value."""
    e, f = (x >> 52) & 0x7FF, x & FRAC
    v = Q(f, 1 << 1074) if e == 0 else Q((1 << 52) | f) * Q(2) ** (e - 1075)
    return -v if neg(x) else v


def ilog2(a):
    """floor(log2(a)) for a positive Fraction."""
    e = a.numerator.bit_length() - a.denominator.bit_length()
    while Q(2) ** e > a: e -= 1
    while Q(2) ** (e + 1) <= a: e += 1
    return e


def round_nearest_even(q, p, emin, emax):
    """|q| rounded to a float with p significant bits, normal range 2^emin..,
    gradual underflow: a Fraction, or INF on overflow."""
    a = abs(q)
    if a == 0: return Q(0)
    quantum = Q(2) ** (max(ilog2(a), emin) - p + 1)
    n = a / quantum
    whole = n.numerator // n.denominator
    rest = n - whole
    if rest > Q(1, 2) or (rest == Q(1, 2) and whole % 2): whole += 1
    r = whole * quantum
    return INF if r >= Q(2) ** (emax + 1) else r


def encode_double(negative, a):
    """A magnitude (Fraction, exactly a double, or INF) and a sign as bits."""
    s = SIGN if negative else 0
    if a == INF: return s | EXP
    if a == 0: return s
    if a < Q(2) ** -1022: return s | int(a * 2 ** 1074)
    e = ilog2(a)
    f = a / Q(2) ** e * (1 << 52) - (1 << 52)
    assert f.denominator == 1, "not a double"
    return s | ((e + 1023) << 52) | int(f)


def to_double(q, zero_negative=False):
    """An exact value rounded to double (gradual underflow)."""
    r = round_nearest_even(q, 53, -1022, 1023)
    return encode_double(q < 0 or (q == 0 and zero_negative), r)


def to_single_value(q, zero_negative=False):
    """An exact value rounded to single precision, as a double's bits."""
    r = round_nearest_even(q, 24, -126, 127)
    return encode_double(q < 0 or (q == 0 and zero_negative), r)


def flush(x):
    """NI: a subnormal double is a signed zero."""
    return x & SIGN if (x & EXP) == 0 else x


def single(x):
    """frsp under NI."""
    if is_nan(x): return (x & ~((1 << 29) - 1)) | QUIET | EXP  # quiet, payload kept to single width
    if (x & ~SIGN) < 0x3810000000000000: return x & SIGN
    if is_inf(x): return x
    return to_single_value(value(x))


def quiet(x): return x | QUIET


def first_nan(*xs):
    for x in xs:
        if is_nan(x): return quiet(x)
    return None


# ---------------------------------------------------------------- arithmetic
def add_exact(a, b):
    """a + b: bits of the double result (no flush), IEEE zero signs."""
    n = first_nan(a, b)
    if n is not None: return n
    if is_inf(a) or is_inf(b):
        if is_inf(a) and is_inf(b) and neg(a) != neg(b): return DNAN
        return a if is_inf(a) else b
    s = value(a) + value(b)
    return to_double(s, zero_negative=(s == 0 and neg(a) and neg(b)))


def sub_exact(a, b):
    n = first_nan(a, b)
    if n is not None: return n
    return add_exact(a, b ^ SIGN)


def mul_exact(a, b):
    n = first_nan(a, b)
    if n is not None: return n
    sign = neg(a) ^ neg(b)
    if is_inf(a) or is_inf(b):
        if (not is_inf(a) and value(a) == 0) or (not is_inf(b) and value(b) == 0): return DNAN
        return (SIGN if sign else 0) | EXP
    return to_double(value(a) * value(b), zero_negative=bool(sign))


def div_exact(a, b):
    n = first_nan(a, b)
    if n is not None: return n
    sign = SIGN if neg(a) ^ neg(b) else 0
    if is_inf(a): return DNAN if is_inf(b) else sign | EXP
    if is_inf(b): return sign
    if value(b) == 0: return DNAN if value(a) == 0 else sign | EXP
    return to_double(value(a) / value(b), zero_negative=bool(sign))


def round25(x):
    """frC of single multiplies: 25 significant bits, ties away from zero."""
    if is_nan(x) or is_inf(x):  # payload bits round as the integer they are (Dolphin's Force25Bit)
        return (x & 0xFFFFFFFFF8000000) + (x & 0x8000000)
    v = value(x)
    if v == 0: return x
    quantum = Q(2) ** (ilog2(abs(v)) - 24)  # 25 significant bits: 24 fraction bits and the implicit one
    n = abs(v) / quantum
    r = (n + Q(1, 2)).numerator // (n + Q(1, 2)).denominator * quantum
    return encode_double(v < 0, INF if r >= Q(2) ** 1024 else r)


def madd_parts(a, c, b, subtract):
    """a*c +/- b: (NaN or special result bits, or None) and the exact value."""
    bb = b ^ SIGN if subtract else b
    if is_nan(a) or is_nan(b) or is_nan(c): return first_nan(a, b, c), None
    if is_inf(a) or is_inf(c):
        if (not is_inf(a) and value(a) == 0) or (not is_inf(c) and value(c) == 0): return DNAN, None
        prod_neg = neg(a) ^ neg(c)
        if is_inf(bb) and neg(bb) != prod_neg: return DNAN, None
        return (SIGN if prod_neg else 0) | EXP, None
    if is_inf(bb): return bb, None
    p = value(a) * value(c)
    s = p + value(bb)
    prod_neg = bool(neg(a) ^ neg(c))
    zero_neg = s == 0 and (prod_neg and neg(bb)) if True else False
    return None, (s, zero_neg)


def fmadd(a, c, b, subtract):
    special, exact = madd_parts(a, c, b, subtract)
    if special is not None: return flush(special)
    s, zero_neg = exact
    return flush(to_double(s, zero_negative=zero_neg))


def fmadds(a, c, b, subtract):
    cr = round25(c)
    if is_nan(cr) and not (is_nan(a) or is_nan(b) or is_nan(c)):
        special = None  # cannot happen: round25 keeps a non-NaN non-NaN
    special, exact = madd_parts(a, cr, b, subtract)
    if special is not None:
        if is_nan(special) and (is_nan(a) or is_nan(b) or is_nan(c) or is_nan(cr)):
            special = first_nan(a, b, c) or DNAN  # the original operands decide which NaN
        return single(special)
    s, zero_neg = exact
    as_double = to_double(s, zero_negative=zero_neg)
    if (as_double & ~SIGN) < 0x3810000000000000: return as_double & SIGN  # NI, judged on the double
    return to_single_value(s, zero_negative=zero_neg)


# ---------------------------------------------------------------- conversions
def fctiw(x, truncate):
    if is_nan(x): v = 0x80000000
    elif is_inf(x): v = 0x80000000 if neg(x) else 0x7FFFFFFF
    else:
        q = value(x)
        n = int(q) if truncate else round(q)  # Python's round(Fraction) is to nearest-even
        v = 0x7FFFFFFF if n > 0x7FFFFFFF else 0x80000000 if n < -0x80000000 else n & 0xFFFFFFFF
    out = 0xFFF8000000000000 | v
    if v == 0 and neg(x): out |= 1 << 32
    return out


def lfs(w):
    s, e, f = w >> 31, (w >> 23) & 0xFF, w & 0x7FFFFF
    if e == 255: return (s << 63) | EXP | (f << 29)
    v = Q(f, 1 << 149) if e == 0 else Q((1 << 23) | f) * Q(2) ** (e - 150)
    return encode_double(bool(s), v)


def bit_map32(x):
    return ((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF)


def stfs(x):
    e = (x >> 52) & 0x7FF
    s = (x >> 32) & 0x80000000
    if is_nan(x) or is_inf(x): return s | 0x7F800000 | ((x & FRAC) >> 29)
    if (x & ~SIGN) == 0: return s
    if e > 1150 or e < 874: return bit_map32(x)  # out of single range: what the hardware does
    a = abs(value(x))
    if e > 896:  # normal single: the significand truncated to 24 bits
        ee = ilog2(a)
        f = a / Q(2) ** ee * (1 << 23)
        return s | ((ee + 127) << 23) | (int(f) - (1 << 23))
    return s | int(a * 2 ** 149)  # single subnormal: truncated


# ---------------------------------------------------------------- quantisation
def signed_scale(sc): return sc if sc < 32 else sc - 64


def dequant(t, sc, raw):
    if t == 0: return lfs(raw)
    n = {4: raw & 0xFF, 5: raw & 0xFFFF, 6: (raw & 0xFF) - ((raw & 0x80) << 1),
         7: (raw & 0xFFFF) - ((raw & 0x8000) << 1)}[t]
    return encode_double(n < 0, abs(Q(n) * Q(2) ** -signed_scale(sc)))


def single_ieee(q):
    """An exact value to single precision with gradual underflow (C++'s float cast)."""
    return round_nearest_even(q, 24, -126, 127)


def quant(t, sc, x):
    if t == 0:  # stfs's conversion, single subnormals to signed zeros
        e = (x >> 52) & 0x7FF
        if e > 896 or (x & ~SIGN) == 0: return bit_map32(x) if not (is_nan(x) or is_inf(x)) or True else 0
        return (x >> 32) & 0x80000000
    if is_nan(x): return 0
    lo, hi = {4: (0, 255), 5: (0, 65535), 6: (-128, 127), 7: (-32768, 32767)}[t]
    if is_inf(x): v = hi if not neg(x) else lo
    else:
        f = single_ieee(value(x))
        if f == INF: v = hi if not neg(x) else lo
        else:
            f = -f if neg(x) else f
            g = single_ieee(f * Q(2) ** signed_scale(sc))
            g = (hi if f > 0 else lo) if g == INF else (-g if f < 0 else g)
            v = int(max(Q(lo), min(Q(hi), g)))  # int() truncates toward zero
    return v & (0xFF if t in (4, 6) else 0xFFFF)


# ---------------------------------------------------------------- estimates
def check_estimate(x, got, kind):
    """None if `got` is a valid fres/frsqrte result for x, else why not."""
    if kind == "fres":
        if is_nan(x): return None if got == quiet(x) else "NaN not kept"
        if is_inf(x): return None if got == x & SIGN else "1/inf isn't 0"
        if (x & ~SIGN) == 0: return None if got == (x & SIGN) | EXP else "1/0 isn't inf"
        e = (x >> 52) & 0x7FF
        if e >= 1149: return None if got == x & SIGN else "tiny result isn't 0"
        if e < 895: return None if got == (x & SIGN) | 0x47EFFFFFE0000000 else "huge result isn't FLT_MAX"
        exact = 1 / value(x)
    else:
        if is_nan(x): return None if got == quiet(x) else "NaN not kept"
        if (x & ~SIGN) == 0: return None if got == (x & SIGN) | EXP else "1/sqrt(0) isn't inf"
        if neg(x): return None if got == DNAN else "negative isn't NaN"
        if is_inf(x): return None if got == 0 else "1/sqrt(inf) isn't 0"
        v = value(x)
        # 1/sqrt(n/d) = sqrt(d*n)/n, from an integer square root 120 bits finer than needed
        import math
        exact = Q(math.isqrt(v.denominator * v.numerator * 4 ** 120), v.numerator * 2 ** 120)
    if is_nan(got) or is_inf(got): return "not finite"
    err = abs(value(got) - exact) / abs(exact)
    return None if err <= Q(1, 4096) else f"relative error {float(err):.3g} over 1/4096"


# ---------------------------------------------------------------- inputs
def edge_doubles():
    return [0, SIGN, 1, SIGN | 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x0010000000000001, 0x3810000000000000,
            0x380FFFFFFFFFFFFF, 0x36A0000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000,
            0x7FEFFFFFFFFFFFFF, 0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x47F0000000000000, EXP, SIGN | EXP,
            0x7FF0000000000001, 0x7FF7FFFFFFFFFFFF, DNAN, 0xFFF8000000000000, 0x7FFFFFFFFFFFFFFF, 0x41DFFFFFFFC00000,
            0x41E0000000000000, 0xC1E0000000000000, 0xC1E0000000200000, 0x3FE0000000000001, 0x4004000000000000]


def random_double(rng):
    pick = rng.random()
    if pick < 0.25: return rng.choice(edge_doubles())
    if pick < 0.55:  # a single-precision value, as the game mostly has
        return lfs(rng.getrandbits(32) & ~(0xFF << 23) | (rng.randrange(1, 255) << 23))
    if pick < 0.75:  # a modest double
        return (rng.getrandbits(1) << 63) | (rng.randrange(1023 - 40, 1023 + 40) << 52) | rng.getrandbits(52)
    return rng.getrandbits(64)


def tie_case(rng):
    """a, c, b for a single multiply-add whose double result sits exactly halfway
    between two singles while the exact result doesn't: the case the runtime's
    error-free correction exists for."""
    a = lfs((rng.randrange(100, 150) << 23) | rng.getrandbits(23))
    c = lfs((rng.randrange(100, 150) << 23) | rng.getrandbits(23))
    p = value(a) * value(round25(c))
    target = round_nearest_even(p, 24, -126, 127)
    half = Q(2) ** (ilog2(abs(target)) - 24)
    h = (target + half) * (1 if p > 0 else -1) if p > 0 else -(target + half)
    tiny = Q(rng.choice([1, -1]), 2 ** rng.randrange(60, 90)) * abs(h)
    b = to_double(h - p + tiny)
    return a, c, b


def vanishing_product_case(rng):
    """b exactly halfway between two singles, plus a product too small for any
    double: the exact sum is just off halfway, and only the product's sign says
    which way (found by this test: the runtime's error-free transformation
    can't see an error below the smallest subnormal)."""
    single = lfs((rng.randrange(1, 255) << 23) | rng.getrandbits(23) | (rng.getrandbits(1) << 31))
    half = Q(2) ** (ilog2(abs(value(single))) - 24)
    b = to_double(value(single) + (half if value(single) > 0 else -half))
    a = (rng.getrandbits(1) << 63) | (rng.randrange(1, 200) << 52) | rng.getrandbits(52)
    c = (rng.getrandbits(1) << 63) | rng.randrange(1, 1 << 20)  # subnormal
    return a, c, b


# Inputs that once disagreed: kept so they're always tested.
REGRESSIONS = [
    ("fmadds", 0x203A4A9B659A9F11, 0x8000000000000001, 0x47EFFFFFF0000000),
]


def cases(n, rng):
    out = []
    ops2 = ["fadd", "fsub", "fmul", "fdiv", "fadds", "fsubs", "fmuls", "fdivs"]
    for _ in range(n):
        op = rng.choice(ops2 + ["fmadd", "fmsub", "fmadds", "fmsubs", "frsp", "round25", "fctiw", "fctiwz", "lfs",
                                "stfs", "fres", "frsqrte", "dequant", "quant", "tie"])
        if op in ops2: out.append((op, random_double(rng), random_double(rng)))
        elif op in ("fmadd", "fmsub", "fmadds", "fmsubs"):
            out.append((op, random_double(rng), random_double(rng), random_double(rng)))
        elif op == "tie":
            a, c, b = tie_case(rng) if rng.random() < 0.8 else vanishing_product_case(rng)
            out.append(("fmadds", a, c, b))
        elif op == "lfs": out.append((op, rng.getrandbits(32) if rng.random() < 0.7 else rng.choice(
            [0, 0x80000000, 1, 0x7F800000, 0xFF800000, 0x7F800001, 0x7FC00000, 0x007FFFFF, 0x00800000])))
        elif op in ("dequant", "quant"):
            t = rng.choice([0, 4, 5, 6, 7])
            sc = rng.choice([0, 0, 1, 7, 8, 15, 31, 32, 33, 56, 63, rng.randrange(64)])
            third = rng.getrandbits(32) if op == "dequant" else random_double(rng)
            if op == "quant" and rng.random() < 0.5:  # values that land in range
                third = to_double(Q(rng.randrange(-70000, 70000), rng.choice([1, 2, 3, 7, 256])) / Q(2) ** signed_scale(sc))
            out.append((op, t, sc, third))
        else: out.append((op, random_double(rng)))
    return out


def expected(c):
    op = c[0]
    if op == "fadd": return flush(add_exact(c[1], c[2]))
    if op == "fsub": return flush(sub_exact(c[1], c[2]))
    if op == "fmul": return flush(mul_exact(c[1], c[2]))
    if op == "fdiv": return flush(div_exact(c[1], c[2]))
    if op == "fadds": return single(add_exact(c[1], c[2]))
    if op == "fsubs": return single(sub_exact(c[1], c[2]))
    if op == "fmuls":
        cr = round25(c[2])
        r = mul_exact(c[1], cr)
        return single(r)
    if op == "fdivs": return single(div_exact(c[1], c[2]))
    if op in ("fmadd", "fmsub"): return fmadd(c[1], c[2], c[3], op == "fmsub")
    if op in ("fmadds", "fmsubs"): return fmadds(c[1], c[2], c[3], op == "fmsubs")
    if op == "frsp": return single(c[1])
    if op == "round25": return round25(c[1])
    if op in ("fctiw", "fctiwz"): return fctiw(c[1], op == "fctiwz")
    if op == "lfs": return lfs(c[1])
    if op == "stfs": return stfs(c[1])
    if op == "dequant": return dequant(c[1], c[2], c[3])
    if op == "quant": return quant(c[1], c[2], c[3])
    return None  # estimates: checked by property


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fp_eval")
    ap.add_argument("--cases", type=int, default=40000)
    ap.add_argument("--seed", type=int, default=20261119)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    import json, pathlib
    vectors = json.loads((pathlib.Path(__file__).with_name("dolphin_frsqrte.json")).read_text())
    all_cases = [("frsqrte", x) for x in vectors["inputs"]] + REGRESSIONS + cases(args.cases, rng)
    text = "".join(c[0] + " " + " ".join(f"{v:x}" for v in c[1:]) + "\n" for c in all_cases)
    out = subprocess.run([args.fp_eval], input=text, capture_output=True, text=True, check=True).stdout.split()
    assert len(out) == len(all_cases)

    failures, counts = {}, {}
    for i, (c, got) in enumerate(zip(all_cases, out)):
        got = int(got, 16)
        op = c[0]
        counts[op] = counts.get(op, 0) + 1
        if i < len(vectors["inputs"]):
            want = vectors["frsqrte"][i]
            why = None if got == want else f"want {want:016x} (Wii)"
        elif op in ("fres", "frsqrte"):
            why = check_estimate(c[1], got, op)
        else:
            want = expected(c)
            why = None if got == want else f"want {want:016x}"
        if why:
            failures.setdefault(op, []).append(f"{op} {' '.join(f'{v:x}' for v in c[1:])} -> {got:016x}: {why}")

    print(f"{len(all_cases)} cases: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    if not failures:
        print("all agree with exact arithmetic")
        return 0
    for op, fs in sorted(failures.items()):
        print(f"FAIL {op}: {len(fs)} of {counts[op]}")
        for f in fs[:6]: print("  " + f)
    return 1


if __name__ == "__main__":
    sys.exit(main())
