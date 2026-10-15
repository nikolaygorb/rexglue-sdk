test_rotld_1:
  #_ REGISTER_IN r4 0x0123456789ABCDEF
  #_ REGISTER_IN r5 8
  rotld r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x23456789ABCDEF01
  #_ REGISTER_OUT r4 0x0123456789ABCDEF
  #_ REGISTER_OUT r5 8

test_rotld_mask:
  # Only the low 6 bits of rB count: 0x7C -> 60.
  #_ REGISTER_IN r4 0x0123456789ABCDEF
  #_ REGISTER_IN r5 0x7C
  rotld r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0xF0123456789ABCDE

test_rotld_zero:
  #_ REGISTER_IN r4 0x8000000000000001
  #_ REGISTER_IN r5 0x40
  rotld r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x8000000000000001
