# Sanitizer and fuzzing builds

## Options

| Option | Meaning |
|---|---|
| `REXGLUE_SANITIZERS` | Comma-separated clang sanitizers: `address`, `undefined` (for example `address,undefined`). Applies to all code, third-party included. |
| `REXGLUE_ENABLE_SANITIZERS` | Old switch, same as `REXGLUE_SANITIZERS=undefined`. |
| `REXGLUE_BUILD_FUZZERS` | Builds the libFuzzer targets in `tests/fuzz` and compiles everything with `-fsanitize=fuzzer-no-link`. |

Sanitizer and fuzzer builds write to their own output directory, `out/<platform>-asan`, `-ubsan`, `-asan-ubsan`, `-asan-fuzz` and so on, so they never replace the normal binaries in `out/<platform>`.

On Windows, `address` switches the CRT to `/MD` (the ASan runtime does not support the debug CRT) and copies `clang_rt.asan_dynamic-x86_64.dll` next to the executables at configure time. Use the RelWithDebInfo configuration.

## Limits

- **No leak detection on Windows.** LeakSanitizer exists only on Linux and macOS; use `linux-amd64-asan` for leaks.
- **Guest memory is not checked.** The guest heap is not `malloc`, so out-of-bounds accesses inside guest memory are not reported. Host code is checked: translators, caches, the file system, the kernel.
- **Fuzzer builds on Windows use the static CRT and have no container-overflow checks.** `clang_rt.fuzzer` is built with `/MT` and without the MSVC STL ASan annotations, so `REXGLUE_BUILD_FUZZERS` switches the whole build to `/MT` and turns the annotations off (`_DISABLE_STRING_ANNOTATION`, `_DISABLE_VECTOR_ANNOTATION`). Use that build only for the fuzzers, not for titles. The plain ASan build stays on `/MD` with the annotations.
- **Fuzzer coverage on Windows comes from the executable only.** libFuzzer does not collect the coverage counters of DLLs, so code in `rexruntime.dll` is fuzzed blind (the log shows a few dozen counters and `cov` stays in single digits). The fuzzers compile the code under test into the executable instead (`tests/fuzz/CMakeLists.txt`).
- **Fibers.** Guest threads switch host fibers; ASan has no fiber-switch annotations yet, so stack reports around fiber switches may be false positives.

## Unit tests under ASan (Windows)

```
cmake --preset win-amd64-asan -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D11=ON
cmake --build --preset win-amd64-asan --target rexglue rexgpu-xenos rexruntime unit_tests ppc_tests
ctest --preset win-amd64-asan -L unit --output-on-failure
ctest --preset win-amd64-asan -L ppc --output-on-failure
```

## Leaks (Linux, ASan + LSan + UBSan)

```
cmake --preset linux-amd64-asan
cmake --build --preset linux-amd64-asan --target rexruntime unit_tests ppc_tests
ctest --preset linux-amd64-asan -L unit --output-on-failure
```

## Fuzzing (Windows)

```
cmake --preset win-amd64-fuzz
cmake --build --preset win-amd64-fuzz --target lzx_fuzzer shader_dxbc_fuzzer shader_spirv_fuzzer texture_layout_fuzzer resolve_fuzzer shader_storage_fuzzer primitive_processor_fuzzer xma_fuzzer xdbf_fuzzer stfs_fuzzer disc_image_fuzzer xex_fuzzer
```

Targets:

