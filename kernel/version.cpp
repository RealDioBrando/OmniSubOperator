/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 */

#include "version.h"

#ifndef OMNI_SUBOP_VERSION_STRING
#define OMNI_SUBOP_VERSION_STRING "0.1.0"
#endif

extern "C" uint32_t omni_subop_abi_version(void) noexcept {
    return OMNI_SUBOP_ABI_VERSION;
}

extern "C" const char* omni_subop_version(void) noexcept {
    return OMNI_SUBOP_VERSION_STRING;
}
