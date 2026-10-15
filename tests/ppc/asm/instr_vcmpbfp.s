test_vcmpbfp_1:
  # 1 in [-2, 2]; 3 above 2; -3 below -2; 0.5 in [-0.5, 0.5]
  #_ REGISTER_IN v3 [3F800000, 40400000, C0400000, 3F000000]
  #_ REGISTER_IN v4 [40000000, 40000000, 40000000, 3F000000]
  vcmpbfp v0, v3, v4
  blr
  #_ REGISTER_OUT v0 [00000000, 80000000, 40000000, 00000000]

test_vcmpbfp_nan:
  # A NaN in either operand sets both bounds bits
  #_ REGISTER_IN v3 [7FC00000, 3F800000, FF800001, 3F800000]
  #_ REGISTER_IN v4 [3F800000, 7FC00000, 3F800000, 3F800000]
  vcmpbfp v0, v3, v4
  blr
  #_ REGISTER_OUT v0 [C0000000, C0000000, C0000000, 00000000]

test_vcmpbfp_record_all_out:
  # Every element out of bounds: CR6 stays clear (no "all true" bit)
  #_ REGISTER_IN cr 0x00000000
  #_ REGISTER_IN v3 [40A00000, 40A00000, 40A00000, 40A00000]
  #_ REGISTER_IN v4 [3F800000, 3F800000, 3F800000, 3F800000]
  vcmpbfp. v0, v3, v4
  blr
  #_ REGISTER_OUT v0 [80000000, 80000000, 80000000, 80000000]
  #_ REGISTER_OUT cr 0x00000000

test_vcmpbfp_record_all_in:
  # Every element in bounds: CR6[2] set
  #_ REGISTER_IN cr 0x00000000
  #_ REGISTER_IN v3 [3F800000, 3F800000, 3F800000, 3F800000]
  #_ REGISTER_IN v4 [40000000, 40000000, 40000000, 40000000]
  vcmpbfp. v0, v3, v4
  blr
  #_ REGISTER_OUT v0 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_OUT cr 0x00000020
