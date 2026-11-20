#include <rex/ppc/reservation.h>

#include <array>
#include <atomic>
#include <cstddef>

#include <rex/exception_handler.h>
#include <rex/platform.h>
#include <rex/thread.h>

namespace rex::ppc {
namespace {

// Xenon reserves 128-byte lines. Hash collisions cause only permitted spurious
// failures. Masking physical aliases is conservative for virtual addresses too.
constexpr size_t kGranules = 1 << 16;
std::array<std::atomic<uint64_t>, kGranules> generations{};

std::atomic<uint64_t>& Granule(uint32_t address) {
  return generations[((address & 0x1FFFFFFFu) >> 7) & (kGranules - 1)];
}

struct StoreClaim {
  std::atomic<uint64_t>* granule;
  uint64_t released_generation;
  const void* host;
  size_t width;
  volatile uintptr_t resume_pc = 0;
  volatile bool faulted = false;
};
thread_local StoreClaim* active_claim = nullptr;

uint64_t AwaitGeneration(const std::atomic<uint64_t>& granule) {
  uint64_t generation = granule.load(std::memory_order_acquire);
  for (uint32_t spin = 0; (generation & 1) && spin < 256; ++spin) {
#if REX_ARCH_AMD64
    _mm_pause();
#endif
    if ((spin & 63) == 63)
      rex::thread::MaybeYield();
    generation = granule.load(std::memory_order_acquire);
  }
  return generation;
}

template <typename T>
T Load(ReservationState& state, uint32_t address, const void* host) {
  state.valid = false;
  state.address = address;
  state.width = sizeof(T);
  state.generation = AwaitGeneration(Granule(address));
  const T value =
      std::atomic_ref<T>(*static_cast<T*>(const_cast<void*>(host))).load(std::memory_order_relaxed);
  state.value = value;
  state.valid = !(state.generation & 1);
  return value;
}

template <typename T>
bool Store(ReservationState& state, uint32_t address, void* host, T value) {
  const bool valid = state.valid;
  state.valid = false;
  if (!valid || state.address != address || state.width != sizeof(T))
    return false;
  T expected = static_cast<T>(state.value);
#if REX_ARCH_AMD64
  auto& granule = Granule(address);
  uint64_t generation = state.generation;
  if ((generation & 1) ||
      !granule.compare_exchange_strong(generation, generation + 1, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return false;
  }
  StoreClaim claim{&granule, generation + 2, host, sizeof(T)};
  active_claim = &claim;
  uint8_t stored;
  // The fault continuation is an actual assembly label after the one access.
  // No generated instruction decoding, machine register assumptions or SEH
  // unwinding are needed. The handler releases the claim, settles the watch,
  // then resumes at 1; faulted overrides the arbitrary condition flags there.
  if constexpr (sizeof(T) == 4) {
    asm volatile(
        "leaq 1f(%%rip), %%r11\n\t"
        "movq %%r11, %1\n\t"
        "lock cmpxchgl %4, %2\n\t"
        "1: sete %0"
        : "=qm"(stored), "=m"(claim.resume_pc), "+m"(*static_cast<T*>(host)), "+a"(expected)
        : "r"(value)
        : "r11", "memory", "cc");
  } else {
    asm volatile(
        "leaq 1f(%%rip), %%r11\n\t"
        "movq %%r11, %1\n\t"
        "lock cmpxchgq %4, %2\n\t"
        "1: sete %0"
        : "=qm"(stored), "=m"(claim.resume_pc), "+m"(*static_cast<T*>(host)), "+a"(expected)
        : "r"(value)
        : "r11", "memory", "cc");
  }
  active_claim = nullptr;
  if (!claim.faulted)
    granule.store(claim.released_generation, std::memory_order_release);
  return !claim.faulted && stored;
#else
  // Canary leaves A64 on its native value-CAS path too. The x64 fault
  // continuation above cannot be used by an ARM exclusive-instruction loop.
  return std::atomic_ref<T>(*static_cast<T*>(host))
      .compare_exchange_strong(expected, value, std::memory_order_acq_rel,
                               std::memory_order_acquire);
#endif
}

}  // namespace

uint32_t LoadReserve32(ReservationState& state, uint32_t address, const void* host) {
  return Load<uint32_t>(state, address, host);
}
uint64_t LoadReserve64(ReservationState& state, uint32_t address, const void* host) {
  return Load<uint64_t>(state, address, host);
}
bool StoreConditional32(ReservationState& state, uint32_t address, void* host, uint32_t value) {
  return Store<uint32_t>(state, address, host, value);
}
bool StoreConditional64(ReservationState& state, uint32_t address, void* host, uint64_t value) {
  return Store<uint64_t>(state, address, host, value);
}

uintptr_t ReleaseFaultingReservation(arch::Exception* exception) {
#if REX_ARCH_AMD64
  auto* claim = active_claim;
  if (!claim || !claim->resume_pc || exception->code() != arch::Exception::Code::kAccessViolation ||
      exception->access_violation_operation() !=
          arch::Exception::AccessViolationOperation::kWrite) {
    return 0;
  }
  const uintptr_t address = exception->fault_address();
  const uintptr_t target = reinterpret_cast<uintptr_t>(claim->host);
  if (address < target || address - target >= claim->width)
    return 0;
  active_claim = nullptr;
  claim->faulted = true;
  claim->granule->store(claim->released_generation, std::memory_order_release);
  return claim->resume_pc;
#else
  return 0;
#endif
}

}  // namespace rex::ppc
