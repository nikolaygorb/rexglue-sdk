# PPC test corpus

The PowerPC instruction tests of xenia-edge, `src/xenia/cpu/ppc/testing`: 581 `.s` files and about 1.7 million lines, most of them (`instr__gen_*.s`, labels ending `_GEN`) captured on Xbox 360 hardware by xenia-project's `gen_tests`. They stay in edge's tree; this folder holds only what tells the build how to use them.

```powershell
cmake --preset win-amd64-relwithdebinfo -DREXGLUE_PPC_CORPUS=ON -DREXGLUE_PPC_CORPUS_DIR=D:/dev/xenia/xenia-edge/src/xenia/cpu/ppc/testing
cmake --build --preset win-amd64-relwithdebinfo --target ppc_corpus_tests
ctest --preset win-amd64-relwithdebinfo -L ppc_corpus
```

`rexglue recompile-tests --table` generates the cases as data, run from one loop per file (`ppc_table_runner.h`), so the corpus compiles in a few minutes. Each file is one CTest test, `ppc_corpus.<file stem>`. A failing case prints one line naming its first wrong register or byte.

| File | Contents |
| --- | --- |
| `assembler_unsupported.txt` | Files the bundled binutils cannot assemble; left out. |
| `known_failures.txt` | Cases expected to fail, each with its cause. |

Edge's own `skip.txt`, captures it judges self-inconsistent, is read from its folder.

## Other vectors (xenia-canary)

xenia-canary keeps its own copy of the same files (`D:\dev\xenia\xenia-canary\src\xenia\cpu\ppc\testing`), compared 2026-11-12:

- Its `skip.txt` is empty: it runs the 919 captures edge skips (CR1 forms, fsqrt, fctiwz, fres, vrefp, vlogefp, vexptefp, vsl/vsr, vmaddfp and others), after fixing its JIT's semantics for them.
- Its estimate tests (`instr_fresx`, `instr_vrefp`, `instr_vrefp128`, `instr_vlogefp`, `instr_vlogefp128`, `instr_vexptefp128`) expect the hardware's estimates (fres(2) = `0x3FDFE00000000000`); edge's expect the exact value or xenia's own estimate. `instr__gen_fctiwz` expects the most negative value and VX where edge's capture (skipped) has 0.
- Its load tests (`instr_lbz`, `instr_lwz`, `instr_lxarx` and about 25 more) still expect the values its old runner read from misparsed `MEMORY_IN` brackets (`lbz` of `AB` gives 0); edge's are right. Don't take those.

Two settings run our recompiler against these:

| Setting | Effect |
| --- | --- |
| `-DREXGLUE_PPC_CORPUS_SKIP_LIST=none` | No skip list: edge's skipped captures run too. A failure there is either our bug or a bad capture; canary passing it says which. |
| `-DREXGLUE_PPC_CORPUS_OVERLAY_DIR=<folder>` | The `.s` files in the folder replace the corpus files of the same name (the corpus is copied into the build tree). Put canary's estimate tests and `instr__gen_fctiwz.s` there to check our estimates against the hardware's. |

After 0737, the overlay must also contain `instr_vmsum3fp128.s` and
`instr_vmsum4fp128.s` from our `tests/ppc/asm`: edge's handwritten cases still
expect round-to-nearest, while the console's fixed point sum truncates. The
hardware captures remain unchanged. After 0751 the three reservation failures
are fixed and must not stay in `known_failures.txt`. The remaining exceptions
are the inconsistent `instr__gen_vsr` captures. NI reads back 0 as on the
console (`instr_mffsx`, `instr_mtfsfix`) since NI is no longer modelled.

`REXGLUE_PPC_CORPUS_KNOWN_FAILURES` selects a different expected-failure file;
`REXGLUE_PPC_CORPUS_EXCLUDE_LIST` selects a file of unavailable file stems.
Defaults remain the two lists in this directory. This allows an independent
upstream corpus without inheriting edge's expected failures or excluding
upstream's assemblable `.long` tests for `vpkd3d128`.

U14 multiply-add hardware inputs are in
`tests/unit/ppc/upstream_cases/multiply_add_cases.h`, exercised by
`upstream_multiply_add_test.cpp` (0760, new execution pending). NJ-clear
multiply-add semantics and the existing non-FMA fallback are not certified by
this source-only pass. U23's separate 50-file input snapshot and maintainer
commands are in `memory/ppc-upstream-corpus/README.md`.

A known failure that passes fails its file's test, so a fix must remove its entries. To write the list, run `ppc_corpus_tests` with `PPC_CORPUS_FAILURES` set to a file path: every failing case is appended to it as `<file stem>/<label> <first mismatch>`.
