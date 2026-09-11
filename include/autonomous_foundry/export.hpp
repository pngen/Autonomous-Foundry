#pragma once

// Autonomous Foundry -- public export macros.
//
// The runtime is built as a static library by default. When built as a shared
// library the macros below expand to the platform import/export attributes.

#if defined(_WIN32) || defined(__CYGWIN__)
#  define AUTONOMOUS_FOUNDRY_EXPORT __declspec(dllexport)
#  define AUTONOMOUS_FOUNDRY_IMPORT __declspec(dllimport)
#  define AUTONOMOUS_FOUNDRY_LOCAL
#else
#  define AUTONOMOUS_FOUNDRY_EXPORT __attribute__((visibility("default")))
#  define AUTONOMOUS_FOUNDRY_IMPORT __attribute__((visibility("default")))
#  define AUTONOMOUS_FOUNDRY_LOCAL __attribute__((visibility("hidden")))
#endif

#if defined(AUTONOMOUS_FOUNDRY_SHARED)
#  if defined(AUTONOMOUS_FOUNDRY_BUILDING)
#    define AUTONOMOUS_FOUNDRY_API AUTONOMOUS_FOUNDRY_EXPORT
#  else
#    define AUTONOMOUS_FOUNDRY_API AUTONOMOUS_FOUNDRY_IMPORT
#  endif
#else
#  define AUTONOMOUS_FOUNDRY_API
#endif
