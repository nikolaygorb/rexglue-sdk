# The Xenon single-precision denormal operand quirk (xenia-edge 28f38affe,
# 8b19ee756): add, subtract, multiply and the single multiply-adds answer with
# the default QNaN; divide and square root keep the double result unrounded.

test_fadds_denormal:
  #_ REGISTER_IN f2 0x0000000000000001
  #_ REGISTER_IN f3 1.0
  fadds f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fsubs_denormal:
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 0x800FFFFFFFFFFFFF
  fsubs f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fmuls_denormal:
  #_ REGISTER_IN f2 0x0008000000000000
  #_ REGISTER_IN f3 2.0
  fmuls f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

# Zeros are not denormal.
test_fadds_zeros:
  #_ REGISTER_IN f2 0.0
  #_ REGISTER_IN f3 0.0
  fadds f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x0000000000000000

# An infinite operand takes precedence over the quirk.
test_fadds_denormal_infinity:
  #_ REGISTER_IN f2 0x0000000000000001
  #_ REGISTER_IN f3 0x7FF0000000000000
  fadds f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x7FF0000000000000

test_fmadds_denormal:
  #_ REGISTER_IN f2 0x0000000000000001
  #_ REGISTER_IN f3 1.0
  #_ REGISTER_IN f4 1.0
  fmadds f1, f2, f3, f4
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fnmsubs_denormal:
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 1.0
  #_ REGISTER_IN f4 0x0000000000000001
  fnmsubs f1, f2, f3, f4
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

# 2^-1023 / 2 is 2^-1024, which single precision would flush to zero.
test_fdivs_denormal:
  #_ REGISTER_IN f2 0x0008000000000000
  #_ REGISTER_IN f3 2.0
  fdivs f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x0004000000000000

test_fdivs_normal:
  #_ REGISTER_IN f2 1.0
  #_ REGISTER_IN f3 3.0
  fdivs f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x3FD5555560000000

# The square root of 2^-1024 is 2^-512, which single precision can't hold.
test_fsqrts_denormal:
  #_ REGISTER_IN f2 0x0004000000000000
  fsqrts f1, f2
  blr
  #_ REGISTER_OUT f1 0x1FF0000000000000
