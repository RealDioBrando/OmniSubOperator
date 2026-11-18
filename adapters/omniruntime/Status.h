#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_STATUS_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_STATUS_H

#include <cstdint>
#include <string>

#include "kernel/sum_batch.h"
#include "util/omni_exception.h"

namespace omni::subop::omniruntime {

/*
 * The integration boundary may fall back only when the shared path explicitly
 * reports an unsupported shape. Invalid state, invalid arguments, arithmetic
 * overflow, and internal errors are fatal: allowing the engine to replay the
 * batch after one of those statuses would hide a partial-write or corruption
 * bug.
 */
inline void CheckSharedStatus(int32_t status, const char* operation) {
    if (status == omni::subop::SUM_OK ||
        status == omni::subop::SUM_UNSUPPORTED) {
        return;
    }
    throw ::omniruntime::exception::OmniException(
        "OMNI_SUBOP_SHARED_KERNEL_ERROR",
        std::string("OmniSubOperator ") + operation +
            " kernel failed with status " + std::to_string(status));
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_STATUS_H */