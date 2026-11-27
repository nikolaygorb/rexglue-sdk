# Vector dot products (vmsum3fp128, vmsum4fp128)

The Xbox 360 computes the VMX128 dot products in its own way: the products are exact, they are added as truncated fixed point with 29 fractional bits below the largest product, and the sum is normalized without rounding. A NaN operand, or an infinity times zero, gives the default quiet NaN (0x7FC00000) whatever the NaN's payload or sign; a sum that overflows can come back as an all-ones NaN pattern (2 * FLT_MAX gives 0x7FFFFFFF).

Generated code does the same by default (`rex::ppc::vec_msum3` / `vec_msum4`, ported from upstream ReXGlue, which matches all 747 of its console samples). The form Xenia uses, float64 sums rounded once to nearest, is cheaper (a handful of instructions against a few dozen; not measured in a title) but 1 ulp off the console in about three of four sampled cases, and propagates NaN payloads.

| Manifest key | Default | Effect |
|---|---|---|
| `fast_dot_products` | `false` | `true` emits the float64 form (`vec_msum3_fast` / `vec_msum4_fast`). For a title whose profile shows the dot products as a cost, after checking that nothing it computes depends on the last bit. The title regenerates. |

Denormal operands are zeros in both forms (VSCR[NJ] set, which titles run with).
