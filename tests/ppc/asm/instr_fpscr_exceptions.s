# FPSCR exception bits (ReXGlue fork): VX, OX, UX, ZX, XX from the host's
# sticky status flags, FX set when one goes from 0 to 1; an invalid operation
# reads as VX and VXSOFT. mffs reads the upper word as 0xFFF80000. Every case
# starts by clearing FPSCR with mtfsf.

test_fpscr_zero_divide:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 0.0
  mtfsf 255, f0
  fdiv f4, f2, f3
  mffs f1
  blr
  #_ REGISTER_OUT f1 0xFFF8000084000000

test_fpscr_invalid:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 0.0
  #_ REGISTER_IN f3 0.0
  mtfsf 255, f0
  fdiv f4, f2, f3
  mffs f1
  blr
  #_ REGISTER_OUT f1 0xFFF80000A0000400

test_fpscr_overflow:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 0x7FE0000000000000
  #_ REGISTER_IN f3 0x7FE0000000000000
  mtfsf 255, f0
  fmul f4, f2, f3
  mffs f1
  blr
  #_ REGISTER_OUT f1 0xFFF8000092000000

# mtfsb0 on XX clears it; FX stays, it is sticky.
test_fpscr_mtfsb0:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 3.0
  mtfsf 255, f0
  fdiv f4, f2, f3
  mtfsb0 6
  mffs f1
  blr
  #_ REGISTER_OUT f1 0xFFF8000080000000

test_fpscr_mtfsfi:
  #_ REGISTER_IN f0 0.0
  mtfsf 255, f0
  mtfsfi 1, 2
  mffs f1
  blr
  #_ REGISTER_OUT f1 0xFFF8000082000000

# The record form copies FX, FEX, VX, OX into CR1.
test_fpscr_record_cr1:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 0.0
  mtfsf 255, f0
  fdiv. f4, f2, f3
  blr
  #_ REGISTER_OUT cr 0x08000000

# mcrfs copies field 1 (UX, ZX, XX, VXSNAN) and clears its exception bits;
# FX stays.
test_fpscr_mcrfs:
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 0.0
  mtfsf 255, f0
  fdiv f4, f2, f3
  mcrfs 0, 1
  mffs f1
  blr
  #_ REGISTER_OUT cr 0x40000000
  #_ REGISTER_OUT f1 0xFFF8000080000000
