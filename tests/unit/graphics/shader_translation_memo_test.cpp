/**
 * @file        graphics/shader_translation_memo_test.cpp
 * @brief       Tests for the last-translation memo of Shader::GetOrCreateTranslation
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>

using namespace rex::graphics;

// ForzaHorizon1-ReImagined 0362: a repeat of the last modification is answered from the memo and
// gives the same translation the map holds; the memo never outlives its translation.
TEST_CASE("Shader translation memo returns the mapped translation", "[graphics][shader]") {
  REXCVAR_SET(gpu_shader_translation_memo, true);
  Shader shader(xenos::ShaderType::kPixel, 0, nullptr, 0);

  bool is_new = false;
  Shader::Translation* a = shader.GetOrCreateTranslation(1, &is_new);
  CHECK(is_new);
  CHECK(shader.GetOrCreateTranslation(1, &is_new) == a);
  CHECK_FALSE(is_new);

  Shader::Translation* b = shader.GetOrCreateTranslation(2, &is_new);
  CHECK(is_new);
  CHECK(b != a);
  CHECK(shader.GetOrCreateTranslation(1, &is_new) == a);
  CHECK_FALSE(is_new);
  CHECK(shader.GetTranslation(1) == a);

  // Destroying the memoized translation must not leave the memo pointing at it.
  shader.DestroyTranslation(1);
  CHECK(shader.GetTranslation(1) == nullptr);
  Shader::Translation* c = shader.GetOrCreateTranslation(1, &is_new);
  CHECK(is_new);
  CHECK(shader.GetTranslation(1) == c);
}

TEST_CASE("Shader translation memo off still finds translations", "[graphics][shader]") {
  REXCVAR_SET(gpu_shader_translation_memo, false);
  Shader shader(xenos::ShaderType::kVertex, 0, nullptr, 0);
  bool is_new = false;
  Shader::Translation* a = shader.GetOrCreateTranslation(7, &is_new);
  CHECK(is_new);
  CHECK(shader.GetOrCreateTranslation(7, &is_new) == a);
  CHECK_FALSE(is_new);
  REXCVAR_SET(gpu_shader_translation_memo, true);
}
