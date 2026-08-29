#pragma once
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace espressolab {

// Section 12.2: errors carry a stable code and a dotted path so the dashboard
// can point at the offending control instead of showing a bare message.
struct ValidationIssue {
    std::string code;
    std::string message;
    std::string path;
};

class ValidationResult {
public:
    void add(std::string code, std::string message, std::string path) {
        issues_.push_back({std::move(code), std::move(message), std::move(path)});
    }
    void merge(const ValidationResult& other) {
        issues_.insert(issues_.end(), other.issues_.begin(), other.issues_.end());
    }
    [[nodiscard]] bool ok() const { return issues_.empty(); }
    [[nodiscard]] const std::vector<ValidationIssue>& issues() const { return issues_; }
    [[nodiscard]] std::string summary() const;

private:
    std::vector<ValidationIssue> issues_;
};

// Thrown for input that fails validation before any work happens. Declared here
// rather than beside the solver so that modules outside the shot pipeline (the
// grinder, and anything else that validates its own spec) can raise the same
// error shape without linking espressolab_core.
class InvalidInputError : public std::runtime_error {
public:
    explicit InvalidInputError(const ValidationResult& result);
    [[nodiscard]] const ValidationResult& validation() const { return validation_; }

private:
    ValidationResult validation_;
};

// Range helper used by recipe and coefficient validation. Inclusive bounds.
void require_in_range(ValidationResult& result, double value, double low, double high,
                      const char* path, const char* unit);
void require_positive(ValidationResult& result, double value, const char* path);
// Finite and >= 0 (unlike require_positive, 0 itself is allowed).
void require_nonnegative(ValidationResult& result, double value, const char* path);
// Finite only, no sign or range constraint.
void require_finite(ValidationResult& result, double value, const char* path);

}  // namespace espressolab
