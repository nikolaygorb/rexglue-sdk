test_vlogefp_1:
  # 1, 2, 4, 0.5: exact powers of two
  #_ REGISTER_IN v3 [3F800000, 40000000, 40800000, 3F000000]
  vlogefp v4, v3
  blr
  #_ REGISTER_OUT v3 [3F800000, 40000000, 40800000, 3F000000]
  #_ REGISTER_OUT v4 [00000000, 3F800000, 40000000, BF800000]

test_vlogefp_special:
  # 0 gives -inf, -1 the default NaN, +inf itself, a NaN quieted
  #_ REGISTER_IN v3 [00000000, BF800000, 7F800000, 7F800001]
  vlogefp v4, v3
  blr
  #_ REGISTER_OUT v4 [FF800000, 7FC00000, 7F800000, 7FC00001]

test_vlogefp_mixed:
  # One special lane: the others are unchanged
  #_ REGISTER_IN v3 [41000000, 00000000, 3E800000, 3F800000]
  vlogefp v4, v3
  blr
  #_ REGISTER_OUT v4 [40400000, FF800000, C0000000, 00000000]
