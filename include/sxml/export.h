#pragma once

/*
 * Symbol visibility for the public interface.
 *
 * Every public function declaration is marked SXML_EXPORT. The build defines
 * SXML_BUILDING_LIBRARY while compiling the library itself and SXML_SHARED
 * when the library is a shared library (DLL on Windows); consumers of a shared
 * library define SXML_SHARED too (the CMake target does this for them).
 * Project code is compiled with hidden visibility, so only symbols marked here
 * are exported from a shared library.
 */

#if defined(SXML_SHARED)
#if defined(_WIN32)
#if defined(SXML_BUILDING_LIBRARY)
#define SXML_EXPORT __declspec(dllexport)
#else
#define SXML_EXPORT __declspec(dllimport)
#endif
#else
#define SXML_EXPORT __attribute__((visibility("default")))
#endif
#else
#define SXML_EXPORT
#endif