| Target | Input |
|---|---|
| `lzx_fuzzer` | Byte 0: window size; bytes 1-2: output size; the rest: an LZX stream, as in a compressed XEX. |
| `shader_dxbc_fuzzer` | An 8-byte header (shader type, translator options, the shader modification the pipeline caches can create, Direct3D 11 device features; layout in the source), then Xenos microcode (big-endian). Runs `AnalyzeUcode`, the DXBC translator (Direct3D 12) and, when the header asks for it, the conversion to Shader Model 5.0 (Direct3D 11). |
| `shader_spirv_fuzzer` | The same layout, with the Vulkan meaning of the header bits (fragment shader interlock instead of ROV, device features). Runs `AnalyzeUcode` and the SPIR-V translator. The preset enables `REXGLUE_USE_VULKAN`, which it needs. |
| `texture_layout_fuzzer` | A texture fetch constant (6 dwords), then guest memory for the base level. Runs the texture cache's key, the guest layout and the texture replacement key. Checks that the upload shaders read only inside the layout's extents. |
| `resolve_fuzzer` | A header byte (resolution scale, EDRAM size), the vertex endianness, the resolve rectangle, then the resolve registers, each XOR a 1280x720 color resolve. Runs `draw_util::GetResolveInfo` and the copy and clear setup. Checks that the copy writes only inside the destination extent. |
| `shader_storage_fuzzer` | A flags byte, the length of the `.xsh` part, the `.xsh` part, then the `.xpso` part (the fuzzer can fill in the right hashes and file headers). Loads the storage twice: the cut files must load the same. |
| `primitive_processor_fuzzer` | A byte of host capabilities (32-bit indices, fans, loops, quads, point and rectangle expansion, strip cuts at all-ones as in Direct3D 11), the draw registers XOR an 8-index triangle strip with primitive reset, then the index buffer. Runs `PrimitiveProcessor::Process`. Checks that the guest's indices are read only inside the draw, converted index buffers are written only inside what was requested, the host index buffer holds what the host draws, and the host draws the same points, lines and triangles as the guest. |
| `xma_fuzzer` | A flags byte (kicks, what the title does between them), the XMA context as in guest memory XOR one that decodes 2 packets, then the packets of both input buffers. Runs `XmaContext::Work` and FFmpeg's decoder. The input and output buffers end at inaccessible pages, so reading or writing past them crashes. |
| `xdbf_fuzzer` | The user language, then the title's XDBF resource. Reads the title, the achievements, their strings and icons as the kernel, `rexglue init` and the overlay do. Checks that every block lies inside the resource. |
| `stfs_fuzzer` | A flags byte, the volume descriptor, `header_size`, the data file count, then the data after the package header (the fuzzer builds the rest of the 0x971A-byte header) or the data fragments. Mounts the package from a file, walks the tree and reads the files, as a title does. |
| `xex_fuzzer` | A XEX file. Runs `XexModule::Load` and `LoadContinue` on real guest memory (headers, image decryption and decompression, PE headers), then `Unload`. Import libraries are left out: the fuzzer renames their header, since resolving them needs a running kernel. |
| `disc_image_fuzzer` | A byte for the game partition offset, then the image from the root directory fields of the volume descriptor on (the magic is put in place). Mounts the image from a file, walks the tree, reads and maps the files. The file is padded to a whole page, so reading past the end of the mapping crashes instead of reading zeros. |

Those with checks of their own abort with a line naming the broken invariant (`texture_layout_fuzzer: ...`) when a check fails; ASan prints the stack. The texture address code (`src/graphics/pipeline/texture/util.cpp`) is built with traps on integer overflow in the fuzzer build, reported as an illegal instruction. An access to the inaccessible pages around guest buffers is reported by ASan as a SEGV on an unknown address.

Seed corpus for the shader fuzzers (both take the same inputs; give each its own corpus directory) from the titles' shader storage (`<cache>/shaders/shareable/<title_id>.xsh`, written while a title runs), from directories written with `dump_shaders=<dir>`, or both. The same shader is written once, so it can be rerun as the caches grow:

```
python -I tools/fuzz/make_shader_corpus.py ~/Documents/*/cache/shaders/shareable/*.xsh out/fuzz/corpus/shader_dxbc
python -I tools/fuzz/make_shader_corpus.py <dump_dir> out/fuzz/corpus/shader_dxbc
python -I tools/fuzz/make_storage_corpus.py ~/Documents/*/cache/shaders/shareable/*.xsh out/fuzz/corpus/shader_storage
```

