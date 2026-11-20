#include <catch2/catch_test_macros.hpp>
#include <rex/codegen/config.h>
#include "codegen/config_keys.h"

using namespace rex::codegen;

TEST_CASE("Config diagnostics use this fork's schema and qualified paths", "[codegen][config]") {
  const auto table = toml::parse(R"(
file_path = "default.xex"
fast_dot_products = true
lock_free_reservations = false
non_volatile_gpr_as_local = true
fpscr_result_bits = true
skip_lrr = true
skip_msr = "true"
[rexcrt]
memcpy = 0x82001000
[functions.82000000]
size = 16
share_registers = true
nmae = "oops"
)");
  std::vector<std::string> warnings;
  CheckConfigKeys(table, "config.toml", warnings);
  REQUIRE(warnings.size() == 3);
  std::string joined;
  for (const auto& warning : warnings)
    joined += warning + '\n';
  CHECK(joined.find("unknown key 'skip_lrr'") != std::string::npos);
  CHECK(joined.find("key 'skip_msr' should be a boolean") != std::string::npos);
  CHECK(joined.find("functions.82000000.nmae") != std::string::npos);
}

TEST_CASE("Loading config retains warnings without rejecting valid fields", "[codegen][config]") {
  RecompilerConfig config;
  REQUIRE(config.LoadFromTable(
      toml::parse("file_path='game.xex'\nout_directory_path='generated'\nskip_lrr=true"), {}));
  REQUIRE(config.Validate().warnings.size() == 1);
  CHECK(config.Validate().warnings[0].find("skip_lrr") != std::string::npos);
  CHECK_FALSE(config.skipLr);
}

TEST_CASE("setjmp_address and longjmp_address take an address or an array", "[codegen][config]") {
  RecompilerConfig config;
  REQUIRE(config.LoadFromTable(toml::parse("file_path='game.xex'\n"
                                           "out_directory_path='generated'\n"
                                           "setjmp_address=[0x82001000, 0x82002000]\n"
                                           "longjmp_address=0x82003000\n"),
                               {}));
  CHECK(config.Validate().warnings.empty());
  CHECK(config.setJmpAddress == 0x82001000);
  CHECK(config.isSetJmp(0x82001000));
  CHECK(config.isSetJmp(0x82002000));
  CHECK_FALSE(config.isSetJmp(0x82003000));
  CHECK_FALSE(config.isSetJmp(0));
  CHECK(config.longJmpAddress == 0x82003000);
  CHECK(config.longJmpAddresses.empty());
  CHECK(config.isLongJmp(0x82003000));
  CHECK_FALSE(config.isLongJmp(0x82002000));
}

TEST_CASE("A misaligned address in a setjmp_address array is an error", "[codegen][config]") {
  RecompilerConfig config;
  CHECK_FALSE(config.LoadFromTable(toml::parse("file_path='game.xex'\n"
                                               "out_directory_path='generated'\n"
                                               "setjmp_address=[0x82001000, 0x82002002]\n"),
                                   {}));
  CHECK_FALSE(config.Validate().valid);
}
