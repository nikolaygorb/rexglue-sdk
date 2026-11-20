# FPSCR NI, FR, FI and FPRF (ReXGlue fork). Xenon does not implement NI: a
# write is dropped, it reads back 0 and scalar code keeps denormals (the
# console's captures in edge's instr_mffsx / instr_mtfsfix, edge c5ae0a002).
# Without the fpscr_result_bits codegen option the arithmetic leaves FR, FI
# and FPRF alone, so they read back as written. Every case starts by clearing
# FPSCR with mtfsf.

# NI is bit 29: mtfsfi 7 writes RN, NI reads back 0.
test_fpscr_ni_roundtrip:
  #_ REGISTER_IN f0 0.0
  mtfsf 255, f0
  mtfsfi 7, 5
  mffs f1
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT f1 0xFFF8000000000001

# mtfsf drops NI as well.
test_fpscr_ni_mtfsf_dropped:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 0x0000000000000006
  mtfsf 255, f0
  mtfsf 0x01, f2
  mffs f1
  mtfsf 255, f0
  blr
  #_ REGISTER_OUT f1 0xFFF8000000000002

# With NI written a denormal result is still the denormal 2^-1050:
# 2^-1000 * 2^-50.
test_fpscr_ni_keeps_denormal:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f3 0x0170000000000000
  #_ REGISTER_IN f4 0x3CD0000000000000
  mtfsf 255, f0
  mtfsb1 29
  fmul f1, f3, f4
  mtfsb0 29
  mtfsf 255, f0
  blr
  #_ REGISTER_OUT f1 0x0000000001000000

# Without NI the same product is the denormal 2^-1050 (2^24 * 2^-1074).
test_fpscr_ni_off_keeps_denormal:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f3 0x0170000000000000
  #_ REGISTER_IN f4 0x3CD0000000000000
  mtfsf 255, f0
  fmul f1, f3, f4
  mtfsf 255, f0
  blr
  #_ REGISTER_OUT f1 0x0000000001000000

# Fields 3 and 4 hold FR, FI and FPRF (and VXVC, left clear here).
test_fpscr_result_bits_roundtrip:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 0x000000000007F000
  mtfsf 255, f0
  mtfsf 0x18, f2
  mffs f1
  mtfsf 255, f0
  blr
  #_ REGISTER_OUT f1 0xFFF800000007F000
