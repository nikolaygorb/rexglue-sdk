test_isync_1:
  #_ REGISTER_IN r3 5
  isync
  addi r3, r3, 1
  blr
  #_ REGISTER_OUT r3 6
