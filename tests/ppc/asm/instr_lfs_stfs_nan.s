# lfs and stfs move a NaN bit for bit: a signalling NaN stays signalling, so a
# 32-bit value copied through a float register keeps its bits (canary 9588ce244).

test_lfs_stfs_signalling_nan:
  #_ MEMORY_IN 10001000 7f 80 00 01
  #_ REGISTER_IN r4 0x10001000
  lfs f1, 0(r4)
  stfs f1, 4(r4)
  blr
  #_ REGISTER_OUT f1 0x7FF0000020000000
  #_ MEMORY_OUT 10001004 7F800001

test_lfsx_stfsx_negative_signalling_nan:
  #_ MEMORY_IN 10001000 ff a0 00 05
  #_ REGISTER_IN r4 0x10001000
  #_ REGISTER_IN r5 0
  #_ REGISTER_IN r6 8
  lfsx f1, r4, r5
  stfsx f1, r4, r6
  blr
  #_ REGISTER_OUT f1 0xFFF40000A0000000
  #_ MEMORY_OUT 10001008 FFA00005

test_lfs_stfsu_largest_signalling_nan:
  #_ MEMORY_IN 10001000 7f bf ff ff
  #_ REGISTER_IN r4 0x10001000
  lfs f1, 0(r4)
  stfsu f1, 4(r4)
  blr
  #_ REGISTER_OUT f1 0x7FF7FFFFE0000000
  #_ REGISTER_OUT r4 0x10001004
  #_ MEMORY_OUT 10001004 7FBFFFFF

test_lfs_stfs_quiet_nan:
  #_ MEMORY_IN 10001000 7f c0 00 01
  #_ REGISTER_IN r4 0x10001000
  lfs f1, 0(r4)
  stfs f1, 4(r4)
  blr
  #_ REGISTER_OUT f1 0x7FF8000020000000
  #_ MEMORY_OUT 10001004 7FC00001

test_stfs_double_signalling_nan:
  # a signalling double narrows to the signalling single with its high bits
  #_ MEMORY_IN 10001000 7f f0 00 00 20 00 00 00
  #_ REGISTER_IN r4 0x10001000
  lfd f1, 0(r4)
  stfs f1, 8(r4)
  blr
  #_ REGISTER_OUT f1 0x7FF0000020000000
  #_ MEMORY_OUT 10001008 7F800001
