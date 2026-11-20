# Runtime settings added in the 2026-11-17 evening ports

| Setting | Default | When it changes | Effect |
|---|---|---|---|
| `log_async` | `true` | startup | One background writer for the log file. Console logs stay synchronous. |
| `host_frame_limit` | `0` (off) | live | Cap host presents, 1–1000 FPS, by skipping them. Guest execution does not wait. |
| `gpu_device_lost_breadcrumbs` | `true` | restart | Vulkan work markers (xenos and native) and D3D12 DRED without the debug layer. |
| `texture_replacement_preload_memory_limit` | `256` MB | restart | CPU staging budget for packs with top-level `preload = true` in `pack.toml`; 0 disables it. |

`present_cas_additional_sharpness`, `present_fsr_sharpness_reduction`,
`present_nis_sharpness` and `present_dither` now apply while running.
Effect selection still follows its existing restart policy.

Replacement images leave CPU preload staging on first use. The existing
`texture_replacement_memory_limit` still bounds GPU replacement images.
Unused replacements are evicted after 30 seconds, once their GPU work completes.

Async file logging copies message text before returning. Under queue pressure
lower-priority lines can be dropped, with a count logged afterwards. Errors and
critical messages are retained. Critical lines, `FlushLogging()` and shutdown
wait up to one second for queued writes; a blocked disk can therefore lose the
tail at a hard process exit. `DrainLogging(timeout_ms)` reports whether that
explicit drain succeeded. `log_async = false` retains synchronous file writes.

XMA resets reuse open decoders through FFmpeg's flush callback. Creation stays
lazy; a used decoder retains about 450 KB until its context is destroyed.
The FFmpeg one-line dependency is in
`memory/thirdparty/0745-ffmpeg-xmaframes-flush.patch`; commit it in the FFmpeg
fork and update the SDK gitlink together with SDK patch 0745.

Regenerate title PCH and code for the guest spinlock global-region tracking
(0744) and reservation helpers (0751). Rebuild all SDK modules and title hooks
against the same headers. See [codegen-atomics.md](codegen-atomics.md) for the
x64 granule model, fault recovery and its ARM64/ordinary-store limits.

On Vulkan, `REX_TEST_GPU_BREADCRUMBS=nv` selects checkpoints when both marker
extensions are available. Unsupported devices disable markers. The existing
`REX_TEST_GPU_LOSS_AT_FRAME` hook is simulated and does not reset the driver;
it deliberately avoids NV checkpoint queries on a healthy device. A report's
first unconfirmed operation is a candidate, not proof of the faulting draw.
