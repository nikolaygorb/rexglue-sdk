test_mcrfs_rn:
  # Field 7 is (0, NI, RN): RN = 2 sets the eq bit of cr2
  #_ REGISTER_IN cr 0x00000000
  mtfsfi 7, 2
  mcrfs cr2, 7
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT cr 0x00200000
