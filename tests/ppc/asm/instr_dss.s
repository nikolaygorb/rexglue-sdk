test_dss_hints:
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x00200010
  dst r3, r4, 1
  dss 1
  dssall
  addi r5, r3, 4
  blr
  #_ REGISTER_OUT r3 0x10001000
  #_ REGISTER_OUT r4 0x00200010
  #_ REGISTER_OUT r5 0x10001004
