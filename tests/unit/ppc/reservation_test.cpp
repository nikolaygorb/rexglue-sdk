#include <catch2/catch_test_macros.hpp>
#include <rex/ppc/reservation.h>
#include <rex/platform.h>

#include <array>
#include <thread>
#include <vector>

using namespace rex::ppc;

TEST_CASE("Store conditional consumes its reservation and checks address and width",
          "[ppc][reservation]") {
  alignas(128) std::array<uint64_t, 16> words{};
  words[0] = 7;
  ReservationState state;
  CHECK_FALSE(StoreConditional64(state, 0x10000, words.data(), 10));
  CHECK(LoadReserve64(state, 0x10000, words.data()) == 7);
  CHECK_FALSE(StoreConditional64(state, 0x10008, words.data() + 1, 10));
  CHECK_FALSE(StoreConditional64(state, 0x10000, words.data(), 10));
  LoadReserve32(state, 0x10000, words.data());
  CHECK_FALSE(StoreConditional64(state, 0x10000, words.data(), 10));
  LoadReserve64(state, 0x10000, words.data());
  CHECK(StoreConditional64(state, 0x10000, words.data(), 10));
  CHECK(words[0] == 10);
  CHECK_FALSE(StoreConditional64(state, 0x10000, words.data(), 11));
}

#if REX_ARCH_AMD64
TEST_CASE("A-B-A through reserved stores invalidates a stale lock-free pop", "[ppc][reservation]") {
  alignas(128) uint32_t head = 1;
  ReservationState stale, other;
  REQUIRE(LoadReserve32(stale, 0x20000, &head) == 1);
  LoadReserve32(other, 0x20000, &head);
  REQUIRE(StoreConditional32(other, 0x20000, &head, 2));
  LoadReserve32(other, 0x20000, &head);
  REQUIRE(StoreConditional32(other, 0x20000, &head, 1));
  CHECK_FALSE(StoreConditional32(stale, 0x20000, &head, 99));
  CHECK(head == 1);
}

TEST_CASE("A reserved write invalidates other words in its 128-byte granule",
          "[ppc][reservation]") {
  alignas(128) std::array<uint32_t, 32> words{};
  ReservationState first, other;
  LoadReserve32(first, 0x30000, words.data());
  LoadReserve32(other, 0x30004, words.data() + 1);
  REQUIRE(StoreConditional32(other, 0x30004, words.data() + 1, 1));
  CHECK_FALSE(StoreConditional32(first, 0x30000, words.data(), 1));
  CHECK(words[0] == 0);
}
#endif

TEST_CASE("Contending reserved increments do not lose updates", "[ppc][reservation]") {
  alignas(128) uint64_t word = 0;
  std::vector<std::thread> workers;
  for (uint32_t i = 0; i < 4; ++i) {
    workers.emplace_back([&] {
      ReservationState state;
      for (uint32_t n = 0; n < 500; ++n) {
        uint64_t value;
        do {
          value = LoadReserve64(state, 0x40000, &word);
        } while (!StoreConditional64(state, 0x40000, &word, value + 1));
      }
    });
  }
  for (auto& worker : workers)
    worker.join();
  CHECK(word == 2000);
}

#if REX_ARCH_AMD64 && REX_PLATFORM_WIN32
#include <windows.h>
#include <rex/exception_handler.h>

namespace {
struct FaultState {
  void* page;
  bool recovered = false;
};
bool RecoverReservationFault(rex::arch::Exception* ex, void* opaque) {
  auto& state = *static_cast<FaultState*>(opaque);
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation)
    return false;
  if (ex->fault_address() != reinterpret_cast<uintptr_t>(state.page))
    return false;
  const uintptr_t resume = ReleaseFaultingReservation(ex);
  if (!resume)
    return false;
  DWORD previous;
  if (!VirtualProtect(state.page, 4096, PAGE_READWRITE, &previous))
    return false;
  state.recovered = true;
  ex->set_resume_pc(resume);
  return true;
}
}  // namespace

TEST_CASE("A faulting conditional store releases its claim and fails without writing",
          "[ppc][reservation]") {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  REQUIRE(page);
  auto* word = static_cast<uint32_t*>(page);
  *word = 7;
  FaultState fault{page};
  rex::arch::ExceptionHandler::Install(RecoverReservationFault, &fault);
  DWORD previous;
  const bool protected_page = VirtualProtect(page, 4096, PAGE_READONLY, &previous);
  ReservationState state;
  LoadReserve32(state, 0x50000, word);
  const bool stored = StoreConditional32(state, 0x50000, word, 9);
  const uint32_t after_fault = *word;
  // The granule must be usable immediately after the fault, with a fresh load.
  LoadReserve32(state, 0x50000, word);
  const bool retry = StoreConditional32(state, 0x50000, word, 10);
  rex::arch::ExceptionHandler::Uninstall(RecoverReservationFault, &fault);
  VirtualFree(page, 0, MEM_RELEASE);
  CHECK(protected_page);
  CHECK(fault.recovered);
  CHECK_FALSE(stored);
  CHECK(after_fault == 7);
  CHECK(retry);
}
#endif
