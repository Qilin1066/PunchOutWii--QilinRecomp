# Recompiler core: status (roadmap steps 1–2)

Last updated 2026-11-18 (v0.1.0: the core brought over from BeatTheBeat-QilinComp v0.4.0, where it was written; four new jump-table rules and a soundness fix for the switches Punch-Out!! has; everything run on the PAL `main.dol`).

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

### `qilin` CLI (`recomp/src/main.cpp`)
`info` (JSON), `insns` (every word of the text sections: address, word, text), `disasm` (a raw file of big-endian words, for the sweep), `profile` (instruction mix of the text sections, with paired-single, floating-point and privileged counts), `cfg` (summary, `--json` for every function), `coverage` (exit status 3 while any word is unknown) and `suspects` (with the rejected starts' addresses). Exit status is non-zero on any error.

## Next
1. Library identification (`roadmap-and-design-targets.md`, *Symbol naming*): the SDK, MSL, MetroTRK, HBM and Wwise ranges, then the lift / replace / drop boundary with every privileged instruction and unknown word on the replaced or dropped side.
2. The lifter's first slice (integer, with CR/XER elimination), the generated function table for indirect calls, and the differential test's reference executor.
