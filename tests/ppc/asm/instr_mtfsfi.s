test_mtfsfi_rn:
  # Round toward minus infinity, not plus infinity
  # Exception bits are sticky: start from a clear FPSCR
  #_ REGISTER_IN f0 0.0
  mtfsf 0xFF, f0
  mtfsfi 7, 3
  mffs f2
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT f2 0xFFF8000000000003

test_mtfsfi_other_field:
  # Other fields don't touch RN: field 6 holds VE, OE, UE, ZE (UE and ZE here)
  # Exception bits are sticky: start from a clear FPSCR
  #_ REGISTER_IN f0 0.0
  mtfsf 0xFF, f0
  mtfsfi 6, 3
  mffs f2
  mtfsfi 6, 0
  blr
  #_ REGISTER_OUT f2 0xFFF8000000000030
