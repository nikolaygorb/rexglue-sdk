test_stwcx_without_reservation:
  #_ MEMORY_IN 10001000 00000001
  #_ REGISTER_IN r3 0x10001000
  li r4, 2
  stwcx. r4, 0, r3
  li r6, 0
  bne no_reservation_done
  li r6, 1
no_reservation_done:
  blr
  #_ REGISTER_OUT r6 0
  #_ MEMORY_OUT 10001000 00000001

test_stwcx_consumes_reservation:
  #_ MEMORY_IN 10001000 00000001
  #_ REGISTER_IN r3 0x10001000
  lwarx r10, 0, r3
  li r4, 2
  stwcx. r4, 0, r3
  li r4, 3
  stwcx. r4, 0, r3
  li r6, 0
  bne consumed_done
  li r6, 1
consumed_done:
  blr
  #_ REGISTER_OUT r6 0
  #_ MEMORY_OUT 10001000 00000002

test_stwcx_requires_same_address:
  #_ MEMORY_IN 10001000 0000000100000001
  #_ REGISTER_IN r3 0x10001000
  addi r5, r3, 4
  lwarx r10, 0, r3
  li r4, 2
  stwcx. r4, 0, r5
  li r6, 0
  bne address_done
  li r6, 1
address_done:
  blr
  #_ REGISTER_OUT r6 0
  #_ MEMORY_OUT 10001000 0000000100000001
