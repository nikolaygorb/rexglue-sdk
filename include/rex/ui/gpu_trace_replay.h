/**
 * @file        rex/ui/gpu_trace_replay.h
 * @brief       Draws a GPU frame trace instead of running the title, saves
 *              the frame as PNG and compares it with a reference image.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace rex::ui {

// Whether trace_replay names a trace: then the title isn't loaded.
bool IsGpuTraceReplay();

// Plays the trace, writes the PNG and compares it. Blocks; call it off the UI
// thread. Returns the process exit code: 0 drawn (and matching the reference
// if one is given), 1 different from the reference, 2 not drawn.
int RunGpuTraceReplay(system::IGraphicsSystem& graphics_system);

}  // namespace rex::ui
