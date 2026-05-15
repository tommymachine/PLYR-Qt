// Static lookup table mapping (drive vendor, drive product) to the
// AccurateRip drive read offset, in stereo sample frames.
//
// The authoritative source is <https://www.accuraterip.com/driveoffsets.htm>
// — an HTML table maintained by the AccurateRip submitter community. The
// v1 of this DB is a starter set seeded only with the drive(s) we've
// empirically verified against; populating from the upstream table is a
// follow-up task (one-time scrape, hand-clean, embed as JSON).
//
// Offset sign convention follows AccurateRip: the value is the number of
// sample frames our read is shifted relative to the canonical pressing.
// Apply at rip time by reading `offset` extra frames at one disc edge
// and slicing the canonical-aligned window — the verifier's offset scan
// is then expected to report `AR offset: 0`.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace plyr::cd {

// Lookup by canonicalized vendor / product strings (uppercase, trimmed,
// internal-whitespace collapsed — the form CdDevice::DriveInfo already
// uses). Returns nullopt if the drive isn't in the bundled table.
std::optional<int> lookupDriveOffset(const std::string& vendor,
                                     const std::string& product);

// Number of entries in the bundled AccurateRip table (excludes local
// overrides). For diagnostics — e.g. `cdrip_cli --db-info` showing
// "4800 drives bundled" so a user can confirm the resource embedded.
int driveOffsetTableSize();

// Random-ish sample of bundled drive names for `--db-info` to print.
// Up to `n` entries. For diagnostics only.
std::vector<std::string> sampleDriveNames(int n);

} // namespace plyr::cd
