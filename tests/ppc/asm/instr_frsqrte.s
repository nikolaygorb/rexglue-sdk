# The console's estimate (rex/ppc/estimates.h), not 1 / sqrt(x).
test_frsqrte_1:
  #_ REGISTER_IN f1 1.0
  frsqrte f1, f1
  blr
  #_ REGISTER_OUT f1 0x3FEF100000000000

test_frsqrte_2:
  #_ REGISTER_IN f1 64.0
  frsqrte f1, f1
  blr
  #_ REGISTER_OUT f1 0x3FBF100000000000

test_frsqrte_3:
  #_ REGISTER_IN f1 0.5
  frsqrte f1, f1
  blr
  #_ REGISTER_OUT f1 0x3FF6000000000000
