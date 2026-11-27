test_mtfsb_rn:
  # Bits 30 and 31 are RN: 0 -> 1 -> 3 -> 2
  # Exception bits are sticky: start from a clear FPSCR
  #_ REGISTER_IN f0 0.0
  mtfsf 0xFF, f0
  mtfsb1 31
  mtfsb1 30
  mtfsb0 31
  mffs f2
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT f2 0xFFF8000000000002
