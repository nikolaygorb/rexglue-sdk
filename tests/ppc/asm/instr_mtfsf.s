test_mtfsf_field7:
  # FM bit 0 selects field 7, which holds RN
  #_ REGISTER_IN f1 0x0000000000000003
  #_ REGISTER_IN f3 0x0000000000000000
  mtfsf 0xFF, f3
  mtfsf 0x01, f1
  mffs f2
  mtfsf 0xFF, f3
  blr
  #_ REGISTER_OUT f2 0x0000000000000003

test_mtfsf_field0:
  # FM bit 7 selects field 0, which leaves RN alone
  #_ REGISTER_IN f1 0x0000000000000003
  #_ REGISTER_IN f3 0x0000000000000000
  mtfsf 0xFF, f3
  mtfsf 0x80, f1
  mffs f2
  mtfsf 0xFF, f3
  blr
  #_ REGISTER_OUT f2 0x0000000000000000
