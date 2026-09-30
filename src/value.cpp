#include "value.hpp"
#include "util.hpp"

namespace sc {

std::optional<int64_t> parse_i64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return std::nullopt;
    uint64_t mag = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        if (mag > 922337203685477580ull) return std::nullopt; // early overflow guard
        mag = mag * 10 + uint64_t(s[i] - '0');
        if (mag > 9223372036854775807ull + (neg ? 1ull : 0ull)) return std::nullopt;
    }
    int64_t out = neg ? -int64_t(mag == 9223372036854775808ull ? 0 : mag) : int64_t(mag);
    if (mag == 9223372036854775808ull) {
        if (!neg) return std::nullopt;
        out = INT64_MIN;
    }
    return out;
}

Value Value::apply_affinity(ColType col, const std::string& column_name) const {
    if (is_null()) return Value::null();
    if (col == ColType::Integer) {
        if (is_int()) return *this;
        // TEXT -> INTEGER when the text is a well-formed integer.
        if (auto n = parse_i64(as_text())) return Value::integer(*n);
        throw DbError::type(str("cannot store ", type_name(), " value '", display(),
                                "' in INTEGER column '", column_name, "'"));
    }
    // TEXT column
    if (is_text()) return *this;
    return Value::text(std::to_string(as_int()));
}

} // namespace sc
