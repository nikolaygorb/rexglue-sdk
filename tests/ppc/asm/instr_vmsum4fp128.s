test_vmsum4fp128_1:
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [40000000, 40700000, 4013d70a, 40b051eb]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v4 [40000000, 40700000, 4013d70a, 40b051eb]
  # The console truncates the sum: 41A5147A (upstream rexglue dc3b7351), not
  # 41A5147B rounded to nearest
  #_ REGISTER_OUT v5 [41A5147A, 41A5147A, 41A5147A, 41A5147A]

test_vmsum4fp128_2:
  # Denormal output flushed to +0.0: FLT_MIN * 0.5 + 0 + 0 + 0 = denormal -> +0.0
  #_ REGISTER_IN v3 [00800000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v4 [3F000000, 00000000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [00000000, 00000000, 00000000, 00000000]

test_vmsum4fp128_3:
  # Negative denormal output flushed to -0.0: -FLT_MIN * 0.5 + 0 + 0 + 0 = neg denormal -> -0.0
  #_ REGISTER_IN v3 [80800000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v4 [3F000000, 00000000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [80000000, 80000000, 80000000, 80000000]

test_vmsum4fp128_4:
  # |(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX)|^2 overflows float32: QNaN, not infinity
  #_ REGISTER_IN v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  vmsum4fp128 v5, v3, v3
  blr
  #_ REGISTER_OUT v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_5:
  # an infinite input gives infinity
  #_ REGISTER_IN v3 [7f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [7f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v4 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v5 [7f800000, 7f800000, 7f800000, 7f800000]

test_vmsum4fp128_6:
  # 1e20 * 1e20 - 1e20 * 1e20: the products overflow float32 but cancel; the 1.0
  # is more than 29 bits below them, so the console's fixed point sum drops it: 0
  #_ REGISTER_IN v3 [60ad78ec, 00000000, 60ad78ec, 3f800000]
  #_ REGISTER_IN v4 [60ad78ec, 00000000, e0ad78ec, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [60ad78ec, 00000000, 60ad78ec, 3f800000]
  #_ REGISTER_OUT v4 [60ad78ec, 00000000, e0ad78ec, 3f800000]
  #_ REGISTER_OUT v5 [00000000, 00000000, 00000000, 00000000]

test_vmsum4fp128_7:
  # 1 + 2^-24 + 2^-24 + 0 = 1 + 2^-23: float64 sums keep both halves of the last bit, which
  # float32 steps (dpps: (w + z) + (y + x)) round away to 1.0
  #_ REGISTER_IN v3 [3f800000, 33800000, 33800000, 00000000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [3f800001, 3f800001, 3f800001, 3f800001]

test_vmsum4fp128_8:
  # a denormal input is zero: 1 + 0 * FLT_MAX, not 1 + 1.18e-38 * FLT_MAX
  #_ REGISTER_IN v3 [007fffff, 3f800000, 00000000, 00000000]
  #_ REGISTER_IN v4 [7f7fffff, 3f800000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [3f800000, 3f800000, 3f800000, 3f800000]

test_vmsum4fp128_9:
  # infinity times zero (invalid): Xenon's default QNaN, 7fc00000 (x86 makes ffc00000)
  #_ REGISTER_IN v3 [7f800000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_10:
  # infinity minus infinity (invalid): 7fc00000
  #_ REGISTER_IN v3 [7f800000, ff800000, 00000000, 00000000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_11:
  # Any NaN operand gives the default QNaN on the console, whatever its payload
  # or sign (upstream rexglue samples: SNaN 7F800001 * 0 -> 7FC00000)
  #_ REGISTER_IN v3 [7fc00001, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v4 [7fc00002, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_12:
  # Any NaN operand gives the default QNaN on the console, whatever its payload
  # or sign (upstream rexglue samples: SNaN 7F800001 * 0 -> 7FC00000)
  #_ REGISTER_IN v3 [3f800000, 3f800000, ffc00003, 3f800000]
  #_ REGISTER_IN v4 [7f800004, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_13:
  # an invalid x before a NaN in z: 7fc00000
  #_ REGISTER_IN v3 [7f800000, 3f800000, 7fc00005, 3f800000]
  #_ REGISTER_IN v4 [00000000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_14:
  # Any NaN operand gives the default QNaN on the console, whatever its payload
  # or sign (upstream rexglue samples: SNaN 7F800001 * 0 -> 7FC00000)
  #_ REGISTER_IN v3 [3f800000, 7fc00006, ffc00009, 3f800000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_15:
  # Any NaN operand gives the default QNaN on the console, whatever its payload
  # or sign (upstream rexglue samples: SNaN 7F800001 * 0 -> 7FC00000)
  #_ REGISTER_IN v3 [3f800000, 7fc00006, 3f800000, 3f800000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, ffc00007]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]
