#include "AcoustIdProvider.h"

namespace concerto::metadata {

// Scaffold: returns nullopt unconditionally. The future implementation
// vendors Chromaprint (MIT, vDSP backend) to produce the fingerprint
// from track-1 PCM held in RipWorker, then issues the AcoustID GET
// described in METADATA_PIPELINE_AUTOMATED.md "AcoustID call shape".
// Until then the pipe degrades to Stage 4 (stub).
std::optional<AcoustIdMatch> AcoustIdProvider::lookup(
    const QByteArray& /*fingerprint*/, int /*durationSec*/)
{
    return std::nullopt;
}

} // namespace concerto::metadata
