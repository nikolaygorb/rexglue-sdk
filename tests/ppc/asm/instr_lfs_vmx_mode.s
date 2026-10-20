# Scalar loads, moves and stores after VMX code. vaddfp leaves the host in the
# VMX flush mode (FTZ/DAZ); the bit-moving FP instructions no longer switch it
# back, so their results must not depend on it.

test_lfs_denormal_after_vmx:
  # the smallest single denormal widens to 2^-149, not zero
  #_ MEMORY_IN 10001000 00 00 00 01
  #_ REGISTER_IN r4 0x10001000
  #_ REGISTER_IN v3 [3F800000, 3F800000, 3F800000, 3F800000]
  vaddfp v3, v3, v3
  lfs f1, 0(r4)
  blr
  #_ REGISTER_OUT f1 0x36A0000000000000
  #_ REGISTER_OUT v3 [40000000, 40000000, 40000000, 40000000]

test_lfsx_negative_denormal_after_vmx:
  # the largest negative single denormal keeps its sign and every mantissa bit
  #_ MEMORY_IN 10001000 80 7f ff ff
  #_ REGISTER_IN r4 0x10001000
  #_ REGISTER_IN r5 0
  #_ REGISTER_IN v3 [3F800000, 3F800000, 3F800000, 3F800000]
  vaddfp v3, v3, v3
  lfsx f1, r4, r5
  blr
  #_ REGISTER_OUT f1 0xB80FFFFFC0000000

test_lfs_normal_after_vmx:
  #_ MEMORY_IN 10001000 3f 80 00 00
  #_ REGISTER_IN r4 0x10001000
  #_ REGISTER_IN v3 [3F800000, 3F800000, 3F800000, 3F800000]
  vaddfp v3, v3, v3
  lfs f1, 0(r4)
  blr
  #_ REGISTER_OUT f1 1.0

test_lfd_fmr_stfd_denormal_after_vmx:
  # a double denormal passes through lfd, fmr and stfd unchanged
  #_ MEMORY_IN 10001000 00 00 00 00 00 00 00 01
  #_ REGISTER_IN r4 0x10001000
  #_ REGISTER_IN v3 [3F800000, 3F800000, 3F800000, 3F800000]
  vaddfp v3, v3, v3
  lfd f1, 0(r4)
  fmr f2, f1
  stfd f2, 8(r4)
  blr
  #_ REGISTER_OUT f1 0x0000000000000001
  #_ REGISTER_OUT f2 0x0000000000000001
  #_ MEMORY_OUT 10001008 00000000 00000001
