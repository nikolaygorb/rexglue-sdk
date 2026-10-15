test_mtfsfi_rn:
  # Round toward minus infinity, not plus infinity
  mtfsfi 7, 3
  mffs f2
  mtfsfi 7, 0
  blr
  #_ REGISTER_OUT f2 0x0000000000000003

test_mtfsfi_other_field:
  # Other fields don't touch RN
  mtfsfi 7, 0
  mtfsfi 6, 3
  mffs f2
  blr
  #_ REGISTER_OUT f2 0x0000000000000000
