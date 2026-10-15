test_fnmadd_1:
  # -(2 * 3 + 1)
  #_ REGISTER_IN f2 2.0
  #_ REGISTER_IN f3 3.0
  #_ REGISTER_IN f4 1.0
  fnmadd f1, f2, f3, f4
  blr
  #_ REGISTER_OUT f1 -7.0
  #_ REGISTER_OUT f2 2.0
  #_ REGISTER_OUT f3 3.0
  #_ REGISTER_OUT f4 1.0

test_fnmadd_nan_not_negated:
  # The negation does not reach a NaN result
  #_ REGISTER_IN f2 0x7FF8000000000003
  #_ REGISTER_IN f3 1.0
  #_ REGISTER_IN f4 1.0
  fnmadd f1, f2, f3, f4
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000003

test_fnmadd_invalid:
  # infinity times zero gives the positive default QNaN, not negated
  #_ REGISTER_IN f2 0x7FF0000000000000
  #_ REGISTER_IN f3 0.0
  #_ REGISTER_IN f4 1.0
  fnmadd f1, f2, f3, f4
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000
