# FPSCR NI, FR, FI and FPRF (ReXGlue fork). Without the fpscr_result_bits
# codegen option the arithmetic leaves FR, FI and FPRF alone, so they read
# back as written. Every case starts by clearing FPSCR with mtfsf.

# NI is bit 29: mtfsfi 7 writes it with RN.
test_fpscr_ni_roundtrip:
  #_ REGISTER_IN f0 0.0
  mtfsf 255, f0
  mtfsfi 7, 4
  mffs f1
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT f1 0x0000000000000004

# With NI set a denormal result is flushed to zero: 2^-1000 * 2^-50.
test_fpscr_ni_flushes_denormal:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f3 0x0170000000000000
  #_ REGISTER_IN f4 0x3CD0000000000000
  mtfsf 255, f0
  mtfsb1 29
  fmul f1, f3, f4
  mtfsb0 29
  mtfsf 255, f0
  blr
  #_ REGISTER_OUT f1 0x0000000000000000

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
  #_ REGISTER_OUT f1 0x000000000007F000
