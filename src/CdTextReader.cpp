#include "CdTextReader.h"

namespace concerto::metadata {

// Scaffold: returns nullopt unconditionally. The future macOS path goes
// through `IOServiceOpen(cdMedia, ...)` + a user-client call that lands
// on `IOCDMedia::readTOC(buffer, 0x05, ...)`, then walks the returned
// pack stream per METADATA_PIPELINE_AUTOMATED.md "CD-TEXT via MMC".
// Until that lands, callers fall through to Stage 3 / Stage 4.
std::optional<CdTextMeta> CdTextReader::read(const QString& /*devicePath*/)
{
    return std::nullopt;
}

} // namespace concerto::metadata
