test_icbi_1:
  # No translated code to invalidate: memory and registers stay as they are.
  #_ MEMORY_IN 10001000 60000000 4E800020
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 4
  icbi r3, r4
  isync
  lwz r5, 0(r3)
  blr
  #_ REGISTER_OUT r3 0x10001000
  #_ REGISTER_OUT r4 4
  #_ REGISTER_OUT r5 0x60000000
  #_ MEMORY_OUT 10001000 60000000 4E800020
