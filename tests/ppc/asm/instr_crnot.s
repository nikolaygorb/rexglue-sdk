test_crnot_1:
  #_ REGISTER_IN cr 0x00000000
  crnot 0, 6
  blr
  #_ REGISTER_OUT cr 0x80000000

test_crnot_2:
  #_ REGISTER_IN cr 0x82000000
  crnot 0, 6
  blr
  #_ REGISTER_OUT cr 0x02000000

test_crnot_same_bit:
  #_ REGISTER_IN cr 0x00000001
  crnot 31, 31
  blr
  #_ REGISTER_OUT cr 0x00000000
