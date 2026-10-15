test_dst_hints:
  # Data stream touch is a prefetch hint: no register or memory changes.
  #_ MEMORY_IN 10001000 01234567 89ABCDEF
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 0x00200010
  dst r3, r4, 0
  dstt r3, r4, 1
  dstst r3, r4, 2
  dststt r3, r4, 3
  lwz r5, 4(r3)
  blr
  #_ REGISTER_OUT r3 0x10001000
  #_ REGISTER_OUT r4 0x00200010
  #_ REGISTER_OUT r5 0x89ABCDEF
  #_ MEMORY_OUT 10001000 01234567 89ABCDEF
