# Hardware-captured PPC test corpus
# The xenia-project gen_tests corpus in xenia-edge's tree (REXGLUE_PPC_CORPUS_DIR);
# see README.md. Built with REXGLUE_PPC_CORPUS=ON, labelled ppc_corpus.

include(ppc_test_pipeline)

if(NOT EXISTS "${REXGLUE_PPC_CORPUS_DIR}/skip.txt")
    message(FATAL_ERROR "REXGLUE_PPC_CORPUS needs REXGLUE_PPC_CORPUS_DIR: "
                        "xenia-edge's src/xenia/cpu/ppc/testing")
endif()

# With an overlay, the corpus is copied into the build tree and the overlay's
# files replace those of the same name (README.md, "Other vectors").
set(CORPUS_ASM_DIR ${REXGLUE_PPC_CORPUS_DIR})
if(REXGLUE_PPC_CORPUS_OVERLAY_DIR)
    set(CORPUS_ASM_DIR ${CMAKE_BINARY_DIR}/tests/ppc_corpus/asm)
    file(GLOB CORPUS_BASE_FILES ${REXGLUE_PPC_CORPUS_DIR}/*.s)
    file(COPY ${CORPUS_BASE_FILES} DESTINATION ${CORPUS_ASM_DIR})
    file(GLOB CORPUS_OVERLAY_FILES ${REXGLUE_PPC_CORPUS_OVERLAY_DIR}/*.s)
    file(COPY ${CORPUS_OVERLAY_FILES} DESTINATION ${CORPUS_ASM_DIR})
    list(LENGTH CORPUS_OVERLAY_FILES CORPUS_OVERLAY_COUNT)
    message(STATUS "PPC corpus: ${CORPUS_OVERLAY_COUNT} files from ${REXGLUE_PPC_CORPUS_OVERLAY_DIR}")
endif()
# The skip list: the corpus' own, another, or none.
set(CORPUS_SKIP_ARGS --skip-list=${REXGLUE_PPC_CORPUS_DIR}/skip.txt)
set(CORPUS_SKIP_DEPENDS ${REXGLUE_PPC_CORPUS_DIR}/skip.txt)
if(REXGLUE_PPC_CORPUS_SKIP_LIST STREQUAL "none")
    set(CORPUS_SKIP_ARGS "")
    set(CORPUS_SKIP_DEPENDS "")
    message(STATUS "PPC corpus: no skip list")
elseif(REXGLUE_PPC_CORPUS_SKIP_LIST)
    set(CORPUS_SKIP_ARGS --skip-list=${REXGLUE_PPC_CORPUS_SKIP_LIST})
    set(CORPUS_SKIP_DEPENDS ${REXGLUE_PPC_CORPUS_SKIP_LIST})
    message(STATUS "PPC corpus: skip list ${REXGLUE_PPC_CORPUS_SKIP_LIST}")
endif()

set(CORPUS_OBJ_DIR ${CMAKE_BINARY_DIR}/tests/ppc_corpus/obj)
set(CORPUS_BIN_DIR ${CMAKE_BINARY_DIR}/tests/ppc_corpus/bin)
set(CORPUS_GEN_DIR ${CMAKE_BINARY_DIR}/tests/ppc_corpus/generated)
file(MAKE_DIRECTORY ${CORPUS_OBJ_DIR} ${CORPUS_BIN_DIR} ${CORPUS_GEN_DIR})

# The cases are generated as data (recompile-tests --table) and split so the
# corpus compiles in parallel; a job pool caps how many chunks compile at once.
set(CORPUS_CHUNKS 16)
set_property(GLOBAL APPEND PROPERTY JOB_POOLS ppc_corpus=4)

file(GLOB CORPUS_ASM_FILES ${CORPUS_ASM_DIR}/instr*.s ${CORPUS_ASM_DIR}/seq_*.s)
# Files the bundled assembler cannot assemble (assembler_unsupported.txt).
file(STRINGS ${REXGLUE_PPC_CORPUS_EXCLUDE_LIST} UNSUPPORTED_LINES
     REGEX "^[^#]")
foreach(LINE ${UNSUPPORTED_LINES})
    string(REGEX MATCH "^[^ 	]+" STEM "${LINE}")
    list(REMOVE_ITEM CORPUS_ASM_FILES ${CORPUS_ASM_DIR}/${STEM}.s)
endforeach()
list(LENGTH CORPUS_ASM_FILES CORPUS_FILE_COUNT)
foreach(ASM_FILE ${CORPUS_ASM_FILES})
    ppc_add_test_binary(${ASM_FILE} ${CORPUS_OBJ_DIR} ${CORPUS_BIN_DIR} PPC_CORPUS)
endforeach()
get_property(CORPUS_BINS GLOBAL PROPERTY PPC_CORPUS_BINS)
get_property(CORPUS_MAPS GLOBAL PROPERTY PPC_CORPUS_MAPS)

set(CORPUS_SOURCES "")
math(EXPR LAST_CHUNK "${CORPUS_CHUNKS} - 1")
foreach(i RANGE ${LAST_CHUNK})
    list(APPEND CORPUS_SOURCES
        ${CORPUS_GEN_DIR}/ppc_test_functions_${i}.cpp
        ${CORPUS_GEN_DIR}/ppc_test_cases_${i}.cpp)
endforeach()

add_custom_command(
    OUTPUT ${CORPUS_SOURCES} ${CORPUS_GEN_DIR}/ppc_test_decls.h
           ${CORPUS_GEN_DIR}/ppc_table_runner.h
    COMMAND $<TARGET_FILE:rexglue> recompile-tests
            --output=${CORPUS_GEN_DIR}
            --bin-dir=${CORPUS_BIN_DIR}
            --asm-dir=${CORPUS_ASM_DIR}
            --chunks=${CORPUS_CHUNKS}
            --table
            ${CORPUS_SKIP_ARGS}
            --known-failures=${REXGLUE_PPC_CORPUS_KNOWN_FAILURES}
    DEPENDS ${CORPUS_BINS} ${CORPUS_MAPS} ${CORPUS_ASM_FILES}
            ${CORPUS_SKIP_DEPENDS}
            ${REXGLUE_PPC_CORPUS_KNOWN_FAILURES} rexglue
    COMMENT "Generating the PPC corpus tests with rexglue"
    VERBATIM
)
set_source_files_properties(${CORPUS_SOURCES} ${CORPUS_GEN_DIR}/ppc_test_decls.h
    ${CORPUS_GEN_DIR}/ppc_table_runner.h
    PROPERTIES GENERATED TRUE)

add_executable(ppc_corpus_tests ${CORPUS_SOURCES})
target_include_directories(ppc_corpus_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CORPUS_GEN_DIR}
)
target_link_libraries(ppc_corpus_tests PRIVATE
    simde
    rexruntime
    fmt::fmt
    Catch2::Catch2WithMain
)
if(WIN32 AND TARGET amd_fidelityfx_dx12)
    target_link_options(ppc_corpus_tests PRIVATE
        "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_dx12>")
    target_link_libraries(ppc_corpus_tests PRIVATE delayimp)
endif()
set_target_properties(ppc_corpus_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
    JOB_POOL_COMPILE ppc_corpus
)
target_compile_options(ppc_corpus_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
    -msse4.1 -mssse3 -w
)
# ISO volatile semantics, as REXGLUE_RECOMP_OPTIONS gives the titles' code.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(ppc_corpus_tests PRIVATE -fno-ms-volatile)
endif()

# One CTest test per file, as the generated sources have one TEST_CASE per file.
foreach(ASM_FILE ${CORPUS_ASM_FILES})
    get_filename_component(STEM ${ASM_FILE} NAME_WE)
    add_test(NAME ppc_corpus.${STEM} COMMAND ppc_corpus_tests "[${STEM}]" --allow-running-no-tests)
    set_tests_properties(ppc_corpus.${STEM} PROPERTIES LABELS "ppc_corpus" TIMEOUT 300)
endforeach()

message(STATUS "Configured the PPC corpus with ${CORPUS_FILE_COUNT} files")
