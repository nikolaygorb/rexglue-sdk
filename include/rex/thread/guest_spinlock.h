#pragma once

#include <cstdint>

namespace rex::thread {

// owner is the host representation of the guest's big-endian PCR address.
// The word remains the exact owner address, including while waiters sleep.
void AcquireGuestSpinLock(uint32_t* word, uint32_t owner);
void ReleaseGuestSpinLock(uint32_t* word);

}  // namespace rex::thread
