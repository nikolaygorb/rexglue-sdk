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
