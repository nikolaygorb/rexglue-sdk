test_crset_1:
  #_ REGISTER_IN cr 0x00000000
  crset 6
  blr
  #_ REGISTER_OUT cr 0x02000000

test_crset_2:
  #_ REGISTER_IN cr 0x0F0F0F0F
  crset 31
  crset 0
  blr
  #_ REGISTER_OUT cr 0x8F0F0F0F
