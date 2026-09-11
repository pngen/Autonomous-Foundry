cmake_minimum_required(VERSION 3.20)

project(AutonomousFoundry
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for coordinating populations of autonomous production workers"
  HOMEPAGE_URL "https://github.com/pngen/Autonomous-Foundry"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(AUTONOMOUS_FOUNDRY_BUILD_APPS "Build the af_cli, af_coordinator and af_worker executables" ON)
option(AUTONOMOUS_FOUNDRY_BUILD_TESTS "Build the validation suites" ON)
option(AUTONOMOUS_FOUNDRY_BUILD_EXAMPLES "Build the runnable examples" ON)
option(AUTONOMOUS_FOUNDRY_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(AUTONOMOUS_FOUNDRY_ENABLE_ASAN "Instrument first-party code with AddressSanitizer" OFF)
option(AUTONOMOUS_FOUNDRY_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning contract
# ---------------------------------------------------------------------------

add_library(autonomous_foundry_warnings INTERFACE)
add_library(AutonomousFoundry::warnings ALIAS autonomous_foundry_warnings)

if(MSVC)
  target_compile_options(autonomous_foundry_warnings INTERFACE
    /W4
    /permissive-
    /utf-8
    /EHsc
    /Zc:__cplusplus
    /Zc:preprocessor
    /Zc:inline
    /Zc:throwingNew
    /guard:cf
    /sdl
    /MP)
  if(AUTONOMOUS_FOUNDRY_WARNINGS_AS_ERRORS)
    target_compile_options(autonomous_foundry_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(autonomous_foundry_warnings INTERFACE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE)
else()
  target_compile_options(autonomous_foundry_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wformat=2)
  if(AUTONOMOUS_FOUNDRY_WARNINGS_AS_ERRORS)
    target_compile_options(autonomous_foundry_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

set(AUTONOMOUS_FOUNDRY_SOURCES
  src/version.cpp
  src/hash.cpp
  src/error.cpp
  src/id.cpp
  src/accounting.cpp
  src/authority.cpp
  src/artifact.cpp
  src/task.cpp
  src/policy.cpp
  src/evaluation.cpp
  src/attempt.cpp
  src/worker.cpp
  src/candidate.cpp
  src/lineage.cpp
  src/population.cpp
  src/selection.cpp
  src/retention.cpp
  src/promotion.cpp
  src/workspace.cpp
  src/persistence.cpp
  src/protocol.cpp
  src/transport.cpp
  src/process.cpp
  src/reference_task.cpp
  src/evaluator.cpp
  src/foundry_core.cpp
  src/foundry_select.cpp
  src/foundry_lifecycle.cpp
  src/foundry_dispatch.cpp
  src/foundry_recovery.cpp
  src/coordinator.cpp
  src/worker_runtime.cpp
  src/local_promotion_sink.cpp)

add_library(autonomous_foundry STATIC ${AUTONOMOUS_FOUNDRY_SOURCES})
add_library(AutonomousFoundry::autonomous_foundry ALIAS autonomous_foundry)

target_include_directories(autonomous_foundry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(autonomous_foundry
  PUBLIC
    AUTONOMOUS_FOUNDRY_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    AUTONOMOUS_FOUNDRY_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    AUTONOMOUS_FOUNDRY_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    AUTONOMOUS_FOUNDRY_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(autonomous_foundry PRIVATE autonomous_foundry_warnings)

if(MSVC)
  target_compile_options(autonomous_foundry PRIVATE /bigobj)
endif()

if(WIN32)
  target_link_libraries(autonomous_foundry PUBLIC ws2_32)
endif()

find_package(Threads REQUIRED)
target_link_libraries(autonomous_foundry PUBLIC Threads::Threads)

if(AUTONOMOUS_FOUNDRY_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(autonomous_foundry PUBLIC /fsanitize=address /Zi)
    target_compile_definitions(autonomous_foundry PUBLIC _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    target_link_options(autonomous_foundry PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(autonomous_foundry PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(autonomous_foundry PUBLIC -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

if(AUTONOMOUS_FOUNDRY_BUILD_APPS)
  add_executable(af_coordinator apps/af_coordinator_main.cpp)
  target_link_libraries(af_coordinator PRIVATE autonomous_foundry autonomous_foundry_warnings)

  add_executable(af_worker apps/af_worker_main.cpp)
  target_link_libraries(af_worker PRIVATE autonomous_foundry autonomous_foundry_warnings)

  add_executable(af_cli apps/af_cli.cpp)
  target_link_libraries(af_cli PRIVATE autonomous_foundry autonomous_foundry_warnings)
  # af_cli shares the private CLI declarations in src/cli_impl.hpp with the
  # library's own translation units, so it needs the same private include root.
  target_include_directories(af_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(AUTONOMOUS_FOUNDRY_BUILD_TESTS)
  enable_testing()

  add_executable(af_tests
    tests/test_main.cpp
    tests/test_ids.cpp
    tests/test_authority.cpp
    tests/test_lifecycle.cpp
    tests/test_lineage.cpp
    tests/test_evaluation.cpp
    tests/test_selection.cpp
    tests/test_retention.cpp
    tests/test_promotion.cpp
    tests/test_accounting.cpp
    tests/test_persistence.cpp
    tests/test_protocol.cpp
    tests/test_workspace.cpp
    tests/test_property.cpp
    tests/test_adversarial.cpp
    tests/test_concurrency.cpp
    tests/test_foundry_end_to_end.cpp
    tests/test_evaluator.cpp
    tests/test_support.cpp)
  target_link_libraries(af_tests PRIVATE autonomous_foundry autonomous_foundry_warnings)
  target_include_directories(af_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  # The distributed proofs are a second binary over the same harness contract
  # and the same shared support translation unit; each binary registers only the
  # cases it links, so the shared main reports exactly its own suites.
  add_executable(af_distributed_tests
    tests/test_main.cpp
    tests/test_distributed_worker_death.cpp
    tests/test_distributed_coordinator_restart.cpp
    tests/test_distributed_proof.cpp
    tests/test_support.cpp)
  target_link_libraries(af_distributed_tests PRIVATE autonomous_foundry autonomous_foundry_warnings)
  target_include_directories(af_distributed_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  # Environment for compiler-backed evaluation. The toolchain is discovered at
  # configure time inside the developer environment and handed to the test
  # processes explicitly; no absolute path is ever written into the repository.
  set(AF_TEST_TOOLCHAIN_ENV "AF_REFERENCE_CXX=${CMAKE_CXX_COMPILER}")
  if(DEFINED ENV{INCLUDE})
    string(REPLACE ";" "\\;" AF_INCLUDE_ESCAPED "$ENV{INCLUDE}")
    list(APPEND AF_TEST_TOOLCHAIN_ENV "AF_REFERENCE_INCLUDE=${AF_INCLUDE_ESCAPED}")
  endif()
  if(DEFINED ENV{LIB})
    string(REPLACE ";" "\\;" AF_LIB_ESCAPED "$ENV{LIB}")
    list(APPEND AF_TEST_TOOLCHAIN_ENV "AF_REFERENCE_LIB=${AF_LIB_ESCAPED}")
  endif()
  if(MSVC)
    list(APPEND AF_TEST_TOOLCHAIN_ENV "AF_REFERENCE_FLAVOUR=msvc")
  else()
    list(APPEND AF_TEST_TOOLCHAIN_ENV "AF_REFERENCE_FLAVOUR=gnu")
  endif()

  add_test(NAME af_core_tests COMMAND af_tests)
  set_tests_properties(af_core_tests PROPERTIES
    ENVIRONMENT "${AF_TEST_TOOLCHAIN_ENV}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  if(AUTONOMOUS_FOUNDRY_BUILD_APPS)
    set(AF_DISTRIBUTED_ENV
      "${AF_TEST_TOOLCHAIN_ENV}"
      "AF_COORDINATOR_EXE=$<TARGET_FILE:af_coordinator>"
      "AF_WORKER_EXE=$<TARGET_FILE:af_worker>"
      "AF_CLI_EXE=$<TARGET_FILE:af_cli>")
    add_test(NAME af_distributed_tests COMMAND af_distributed_tests)
    set_tests_properties(af_distributed_tests PROPERTIES
      ENVIRONMENT "${AF_DISTRIBUTED_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(AUTONOMOUS_FOUNDRY_BUILD_EXAMPLES)
  set(AF_EXAMPLES
    ex_basic_population
    ex_selection_and_retention
    ex_lineage_evolution
    ex_stale_authority_rejection
    ex_promotion_handoff
    ex_persistence_recovery
    ex_cancellation)
  foreach(example IN LISTS AF_EXAMPLES)
    add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE autonomous_foundry autonomous_foundry_warnings)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(AUTONOMOUS_FOUNDRY_BUILD_BENCHMARKS)
  add_executable(af_benchmarks benchmarks/af_benchmarks.cpp benchmarks/bench_support.cpp)
  target_link_libraries(af_benchmarks PRIVATE autonomous_foundry autonomous_foundry_warnings)
  target_include_directories(af_benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------

install(TARGETS autonomous_foundry autonomous_foundry_warnings
  EXPORT AutonomousFoundryTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(AUTONOMOUS_FOUNDRY_BUILD_APPS)
  install(TARGETS af_cli af_coordinator af_worker
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT AutonomousFoundryTargets
  FILE AutonomousFoundryTargets.cmake
  NAMESPACE AutonomousFoundry::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AutonomousFoundry)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/AutonomousFoundryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AutonomousFoundryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/AutonomousFoundryConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AutonomousFoundry)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/AutonomousFoundryConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/AutonomousFoundryConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AutonomousFoundry)

install(FILES LICENSE README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/autonomous-foundry
  OPTIONAL)
