// =============================================================================
// sha256.h -- the SHA-256 of a file (checking an update's download)
// =============================================================================
//
// An update's release.toml carries the archive's SHA-256 (tools/
// make_release.sh); the launcher checks the download against it before
// unpacking anything (update.h). Computed here, the standard algorithm
// (FIPS 180-4), so no outside tool is needed (Windows has no sha256sum).
// =============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace sha256 {

// The file's SHA-256 as 64 lower-case hex digits; empty if it can't be read.
std::string OfFile(const std::filesystem::path& file);

}  // namespace sha256
