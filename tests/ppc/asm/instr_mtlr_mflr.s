# LR is kept as 32 bits, the width of a code address (PPCContext::lr):
# mtlr drops the register's upper half and mflr zero-extends.
test_mtlr_mflr_32:
  #_ REGISTER_IN r3 0xFFFFFFFF82001234
  mflr r5
  mtlr r3
  mflr r4
  mtlr r5
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFF82001234
  #_ REGISTER_OUT r4 0x0000000082001234
