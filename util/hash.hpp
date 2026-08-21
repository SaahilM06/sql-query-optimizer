#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sql::util {

// 64-bit FNV-1a. Not cryptographic -- this is only ever used to build cache
// keys, where the versioned key format (schema/stats version segments) plus
// TTL expiry already bound the blast radius of a hypothetical collision.
inline uint64_t fnv1a64(std::string_view data) {
    constexpr uint64_t kOffsetBasis = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffsetBasis;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= kPrime;
    }
    return hash;
}

inline std::string hex64(uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
    return out;
}

} // namespace sql::util
