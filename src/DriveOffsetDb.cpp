#include "DriveOffsetDb.h"

#include <string_view>

namespace plyr::cd {

namespace {

struct Entry {
    std::string_view vendor;   // already canonical: UPPER, trimmed
    std::string_view product;
    int              offset;
};

// Starter table. Every entry must be (canonicalized vendor, canonicalized
// product, signed sample-frame offset).
//
// * APPLE / SUPERDRIVE @ -6: empirical observation from the verifier's
//   AR offset scan on a Ravel Edition disc 2 rip (no-offset). Matches
//   the order-of-magnitude expectation for slim-USB SuperDrives; we'll
//   confirm against AR's canonical value when the table import lands.
constexpr Entry kTable[] = {
    {"APPLE", "SUPERDRIVE", -6},
};

} // namespace

std::optional<int> lookupDriveOffset(const std::string& vendor,
                                     const std::string& product) {
    for (const auto& e : kTable) {
        if (vendor == e.vendor && product == e.product) return e.offset;
    }
    return std::nullopt;
}

} // namespace plyr::cd
