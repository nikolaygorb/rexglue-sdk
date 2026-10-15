test_mtvrsave_roundtrip:
  # VRSAVE is 32 bits: the high word of rS is dropped, mfvrsave zero-extends.
  #_ REGISTER_IN r3 0xFFFFFFFF80000001
  mtvrsave r3
  mfvrsave r4
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFF80000001
  #_ REGISTER_OUT r4 0x80000001

test_mtvrsave_overwrite:
  #_ REGISTER_IN r3 0xFFFFFFFF
  #_ REGISTER_IN r5 0x00F0000F
  mtvrsave r3
  mtvrsave r5
  mfvrsave r4
  blr
  #_ REGISTER_OUT r4 0x00F0000F
