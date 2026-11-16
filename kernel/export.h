/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Export and calling-convention helpers for the shared micro-operator API.
 */

#ifndef OMNI_SUBOP_EXPORT_H
#define OMNI_SUBOP_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(OMNI_SUBOP_STATIC)
#define OMNI_SUBOP_API
#elif defined(OMNI_SUBOP_BUILD)
#define OMNI_SUBOP_API __declspec(dllexport)
#else
#define OMNI_SUBOP_API __declspec(dllimport)
#endif
#else
#if defined(OMNI_SUBOP_BUILD) && defined(__GNUC__)
#define OMNI_SUBOP_API __attribute__((visibility("default")))
#else
#define OMNI_SUBOP_API
#endif
#endif

#ifdef __cplusplus
#define OMNI_SUBOP_NOEXCEPT noexcept
#else
#define OMNI_SUBOP_NOEXCEPT
#endif

#endif /* OMNI_SUBOP_EXPORT_H */
