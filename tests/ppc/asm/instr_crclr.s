test_crclr_1:
  #_ REGISTER_IN cr 0xFFFFFFFF
  crclr 6
  blr
  #_ REGISTER_OUT cr 0xFDFFFFFF

test_crclr_2:
  #_ REGISTER_IN cr 0x80000001
  crclr 0
  crclr 31
  blr
  #_ REGISTER_OUT cr 0x00000000
