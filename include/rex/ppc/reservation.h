#pragma once

#include <cstdint>

namespace rex::arch {
class Exception;
}

namespace rex::ppc {

// Volatile CPU state, shared across generated function calls even with
// reserved_as_local. A store-conditional consumes it on every outcome.
struct ReservationState {
  uint64_t value = 0;
  uint64_t generation = 0;
  uint32_t address = 0;
  uint8_t width = 0;
  bool valid = false;
};

// Values are raw host representations of guest memory; codegen byte-swaps them.
uint32_t LoadReserve32(ReservationState& state, uint32_t address, const void* host);
uint64_t LoadReserve64(ReservationState& state, uint32_t address, const void* host);
bool StoreConditional32(ReservationState& state, uint32_t address, void* host, uint32_t value);
bool StoreConditional64(ReservationState& state, uint32_t address, void* host, uint64_t value);

// Called by the guest fault handler before acquiring the global region.
// Releases the active x64 claim and returns the assembly continuation that
// fails this store instead of retrying a CAS after its claim was released.
uintptr_t ReleaseFaultingReservation(arch::Exception* exception);

}  // namespace rex::ppc
