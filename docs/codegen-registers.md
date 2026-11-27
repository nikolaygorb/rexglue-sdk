# Guest registers as C++ locals

Generated code keeps the guest registers in the `PPCContext` (`ctx.r3`, `ctx.f1`, ...). Every access is a load or store the C++ compiler has to keep, because a callee or the runtime may look at `ctx`. The manifest options below turn some registers into locals of each generated function instead, which the compiler keeps in host registers. On Perfect Dark Zero (one source file, clang -O3) the default set below cuts the instructions by 5% and the memory operands by 13%.

## Options

| Manifest key | Registers made local | Notes |
|---|---|---|
| `non_volatile_gpr_as_local` | r14-r31 | Recommended. f14-f31 and the VMX non-volatiles stay in ctx (floating-point ordering against FPSCR mode changes). |
| `non_argument_gpr_as_local` | r0, r2, r11, r12 | Recommended with the above. f0 and v32-v63 stay in ctx. |
| `non_volatile_as_local` | r14-r31, f14-f31, v14-v31, v64-v127 | Older and wider form of `non_volatile_gpr_as_local`. |
| `non_argument_as_local` | r0, r2, r11, r12, f0, v32-v63 | Older and wider form of `non_argument_gpr_as_local`. |
| `auto_share_registers` | none | Default `true`. Finds the functions that need `share_registers` (below). Set it to `false` only to compare against a manual list. |

The projects created by `rexglue init` build Windows targets with `-fno-ms-volatile` (clang) or `/volatile:iso` (MSVC). Guest memory is accessed through volatile pointers, and Microsoft's volatile semantics make each access a barrier that forces every register back into `ctx`. The locals pay off only with ISO semantics.

## When a function keeps ctx

A local starts at zero in every function. Code that hands a register to a callee outside the calling convention therefore needs that callee to work on `ctx`:

- SEH and C++ EH funclets read the owner's frame in r12.
- `__chkstk` reads the frame size in r12.
- `__savefpr_N`, `__restfpr_N`, `__savevmx_N` and `__restvmx_N` read the save area in r12 (or r11).
- A tail fragment reads the value its caller computed in r11.
- A function that starts in the middle of another one's body reads what the code before it left.

A function marked `share_registers` keeps its registers in `ctx`. A localizing caller copies its locals into `ctx` before calling it, and copies them back afterwards. For r14-r31 the caller also restores what `ctx` held before the call, so its own caller's values survive.

These functions get `share_registers` without any manifest entry:

- the SEH and C++ EH funclets named by the exception tables;
- chunks;
- the save and restore helpers;
- the bodies of setjmp and longjmp and every function calling setjmp (a host longjmp does not restore C++ locals);
- every function with SEH scopes when `generate_exception_handlers` is on;
- with `auto_share_registers`, every function that reads a localized GPR before any path of its own sets it, directly or through a callee.

The last check follows each function the way the code generator emits it: its branches, jump tables and mid-asm hooks. A mid-asm hook's register arguments count as written, since the hook gets them by reference. A register set in one branch counts as set after the join. A register set inside a loop does not count at the loop's head, because the first pass comes from before the loop. This matters for a function whose entry is a loop head, entered by a branch from another function with the running value in r11. The codegen log lists the functions found at debug level and a one-line summary at info level.

A manifest entry still works for anything the analysis cannot see:

```toml
[functions]
0x82A9AD34 = { share_registers = true }  # reads the caller's r12
```

## Limits

- **Only GPRs are checked.** With `non_volatile_as_local` or `non_argument_as_local`, a function reading f14-f31, f0 or a VMX register its caller set still needs a manual `share_registers` entry. The `_gpr_` options leave those registers in `ctx` and need nothing.
- **Indirect calls are not followed.** A function reached only through a pointer is checked on its own: it is flagged if it reads a localized register before setting it, but nothing is synced into `ctx` for it at the call through the pointer.
- **Values returned in volatile registers are not passed back.** A callee that returns something in r11 or r12, outside the ABI, to a localizing caller would lose it. None is known in the titles checked so far.
