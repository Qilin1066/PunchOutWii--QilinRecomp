# Recompiler core: status (roadmap steps 1–2)

Last updated 2026-11-18 (v0.5.0: lifter slice 2, floating point, bit-exact with Broadway as Dolphin's hardware tests record it; v0.4.0: slice 1, integer code, with a differential test against an independent reference interpreter; v0.1.0: the core brought over from BeatTheBeat-QilinComp v0.4.0, four new jump-table rules and a soundness fix).

## Language and toolchain
Everything is **C++23**: the recompiler, the runtime and the generated code. The C++23 features in use so far are `std::expected` (the DOL loader), `std::print`/`std::format` (the tools), `std::ranges` algorithms and `std::string::contains`.

Minimum compilers are GCC 14, Clang 19, Xcode 16 and Visual Studio 2022 17.10. CMake checks these at configure time, and refuses 32-bit targets. Clang 18 is excluded because it can't use libstdc++'s `std::expected`.

Runtime code (what players run: the DOL loader and SHA-256 so far) avoids `std::format`, because the macOS runtime is built for 10.15 and Apple's libc++ only has it from 13.3. The developer tools are built for macOS 13.3.

CI builds with warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow`, MSVC `/W4`) and runs the tests on six configurations:
- Linux x64 with GCC 14 (plus the decoder sweep, below)
- Linux x64 with Clang 19
- Linux ARM64 with GCC 14
- macOS ARM64 and macOS x64 (Intel): the runtime built for macOS 10.15, the tools for 13.3
- Windows x64 with MSVC

## Done

### DOL loader (`recomp/src/dol.*`, `sha256.*`)
It parses the header's 7 text and 11 data slots, BSS and entry point, and builds the flat guest image from the lowest section to the end of the highest section or BSS. Sections inside the BSS range (the small-data sections a Wii linker puts there) keep their bytes; the rest of BSS is zero. `Dol::load`/`parse` return `std::expected<Dol, std::string>`: a malformed file is an error message, never a crash. It rejects:
- sections whose bytes lie outside the file;
- sections or BSS outside MEM1 (`0x80000000`–`0x81800000`);
- sections that overlap in memory;
- code that isn't word-aligned;
- an entry point that isn't an instruction in a text section.

The SHA-256 of the whole file identifies the build; `known_build()` lists the one this port supports. The SHA-256 is our own (FIPS 180-4), tested against the standard vectors and the padding edges.

### Decoder (`recomp/src/decoder.*`)
Our own, table-driven. One row per base instruction (**222**: the 750CL's integer, floating-point, load/store, branch, condition-register, system, cache and synchronisation instructions, and the paired singles), with its opcode, extended opcode, operand fields in objdump's order, and flags (record/overflow/link/absolute bits, branch, load, store, float, paired, privileged, cache, sync) for the analysis passes and the lifter. The decode tables are built once: one slot per primary opcode, and for the five primary opcodes with extended opcodes (4, 19, 31, 59, 63) a slot per value of the 10-bit extended-opcode field (bits 21–30), filled from the rows (10-bit, 9-bit XO, 6-bit and 5-bit A-form extended opcodes). Bit numbers in these docs are IBM's, from 0 at the most significant. Building the tables fails if two rows claim the same encoding.

**Every bit an instruction doesn't define must be zero**, so a word decodes as invalid when:
- a reserved bit is set (including the fields later Power ISA versions added: branch hints, `L` fields, `TH`, `EH`, …);
- it's an invalid form: a load with update whose base is 0 or its target; a store, floating-point or paired-single update with base 0; `lmw`/`lswi` with the base among the registers loaded; `lswx` with `rD` equal to `rA` or `rB`; a `bc`/`bclr`/`bcctr` with a non-zero "z" bit in BO; `bcctr` that decrements CTR; a compare with `L=1` (64-bit);
- it isn't a 750CL instruction (`fsqrt`, `fsqrts`, `tlbia`, the 603e's `tlbld`/`tlbli`, 64-bit and AltiVec instructions …).

**Cross-check against GNU binutils 2.42** (`tools/check/verify_decoder.py`, `objdump -M broadway,raw`): the text of every word, mnemonic and every operand, must match, and a word must be valid for both or neither.

| Set | Words | Result |
|---|---|---|
| Sweep of the encoding space, 8 seeds | ~38 million | **0 unexplained disagreements**; 4.15 million valid words per seed identical |
| Every word of the PAL `main.dol`'s text sections | 512,944 | **0 unexplained disagreements**; 505,952 valid and identical |

The sweep takes random words for every primary opcode and every extended-opcode value (each register field left zero half the time, so valid forms of instructions with reserved fields come up), then every single-bit neighbour of every word either side accepts, which tests the edge of each instruction's reserved bits. CI runs one seed (about 4.8 million words, 30 s).

The reviewed disagreements, all counted by class and none failing the check:
- **binutils leaves out `mftb`** in its `broadway` and `750cl` sets; the 750CL has it, and the SDK's `OSGetTime` uses it.
- **binutils accepts what the 750CL reserves:** later ISAs' fields (`bclr`/`bcctr` BH, `dcbt`/`dcbtst` TH, `lwarx` EH, `dcbf`/`sync`/`tlbie`/`mtmsr` L, `mtfsf` L and W, `sc` LEV, `mfocrf`/`mtocrf`), compare `L=1`, the reserved bit 9 of compares, bit 15 of `fres`/`frsqrte`, bit 31 of the paired-single indexed forms.
- **binutils accepts instructions the 750CL doesn't have:** `fsqrt`, `fsqrts`, `tlbia`, `tlbld`, `tlbli`.
- **binutils doesn't check some invalid forms:** paired-single updates with `rA=0`, `lswi` with `rA` among the registers loaded, `bcctr` that decrements CTR.
- **Formatting only:** objdump's raw mode prints `mfcr`'s absent field as `-1`. Elsewhere our text follows objdump's (`addi r3,0,0`, `bclr 20,lt,0`, `lwarx r3,r4,r5,0`, `psq_l f1,8(r0),0,0` although `rA=0` means 0 there).

**Unit tests** (`recomp/tests/tests.cpp`, no binutils or game files), for the decoder: 63 encodings pinned to objdump's exact text; 18 words that must not decode, each with its reason; for each of the 222 instructions, a canonical word that must decode to it and, for every bit it doesn't define, the same word with that bit set, which must not decode; field accessors (sign extension, branch targets, the paired-single fields) and mnemonic suffixes.

### Function discovery + CFG (`recomp/src/cfg.*`)
CodeWarrior makes this simpler than on x86: every instruction is one aligned word, switch tables and constants live in data sections, so text holds code and padding, and each function is one contiguous range. It runs in 1.9 s on the game.

1. **Strong seeds:** the entry point and every `bl` target from a linear sweep of the text. A `bl` to the next instruction is the get-PC idiom (`bl $+4; mflr`), not a call.
2. **Recursive descent**, each function bounded by the next known start. Calls continue; `blr`, `bctr`, `b`, `rfi` and an unconditional trap end a path; conditional returns and branches fork it. Execution that runs past the bound into the next function is recorded (`falls_into`): CodeWarrior's register save/restore routines are one routine with an entry point per register.
3. **Tail calls:** a `b` (or `bc`) past the bound to a *clean boundary* (a valid word that starts its section or follows code that can't fall through, or padding) becomes a start. So does one into another function's span when the branching function is strongly seeded: that span was explored before the start was known, and CodeWarrior never branches between functions except to an entry. `lis`/`addi` (or `ori`) pairs in code that build an address at a clean boundary are starts too (callbacks, thread entries).
4. **Jump tables**, CodeWarrior's pattern (`cmplwi`, `bgt`, `lis`, `slwi`, `addi`, `lwzx`, `mtctr`, `bctr`, in any order). The search walks back from the `bctr` on the straight line, follows the one branch into a block above an early return, and finds the table base in a data-pool register (`lis r18` early in the function, `addi r3, r18, n` at the switch: one writer, and for a volatile register no call after it; calls preserve r14–r31 under the EABI). **A table is proven** when a `cmplwi` on the index register, followed by `bgt` (or `bge`, or a conditional return) on that CR field, bounds it, nothing branches into the code between the check and the `bctr`, and every entry lands inside the function on a valid instruction; otherwise it's read while entries are plausible and reported. Every table in the game is proven. Punch-Out!! added four shapes (v0.1.0), each proven rather than guessed:
   - **The check after the shift.** The scheduler may put `cmplwi` below `slwi`, and reuse the index register for the table's address once it's been shifted. The check may sit on either side of the shift, as long as the index doesn't change between the two.
   - **A table base set at a loop's head.** The base register is written more than once in the function (`li r31, 0` early on, `lis r31` before the loop), so "one writer" fails. It's still constant at the switch if the last write before it heads a region that can only be entered through that write: the region grows to cover every branch back into it, a branch into it from above the head fails, and so does any other write to the register inside it.
   - **A strength-reduced index.** In a loop, CodeWarrior may index the table with a register that steps by 4 and check the bound on one that steps by 1 (`lwzx r3, r3, r19` with `cmplwi r24, 6`). Proven when the last writes of both before the switch zero them together (nothing can branch in between), that zeroing heads a region holding the switch that can only be entered through it, and inside the region the only writes to either are one step of each, again together. Then `r19 == 4 * r24` wherever the switch runs. The registers may be volatile if the region makes no calls.
   - The two region rules also require that **no word in data, other than the switch's own table, points into the region**, since another switch or a function pointer could enter it that way.

   **Soundness fix:** walking back for a register's definition, the search used to step straight up through a loop head, so it could return a `lis` even when the loop body rewrote the register. Now, when the walk crosses any word a branch lands on, the definition it finds must be the only one that reaches the use (the same region check). No table in Punch-Out!! changed; the case that showed it is in the tests.

5. **Weak seeds, in rounds until nothing changes:** pointers into text stored in data (vtables, `.ctors`, `extabindex`, callback tables), except words inside a jump table: every table a linear scan of the code can prove is excluded first, so a function reached only through a vtable doesn't have its case labels taken for functions. Nor is any case label of a table the scan proves, wherever a pointer to it turns up: in Punch-Out!!, a word of binary data in `.rodata` happens to equal a case label of the 416-case switch, and seeding it cut the switch's function short before its table could be proven (286 false functions followed). Then the first word of each run of unreached code that starts at a clean boundary and isn't inside a function's span (gaps). A weak seed must not fall inside another function's span.
6. **CodeWarrior's leftovers.** Checking our boundaries against the Rhythm Heaven Fever decomp's (in BeatTheBeat-QilinComp) found three habits, now handled. (a) After a function's final tail call (`b` or `bctr`), or a trap, CodeWarrior keeps the function's own `blr`; after a final call through `bctr` (the OS's `Run`), its whole epilogue (`addi r1, r1, n` … `blr`). That code is unreachable and belongs to the function: gap seeding steps over it and coverage counts it dead, unless something branches to it or points at it, in which case it's a function of its own (a lone `blr` reached through a vtable is an empty method). (b) A stack frame is only ever set up at a function's entry, so a branch to `stwu r1, -n(r1)` (possibly after `mflr r0`) at a clean boundary is a tail call to a new function, even inside the branching function's range: the Bluetooth stack's state machine reaches each state handler that way.
7. **Validation:** a weakly seeded function that reaches an invalid word or runs off its section is dropped and never re-seeded; a strongly seeded one is a suspect. Branches to targets that are neither inside the function nor a clean boundary are suspects too.

**Coverage** (`qilin coverage`) accounts for every word of the text: **code** (reached), **padding** (zero or `nop`, not reached), **dead** (not reached, inside a function, after code that can't fall through, and no branch in reached code nor any pointer in data targets any word of it) or **unknown**. Results on the game are in `dol-analysis.md`: 99.58% of `.text` is code, and the 101 unknown words are identified data and debugger-stub code that isn't lifted.

**Tests:** a synthetic program with a function found each way (entry, call, tail call, `lis`/`addi`, vtable, gap), a switch with its case after the default, a register-save chain, the get-PC idiom, an indirect tail call, a tail call into a frame set-up, the leftover `blr` and epilogue, padding and dead code, checked function by function and word by word; and a data pointer to code that runs into an invalid word, which validation must reject. For the Punch-Out!! rules: a stray data word equal to a case label, the check after the shift (and a variant where the index changes in between, which must stay unproven), a table base set at a loop's head (and a variant whose loop rewrites it), and a strength-reduced index (and a variant stepping by 8, which must not resolve).

### Lifter, slice 1: integer code (`recomp/src/lift.*`, v0.4.0)
One guest function becomes one C++ function, `void fn_<address>(Cpu& c)`, over the runtime's `Cpu` (`runtime/include/qilin/rt/cpu.hpp`).

- **Register model.** The GPRs, CR fields, LR, CTR and XER's SO, OV and CA that a function uses are host locals, loaded at entry. They're written back to the `Cpu` before anything outside the function can see them (a call, a return, a trap, an indirect jump), and loaded again after a call. CR fields are separate locals, so the C++ compiler drops any compare result nothing reads: that's the CR elimination the design asks for, done by the host compiler for now. A dataflow pass that also removes the write-backs nobody reads comes later, with measurements.
- **Control flow.** Branches inside the function are `goto`s to labels at their targets; proven jump tables are a `switch` on CTR whose default traps; a branch out of the function is a tail call (`fn_X(c); return;`); `bl` is a call; `bctrl`, `blrl` and an unresolved `bctr` go through the runtime's dispatcher (`hooks->call`), with the low two bits of the target cleared as the hardware does; falling into the next function calls it. The get-PC idiom only sets LR.
- **Memory.** Loads and stores go through `read8/16/32` and `write8/16/32`: cached MEM1 and MEM2 are host memory at `mem + address`, byte-swapped (one `bswap`/`rev`); anything at `0xC0000000` or above (hardware registers, the write-gather pipe, uncached mirrors, the locked cache) goes to the runtime's hooks, which is where the GX FIFO decoder will sit.
- **The slice:** integer arithmetic (with carries, `mulhw`, `divw` …), logic, rotates and shifts, compares, all the CR operations, `mfcr`/`mtcrf`/`mcrxr`, integer loads and stores (update, indexed, byte-reversed, `lmw`/`stmw`, `lwarx`/`stwcx.`), `dcbz` and the cache and ordering hints (no-ops on a coherent host), LR, CTR, XER and the GQRs, traps, and every branch. The game uses no overflow-enable (`o`) forms and no string loads or stores, so those stay out and fail loudly if met. `divw`'s undefined cases (by zero, INT_MIN / −1) give all ones for a negative dividend and zero otherwise, as Dolphin's interpreter does.
- **Anything outside the lifter** (in slice 1 floating point and paired singles; now only privileged and system instructions, FPSCR access and the locked cache) makes the function a stub that traps with the reason and address when called, so the whole program always links and nothing is skipped silently.

### Lifter, slice 2: floating point (v0.5.0)
Every FPR is two host locals when a function uses it: `f<n>`, the double the FP instructions work on (ps0), and `p<n>`, the second paired-single half (ps1), which Broadway writes too wherever a single-precision result is produced. Values live in host `double`s and move bit for bit; the arithmetic goes through `rt::fp` (`runtime/include/qilin/rt/cpu.hpp`), which reproduces what Broadway does, as Dolphin's interpreter records it from hardware tests:

- **The game's FPSCR, fixed:** `OSInit` calls `PPCSetFpNonIEEEMode`, so the whole game runs with NI set: a double result that comes out subnormal is a signed zero, and so is a single-precision result whose value before rounding is below the smallest normal single. Rounding is to nearest and every exception disabled. FPSCR's flags aren't kept: only the replaced OS reads FPSCR (`mffs` and `mtfsf` appear nowhere else), and no lifted code uses a record form (`fadd.`); both stay outside the slice and fail loudly.
- **NaNs:** a NaN result is the first NaN operand made quiet, in the order a, b, c (b before any negation), else the default NaN `0x7FF8000000000000`, not the host's (x86's is negative). The check costs one predictable branch on the result.
- **Single precision:** the double result rounded to single, and written to both halves. In `fmuls` and the single multiply-adds, frC is first rounded to 25 significant bits. The single multiply-adds round only once: a double `fma`, and where that lands exactly halfway between two singles, the `fma`'s own rounding error (found with an error-free transformation) decides the direction.
- **`fmadd` family:** `std::fma`, one rounding; `fnmadd`/`fnmsub` negate unless the result is a NaN. The host compiler is told never to fuse anything itself (`-ffp-contract=off`).
- **`fres` and `frsqrte`:** Broadway's estimate tables (32 base-and-decrement entries each), not the host's reciprocal.
- **`fctiw`/`fctiwz`:** the integer in the low word with `0xFFF80000` above it (and the next bit set for a negative value that converts to zero), saturating to `0x7FFFFFFF`/`0x80000000`, NaN to `0x80000000`.
- **`lfs`/`stfs`:** the manual's bitwise conversions, not the host's: no rounding, signalling NaNs kept, single subnormals handled as the hardware does. `lfs` fills both halves; `lfd` only ps0.

The rules and the tables come from Dolphin's interpreter (`Interpreter_FPUtils.h`, `Common/FloatUtils.cpp`; GPL-2.0-or-later). They're measurements of the hardware, and the code here is written from them, not copied (`prior-art.md` §8).

**Known answers** (`recomp/tests/tests.cpp`) pin the primitives: the 57 `frsqrte` results from Dolphin's hardware-derived unit tests, the `fmadds` case found in Mario Strikers Charged (hardware gives `0xBF55BF17`, a double fma rounded again gives `0xBF55BF18`, and both are checked), the bitwise single conversions, the 25-bit rounding, `fctiw`'s saturation and its negative-zero bit, NaN selection and the default NaN, and NI flushing. **The differential test** now also has every FP instruction in the slice (45, 24 encodings each), from random FPR states mixing infinities, both kinds of NaN with either sign, subnormals, the single-precision boundaries, values singles hold exactly and raw bit patterns, comparing both halves of every FPR bit for bit. The reference interpreter wires each instruction independently from the manual's operand fields (which register, which half, flushing, update forms) and shares only the `rt::fp` primitives the known answers pin down: 4,173 cases, 267,072 runs, 0 differences; planted wiring bugs (`fmuls` reading frB for frC, `lfd` filling both halves) fail at once.

### Lifter, slice 3: paired singles (v0.6.0)
Every paired-single instruction the game uses: each half is computed from the old register values, then both are written, so a destination that is also a source reads correctly.

- **Arithmetic** (`ps_add`, `ps_sub`, `ps_mul`, `ps_div`, the four multiply-adds, `ps_muls0/1`, `ps_madds0/1`): per half the same `rt::fp` rules as the single-precision scalar instructions (NaN selection, frC at 25 bits, one rounding, NI flushing). The `s0`/`s1` forms use one half of frC for both results.
- **`ps_sum0/1`:** one half is `a0 + b1` rounded to single, the other frC's half rounded to single.
- **Moves** (`ps_merge00/01/10/11`, `ps_mr`, `ps_neg`, `ps_abs`, `ps_nabs`): raw bit copies, sign bits only, no rounding. **`ps_sel`:** per half, `a >= -0.0 ? c : b`.
- **Estimates:** `ps_res` is `fres`'s table per half (not rounded again); `ps_rsqrte` is `frsqrte`'s rounded to single.
- **`ps_cmpu0/1`, `ps_cmpo0/1`:** compare one half into a CR field; FPSCR isn't kept, so the ordered and unordered forms agree.
- **`psq_l`/`psq_st` (all four forms each):** through `psq_load`/`psq_store` in the runtime, which read the GQR at run time (the game sets GQRs in code, so they can't be folded in at lift time). Types: float, u8, u16, s8, s16; the 6-bit scale is signed. Loads convert the integer to single and multiply by 2^-scale; float loads are `lfs`'s bitwise conversion; `W=1` loads one element and sets ps1 to 1.0. Stores multiply by 2^scale in single precision, clamp to the type's range and truncate toward zero; float stores flush single subnormals to signed zeros. An invalid type (1–3) traps with the address.

As before, the rules come from Dolphin's interpreter (`Interpreter_Paired.cpp`, `Interpreter_LoadStorePaired.cpp`). One case it leaves to the host: quantising a NaN to an integer type. Here that stores 0, which isn't verified on hardware. Left out: `dcbz_l` (the locked cache, used only by the replaced OS) and paired-single record forms (the game has none); both fail loudly.

**Known answers** for the conversions (types, signed scales, clamping, truncation, NaN, the float store's flushing), and **the differential test** now has all 44 paired-single instructions too, from random GQRs (a valid type in both halves 15 times in 16; invalid types must trap in both): 5,061 cases, 323,904 runs, 0 differences.

`qilin lift main.dol -o generated/` writes `functions.hpp`, `lifted_NNN.cpp` (400 functions each), `table.cpp` (every function by address, for the dispatcher) and `unlifted.txt`. On the game, with slices 1 to 3: **13,392 of 13,487 functions lifted (99.3%), 792,372 of 794,194 instructions (99.8%)** (slice 1 alone: 80.9% and 57.8%; with slice 2: 94.8% and 82.9%). The 95 stubs all use special-purpose registers, privileged or system instructions, the time base or the FPSCR, and every one is in code we replace or drop: 59 in the OS and CPU-register libraries, 36 in the debugger stub, the interrupt vectors and the debugger hooks. Every function in the libraries we lift is lifted. All 36 files compile with GCC 14 (`-O2`, about 5 minutes on two cores) and link into one program with no undefined function.

**Differential test** (`qilin_difftest`, in CI on every configuration, no game files). At build time `qilin_difftest_gen` makes 24 random valid encodings of each of the slice's 111 non-branch instructions (the decoder decides validity; memory operands are steered into a test window), and `bc`, `bclr` and `bcctr` programs over every BO the decoder accepts and a spread of BI, and lifts them: 3,093 cases. Each runs 64 times from random states (with the edge values 0, ±1, INT_MIN, INT_MAX … mixed in), lifted and on a reference interpreter (`recomp/tests/difftest/ref.cpp`) written separately from the manual's definitions: packed CR and XER, carries in 64 bits, memory byte by byte. GPRs, CR, XER, LR, CTR, GQRs, the reservation, traps, indirect calls and the whole memory window must agree: 197,952 runs, 0 differences, in 6 s. Planted bugs (`subf`'s operands swapped, `rlwimi`'s mask wrong, the CTR test inverted) each fail on the first trial.

### `qilin` CLI (`recomp/src/main.cpp`)
`info` (JSON), `insns` (every word of the text sections: address, word, text), `disasm` (a raw file of big-endian words, for the sweep), `profile` (instruction mix of the text sections, with paired-single, floating-point and privileged counts), `cfg` (summary, `--json` for every function), `coverage` (exit status 3 while any word is unknown), `suspects` (with the rejected starts' addresses) and `lift`. Exit status is non-zero on any error.

## Next
1. The runtime skeleton (roadmap step 3): load the DOL into the reserved address space, the dispatcher over `table.cpp`, and the first replaced OS functions.
