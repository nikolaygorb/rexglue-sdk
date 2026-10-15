test_crmove_1:
  #_ REGISTER_IN cr 0x02000000
  crmove 31, 6
  blr
  #_ REGISTER_OUT cr 0x02000001

test_crmove_2:
  #_ REGISTER_IN cr 0x00000001
  crmove 31, 6
  blr
  #_ REGISTER_OUT cr 0x00000000
