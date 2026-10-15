test_mfvrsave_reset:
  # VRSAVE starts at 0.
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  mfvrsave r4
  blr
  #_ REGISTER_OUT r4 0
