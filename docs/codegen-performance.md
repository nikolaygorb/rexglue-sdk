# Recompiled code: CPU performance options

Options that make the generated guest code cheaper on the host CPU. Most of them trade a Xenon detail for speed, so they are off by default and are turned on per title, after checking the title in play.

Instruction counts below are static counts of one FP-heavy translation unit of Perfect Dark Zero (`recomp.26.cpp`, clang `-O3 -march=x86-64-v2`), not frame times. Check a title with a profile before and after.

## Single-precision denormal operands

Xenon's single-precision arithmetic treats a denormalized double operand specially: `fadds`, `fsubs`, `fmuls` and the single multiply-adds answer with the default QNaN, while `fdivs` and `fsqrts` keep the double result without rounding it to single. Generated code checks every such instruction for it (`rex::ppc::fp_single_denormal_operand`).

| Manifest key | Default | Effect |
|---|---|---|
| `single_denormal_operand_quirk` | `true` | `false` drops the check: the instruction rounds its result to single as for any other operand. The title regenerates. |

- The check costs about 18 instructions per instruction in the static count (1,232 checks in the unit: 299,401 instructions with the check, 276,836 without, -7.5 %). Its hot path, with no denormal operand, is 9 instructions for two operands and 17 for three (x86-64-v2).
- Denormal doubles reach single-precision instructions rarely: a value computed in double precision and passed on without `frsp`. A title that never does this behaves the same either way. One that does gets an ordinary rounded result where the console gives a NaN, so check for picture or physics changes.

## Link register as 32 bits

`PPCContext::lr` holds 32 bits, the width of a guest code address, in an 8-byte slot (the fields after it keep their offsets). Each `bl` stores its return address as one 32-bit immediate store; with a 64-bit field an address above 0x80000000 took `movabs` and `mov`. `mflr` zero-extends, `mtlr` drops the register's upper half. Always on, nothing to set: -2.1 % instructions and -3.4 % memory operands in an integer-heavy unit (`recomp.12.cpp`: 241,117 -> 236,009), no change on ARM64. Titles rebuild; regenerating is needed only for the `mtlr` form (`.u32`), older generated code still compiles.

## Instruction set of the generated code (x86-64)

CMake cache variable of the title's build (from `generated/rexglue.cmake`; a title gets it when codegen refreshes that file):

| Variable | Default | Effect |
|---|---|---|
| `REXGLUE_RECOMP_ISA` | empty | `fma`: the generated code gets AVX and FMA (`fp_fma` inline instead of a library call). `x86-64-v3`: AVX2, FMA, BMI1/2, MOVBE, LZCNT, F16C. Only the generated code: the runtime and the GPU plugins keep the baseline. |

| Unit (static count) | baseline (v2) | `fma` | `x86-64-v3` |
|---|---|---|---|
| `recomp.12.cpp` (integer) | 236,009 | 226,510 (-4.0 %) | 218,341 (-7.5 %) |
| `recomp.26.cpp` (FP) | 297,392 | 279,056 (-6.2 %) | 271,950 (-8.6 %) |

- Most of v3's extra gain over `fma` is MOVBE (a byte-swapped load or store in one instruction; still two micro-ops on large Intel cores, so the cycle gain is smaller than the count).
- Such an executable needs that CPU (x86-64-v3: Intel Haswell, AMD Excavator / Zen and later). At start it checks (`rex/host_isa.h`) and shows a message instead of stopping at the first unknown instruction. Ship it as a second build beside the baseline one, for example in a second build directory: `cmake --preset <preset> -B out/build/v3 -DREXGLUE_RECOMP_ISA=x86-64-v3`.

## Profile-guided optimization (clang)

| Variable | Default | Effect |
|---|---|---|
| `REXGLUE_RECOMP_PGO` | empty | `generate`: the generated code is instrumented and the executable writes a profile when it exits (also on the hard exit when the window closes). A path to a `.profdata` file: the generated code is optimized with it (block layout, inlining, hot / cold splitting). |

Steps:

1. Configure a separate build directory with `-DREXGLUE_RECOMP_PGO=generate` and build it.
2. Play the title through its usual and heavy scenes. Set `LLVM_PROFILE_FILE=<dir>/title-%p.profraw` to keep one file per run (default: `default.profraw` in the working directory).
3. `llvm-profdata merge -output=title.profdata <dir>/*.profraw` (same LLVM version as the compiler).
4. Configure the release build with `-DREXGLUE_RECOMP_PGO=<path>/title.profdata` and build it.