`texture_layout_fuzzer` and `resolve_fuzzer` start from an empty corpus. The primitive processor, XMA, XDBF, STFS and disc image fuzzers start from well-formed inputs written by a script (the XMA frames' bits are random, so FFmpeg rejects most of the audio):

```
python -I tools/fuzz/make_synthetic_corpus.py primitive_processor out/fuzz/corpus/primitive_processor
python -I tools/fuzz/make_synthetic_corpus.py xma out/fuzz/corpus/xma
python -I tools/fuzz/make_synthetic_corpus.py xdbf out/fuzz/corpus/xdbf
python -I tools/fuzz/make_synthetic_corpus.py stfs out/fuzz/corpus/stfs
python -I tools/fuzz/make_synthetic_corpus.py disc_image out/fuzz/corpus/disc_image
```

`lzx_fuzzer` and `xex_fuzzer` take seeds from real XEX files (only normally compressed ones have an LZX stream; others are skipped with a note). The inputs hold game data, so keep the corpus out of the repository:

```
python -I tools/fuzz/make_xex_corpus.py lzx out/fuzz/corpus/lzx <file.xex>...
python -I tools/fuzz/make_xex_corpus.py xex out/fuzz/corpus/xex <file.xex>...
```

A process that ends without a report and without an artifact (with `-fork`, a crash counted on a job whose line says `exec/s: 0`) can't be replayed from libFuzzer's output. `stfs_fuzzer`, `disc_image_fuzzer`, `xex_fuzzer`, `lzx_fuzzer` and `shader_dxbc_fuzzer` write each input to the file named by `REX_FUZZ_LAST_INPUT` before running it (one process only, not `-fork`).

Run from the repository root. `-fork` keeps going after a crash, `-max_total_time` is in seconds, `-artifact_prefix` is where crashes go (the directory must exist, keep the trailing slash):

```
mkdir -p out/fuzz/corpus/lzx out/fuzz/corpus/shader_dxbc out/fuzz/corpus/shader_spirv out/fuzz/artifacts/lzx out/fuzz/artifacts/shader_dxbc out/fuzz/artifacts/shader_spirv
out/win-amd64-asan-fuzz/RelWithDebInfo/shader_dxbc_fuzzer.exe out/fuzz/corpus/shader_dxbc -max_len=49160 -fork=8 -ignore_crashes=1 -max_total_time=3600 -artifact_prefix=out/fuzz/artifacts/shader_dxbc/ 2>&1 | tee out/fuzz/shader_dxbc.log
out/win-amd64-asan-fuzz/RelWithDebInfo/shader_spirv_fuzzer.exe out/fuzz/corpus/shader_spirv -max_len=49160 -fork=8 -ignore_crashes=1 -max_total_time=3600 -artifact_prefix=out/fuzz/artifacts/shader_spirv/ 2>&1 | tee out/fuzz/shader_spirv.log
out/win-amd64-asan-fuzz/RelWithDebInfo/lzx_fuzzer.exe out/fuzz/corpus/lzx -max_total_time=3600 -artifact_prefix=out/fuzz/artifacts/lzx/ 2>&1 | tee out/fuzz/lzx.log
mkdir -p out/fuzz/corpus/texture_layout out/fuzz/corpus/resolve out/fuzz/corpus/shader_storage out/fuzz/artifacts/texture_layout out/fuzz/artifacts/resolve out/fuzz/artifacts/shader_storage
out/win-amd64-asan-fuzz/RelWithDebInfo/texture_layout_fuzzer.exe out/fuzz/corpus/texture_layout -max_len=4096 -fork=8 -ignore_crashes=1 -max_total_time=1800 -artifact_prefix=out/fuzz/artifacts/texture_layout/ 2>&1 | tee out/fuzz/texture_layout.log
out/win-amd64-asan-fuzz/RelWithDebInfo/resolve_fuzzer.exe out/fuzz/corpus/resolve -max_len=256 -fork=8 -ignore_crashes=1 -max_total_time=1800 -artifact_prefix=out/fuzz/artifacts/resolve/ 2>&1 | tee out/fuzz/resolve.log
out/win-amd64-asan-fuzz/RelWithDebInfo/shader_storage_fuzzer.exe out/fuzz/corpus/shader_storage -max_len=65536 -fork=4 -ignore_crashes=1 -max_total_time=1800 -artifact_prefix=out/fuzz/artifacts/shader_storage/ 2>&1 | tee out/fuzz/shader_storage.log
mkdir -p out/fuzz/artifacts/primitive_processor out/fuzz/artifacts/xma out/fuzz/artifacts/xdbf
out/win-amd64-asan-fuzz/RelWithDebInfo/primitive_processor_fuzzer.exe out/fuzz/corpus/primitive_processor -max_len=4096 -fork=8 -ignore_crashes=1 -max_total_time=1800 -artifact_prefix=out/fuzz/artifacts/primitive_processor/ 2>&1 | tee out/fuzz/primitive_processor.log
out/win-amd64-asan-fuzz/RelWithDebInfo/xma_fuzzer.exe out/fuzz/corpus/xma -max_len=16449 -fork=8 -ignore_crashes=1 -max_total_time=1800 -artifact_prefix=out/fuzz/artifacts/xma/ 2>&1 | tee out/fuzz/xma.log
out/win-amd64-asan-fuzz/RelWithDebInfo/xdbf_fuzzer.exe out/fuzz/corpus/xdbf -max_len=4096 -fork=8 -ignore_crashes=1 -max_total_time=1800 -artifact_prefix=out/fuzz/artifacts/xdbf/ 2>&1 | tee out/fuzz/xdbf.log
```

`shader_storage_fuzzer` writes its files under the temporary directory (`rexglue_shader_storage_fuzzer_<pid>`, removed at exit); `-fork=4` because each run is file I/O. Out-of-memory artifacts are `oom-<sha1>` (libFuzzer's limit is 2 GB per allocation and process).

Outputs:

| What | Where |
|---|---|
| New inputs that reach new code | Added to the corpus directory (the first argument); the next run continues from them. |
| Crashes, timeouts, out-of-memory | `crash-<sha1>`, `timeout-<sha1>`, `oom-<sha1>`, `slow-unit-<sha1>` under `-artifact_prefix`, or in the current directory without it. |
| The log and ASan reports | stderr only; the `tee` above keeps a copy (Windows PowerShell's `tee` writes UTF-16; run the commands from Git Bash, or use `\| Out-File -Encoding utf8`, for a UTF-8 log). With `-fork`, the jobs' own logs are in a temporary directory that is deleted at the end; the crash reports also go to the main stderr. |

Replay an artifact (the ASan report with the stack prints again):

```
out/win-amd64-asan-fuzz/RelWithDebInfo/shader_dxbc_fuzzer.exe out/fuzz/artifacts/shader_dxbc/crash-<sha1>
```
