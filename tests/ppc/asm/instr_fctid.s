# 2^63 is the first value past the range, and double(LLONG_MAX) rounds up to it.
test_fctid_two_pow_63:
  #_ REGISTER_IN f0 0x43E0000000000000
  fctid f1, f0
  blr
  #_ REGISTER_OUT f1 0x7FFFFFFFFFFFFFFF

test_fctid_below_two_pow_63:
  #_ REGISTER_IN f0 0x43DFFFFFFFFFFFFF
  fctid f1, f0
  blr
  #_ REGISTER_OUT f1 0x7FFFFFFFFFFFFC00

test_fctid_minus_two_pow_63:
  #_ REGISTER_IN f0 0xC3E0000000000000
  fctid f1, f0
  blr
  #_ REGISTER_OUT f1 0x8000000000000000

test_fctidz_two_pow_63:
  #_ REGISTER_IN f0 0x43E0000000000000
  fctidz f1, f0
  blr
  #_ REGISTER_OUT f1 0x7FFFFFFFFFFFFFFF

test_fctidz_below_two_pow_63:
  #_ REGISTER_IN f0 0x43DFFFFFFFFFFFFF
  fctidz f1, f0
  blr
  #_ REGISTER_OUT f1 0x7FFFFFFFFFFFFC00
