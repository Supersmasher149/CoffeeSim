#pragma once

#include <functional>
#include <stdexcept>

namespace espressolab {

// Execution controls are deliberately separate from serialized solver inputs.
// Callers can stop a long-running calculation without changing its identity.
using CancellationCallback = std::function<bool()>;

class ExecutionCancelled final : public std::runtime_error {
public:
    ExecutionCancelled() : std::runtime_error("execution cancelled") {}
};

inline void throw_if_cancelled(const CancellationCallback& is_cancelled) {
    if (is_cancelled && is_cancelled()) throw ExecutionCancelled();
}

}  // namespace espressolab
