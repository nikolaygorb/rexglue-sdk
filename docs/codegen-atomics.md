# Atomic sections without the global lock

Xbox 360 code wraps each `lwarx` / `stwcx.` (and `ldarx` / `stdcx.`) loop in an interrupts-off section: `mtmsrd r13, 1` before the load-reserve, `mtmsrd rN, 1` (rN from an earlier `mfmsr`) after the store-conditional. The generated code holds the global critical region, a recursive mutex the kernel exports share, for every such section. In games that count references or exchange values atomically all the time, that lock is taken hundreds of thousands of times a second (Forza Horizon 1: about 970,000).

The code generator checks each section before it emits the function. When every path from its `mtmsrd r13` to a closing `mtmsrd` is one load-reserve and at most one store-conditional (none on a compare-exchange's failed path) of the same width to the same address registers, which nothing between them writes, with only register arithmetic, compares and forward branches around them, the section is one atomic read-modify-write of one word. Its store-conditional uses the runtime reservation helper, so it runs without the global lock (`REX_ENTER_RESERVATION_SECTION`), as on the console, where the section only keeps out this CPU's interrupts. Anything else in the section (another load or store, a call, a return, an indirect branch, a trap, a barrier, `mfmsr`, a nested section, a loop), or a way into it other than its `mtmsrd r13` (a branch from outside, a jump-table entry, a mid-asm hook), keeps the lock.

`mfmsr` reads the count of locked sections the current thread is in, without taking the lock: 0x1030 (EE clear) inside one, 0x9030 outside, the console's MSR.

| Manifest key | Default | Effect |
|---|---|---|
| `lock_free_reservations` | `true` | `false` keeps the global lock for every section, as before. For comparison if a game misbehaves after a codegen with it. |

The codegen log says how many sections it found and how many run without the lock ("Interrupts-off sections: N, M of them lock-free"). A build with `-DREX_GUEST_LOCK_STATS` logs the totals of `mfmsr`, locked and lock-free sections every second.

The runtime reservation helper uses a host compare-exchange against other writers of the word. On x64 it first claims a 128-byte reservation granule, preventing another reservation store from changing A to B and back to A while a stale operation is preempted. A section that keeps the lock can now see a lock-free one change the word between its load-reserve and its store-conditional; its compare-exchange then fails and it retries, as a lost reservation would. The kernel exports that change guest words games also change atomically (spin locks, SLists) use host compare-exchanges, not the lock.

From ReIGlue 0470 / 0471 (Forza Horizon 1 ReImagined's SDK fork); the idea from Pinyon Shift's ShiftGlue.

## Reservation granules (0751, canary 28bc50db0 / 11689d217)

`lwarx` / `ldarx` save an address, width, raw value and generation in `PPCContext::reservation`.
`stwcx.` / `stdcx.` consume that reservation even on failure. On x64 a CAS changes its granule's even
generation to odd, the value CAS runs while it is claimed, and release advances the generation by two.
Generations are 64-bit in a shared runtime table (65536 buckets). Hash collisions cause spurious failures,
which guest retry loops must tolerate. Common physical aliases use the same bucket.

A watched-page store fault releases the claim **before** the memory handler takes the global region.
The handler resolves the watch and resumes at the helper's assembly failure continuation; it never
retries a CAS whose claim was released. A load seeing an odd generation waits only briefly and then
returns a value with an invalid reservation, keeping a suspended claimant from trapping it in a host loop.

This follows canary's x64 scope: ordinary guest stores and host atomic writers are checked by value,
not instrumented with generations, so A-B-A made entirely by those writes is still outside the model.
ARM64 retains value-CAS semantics pending its own fault-safe granule implementation.

The reservation state is appended after existing registers, preserving their offsets. The added field changes
the host `PPCContext` contract. Rebuild the SDK **and regenerate/rebuild every title**, including PCH, generated
sources and hooks. The snapshot lives in `ctx` even with `reserved_as_local`, so a generated function
boundary cannot hide an intervening reservation operation. Binding a guest thread clears its reservation.
No built-in shaders or shader caches change.
