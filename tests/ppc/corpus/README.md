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

A known failure that passes fails its file's test, so a fix must remove its entries. To write the list, run `ppc_corpus_tests` with `PPC_CORPUS_FAILURES` set to a file path: every failing case is appended to it as `<file stem>/<label> <first mismatch>`.
