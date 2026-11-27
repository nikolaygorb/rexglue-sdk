/**
 * @file        graphics/native/command_recorder.h
 * @brief       Native renderer: where its Vulkan commands are recorded
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/graphics/vulkan/deferred_command_buffer.h>

namespace rex::graphics::native {

// The native renderer records its Vulkan commands into a CPU-side stream (the
// SDK's DeferredCommandBuffer, as the Vulkan backend does), replayed into a
// command buffer when the Submitter submits: with native_threaded_submission
// on a thread of its own, so the driver's work is off the command processor
// thread. Handed out by the Submitter (Open, OpenOutsideRenderPass,
// EnterRenderPass, BeginBeforeRenderPass); null where no command buffer could
// be opened.
using CommandRecorder = vulkan::DeferredCommandBuffer;

}  // namespace rex::graphics::native
