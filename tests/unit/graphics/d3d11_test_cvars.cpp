/**
 * @file        graphics/d3d11_test_cvars.cpp
 * @brief       Plugin cvars read by the DXBC translator and the D3D11 render
 *              target shaders, for the unit tests that compile them
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/cvar.h>

// The plugin defines these in graphics/command_processor.cpp and
// graphics/pipeline/render_target/cache.cpp, which unit_tests does not compile
// (they pull in the whole GPU core). Same defaults.
REXCVAR_DEFINE_BOOL(occlusion_query_full_counters, false, "GPU",
                    "Test copy of the plugin cvar");
REXCVAR_DEFINE_BOOL(no_discard_stencil_in_transfer_pipelines, false, "GPU",
                    "Test copy of the plugin cvar");