A profile goes stale as the generated code changes: regenerate it after codegen or SDK changes that touch the generated code (clang warns about functions it no longer matches; those warnings are silenced, so check the profile's age). The instrumented build is several times slower; it is for profiling only.

## Unaligned vector stores (stvlx / stvrx)

`stvlx` and `stvrx` (and their `l` / `128` forms) write their part of the register with at most two overlapping stores (`rex::ppc::StoreVectorLeft` / `StoreVectorRight`), not a loop of up to 16 volatile byte stores. Like `stvx`, the stores are plain (not volatile). Always on; titles regenerate to get it. From upstream ReXGlue `55dd9b0b` (the store pair it also fuses into one 16-byte store is not taken: two plain stores each are close).

## Vector estimates (vrsqrtefp / vrefp / vlogefp / vexptefp)

`vrsqrtefp` and `vrefp` return the console's 12-bit table estimates (`rex/ppc/estimates.h`). When all four lanes are normal (positive for `vrsqrtefp`, biased exponent 1..252 for `vrefp`), `vec_estimate` computes them in SSE4.1 for the whole vector (one table read per lane, `pmaddwd` interpolation); any other lane takes the scalar per-lane path. Bit-exact: checked against the scalar code for all 2^32 inputs. Always on, header only (titles rebuild, no regeneration). Under `__AVX2__` the table reads use a gather; `REX_PPC_NO_GATHER` turns that off.

`vlogefp` (all lanes positive normals) and `vexptefp` (all lanes below 128 in magnitude with a result of at least 2^-126) take the same kind of whole-vector path: table reads per lane, the exponent and the fixed-point input in integer SSE (a variable shift as a multiply's high half), no host floating-point flag raised. Also checked for all 2^32 inputs, on x86-64-v2 and v3.

In Fuse, whose hottest vector job kernel runs four `vrsqrtefp` and four `vrefp` per record (~330,000 records a frame), the scalar estimates were ~44 ms of CPU a frame; with this path gameplay went from ~27 to ~40-45 FPS (x86-64-v2).

| Setting | Default | Effect |
|---|---|---|
| `vec_estimate_host` | `false` | `vrsqrtefp` = `1 / sqrt(x)`, `vrefp` = `1 / x` with SSE `sqrtps` / `divps`. Applies while running. **Not bit-exact**: a title that compares or accumulates estimates can diverge (xenia-canary saw spawning and collision differences in Red Dead Redemption). Fuse: another +15-20 % in its heaviest scenes. |

## Vector integer instructions without a lane loop

`vsubsws`, `vpkuhus` / `vpkuhus128`, `vpkuwus` / `vpkuwus128`, `vrlw` / `vrlw128`, `vsrw` / `vsrw128` and `vsraw` / `vsraw128` used to be generated as one statement per lane (4 to 16). They are now one SSE helper each (`rex::ppc::simde_mm_subs_epi32`, `simde_mm_packus_epu16` / `_epu32`, `simde_mm_rolv_epi32`, simde's `srlv` / `srav`, native only under AVX2, as `vslw` already was), with the same results (unit tests against scalar references, PPC asm tests). Titles regenerate to get it. As before, none of them sets VSCR[SAT].

## Vector registers as locals in leaf functions

`vmx_leaf_as_local` (manifest, default `false`; see [codegen-registers.md](codegen-registers.md)): a function with no call or branch out, no trap or `sc`, no scalar floating point and no `mtvscr` keeps the vector registers it uses in C++ locals, copied from `ctx` at entry and back at every return. In Fuse 820 functions qualified and the title ran correctly, but the hot vector kernel was not faster (~95 ns per record either way): its cost is in the exact per-operation helpers, not in the register file in memory. Kept as an option to measure on other titles.

## Finding the hot code: thread sampler

A wall-clock sampler of the title's threads, built into the runtime (Windows x64). Every `thread_sampler_interval_us` it suspends each selected thread, walks its host stack and resumes it; waiting threads are sampled too, so a report shows where a thread's frame time goes, not just its CPU use. At the end it names the frames from the PDBs next to the executable (inlined frames expanded). Build with symbols (`RelWithDebInfo`).

| Setting | Default | Effect |
|---|---|---|
| `thread_sampler_delay` | `0` | Capture once this many seconds after start (0: only on request). |
| `thread_sampler_request` | `0` | Change to a new nonzero number while running to capture now. |
| `thread_sampler_seconds` | `20` | Length of a capture. |
| `thread_sampler_interval_us` | `1000` | Time between sampling rounds. |
| `thread_sampler_threads` | empty | Threads whose name contains this. Empty: guest threads (`"<name> (XXXXXXXX)"`) and the thread calling `VdSwap`; `*`: every thread. |
| `thread_sampler_dir` | empty | Report folder; empty: `logs/profile` next to the executable. |

Example: `title.exe --frame_stats_interval=5 --thread_sampler_delay=90`.

Each capture writes `sampler-<UTC time>/`:

- `report.txt`: per thread its CPU % and the % of samples blocked in a syscall (`Nt*` / `Zw*` on top); for the `VdSwap` thread and the 6 busiest threads three tables in % and ms per guest frame (frames counted at `VdSwap`), with the blocked part: *Guest self* (innermost `sub_XXXXXXXX` on the stack), *Guest inclusive* (any `sub_` on the stack), *Leaf* (any function on top).
- `stacks.folded`: folded stacks of all sampled threads, for speedscope or `flamegraph.pl`.

The sampler costs some FPS while it captures (each thread is stopped about once per interval). A title whose renderer does not call `VdSwap` calls `rex::perf::NoteGuestSwap()` from its own present hook to get per-frame numbers.
