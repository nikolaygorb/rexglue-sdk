/**
 * @file        perf/thread_sampler.h
 * @brief       Wall-clock sampler of the title's threads (thread_sampler_* cvars)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

namespace rex::perf {

/// Registers the request cvar and starts a capture if thread_sampler_delay or
/// thread_sampler_request ask for one. ReXApp calls it before OnPostSetup.
void StartThreadSampler();

/// Captures thread_sampler_seconds now (after the running capture, if any).
void RequestThreadSamplerCapture();

/// One guest frame presented by the calling thread. VdSwap calls it; a title
/// whose renderer bypasses VdSwap calls it from its own present hook.
void NoteGuestSwap();

}  // namespace rex::perf
