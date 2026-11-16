/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Library-level version and ABI interface.
 */

#ifndef OMNI_SUBOP_VERSION_H
#define OMNI_SUBOP_VERSION_H

#include <stdint.h>

#include "export.h"

/* Increment this when the shared C ABI changes incompatibly. */
#define OMNI_SUBOP_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

OMNI_SUBOP_API uint32_t omni_subop_abi_version(void) OMNI_SUBOP_NOEXCEPT;
OMNI_SUBOP_API const char* omni_subop_version(void) OMNI_SUBOP_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* OMNI_SUBOP_VERSION_H */
