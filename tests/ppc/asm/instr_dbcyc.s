test_dbcyc_hints:
  # db8cyc/db10cyc/db12cyc are "or rX, rX, rX" on r28/r29/r30: delay hints
  # that leave those registers as they are. The assembler has no mnemonics
  # for them, so they are written as "or".
  #_ REGISTER_IN r28 0x1111111111111111
  #_ REGISTER_IN r29 0x2222222222222222
  #_ REGISTER_IN r30 0x3333333333333333
  or r28, r28, r28
  or r29, r29, r29
  or r30, r30, r30
  blr
  #_ REGISTER_OUT r28 0x1111111111111111
  #_ REGISTER_OUT r29 0x2222222222222222
  #_ REGISTER_OUT r30 0x3333333333333333
