// SsmGenerator -- offline self-similarity-matrix generator (Layer 4b).
//
// Reads a FLAC, frames it into ~0.5 s windows (or 2 s for tracks longer
// than 10 min so the matrix stays bounded), extracts an MFCC vector per
// frame, and writes a quantised cosine-similarity matrix to a sidecar
// {flacPath}.ssm file. The visualizer layer loads the sidecar and renders
// it as the playback scrubber.
//
// MFCC pipeline mirrors MfccAnalyzer's recipe (Davis & Mermelstein 1980,
// Slaney mel scale, type-II DCT with ortho normalisation, M=40 filters,
// 13 coefficients). The code is intentionally a clean duplicate -- the
// live analyzer is QObject-bound and inherits AudioFeatures plumbing we
// don't want here, and refactoring it to share the inner pipeline would
// have entangled two non-trivial classes. The duplication is ~50 lines
// of arithmetic; clean-room rewrite of the same canonical formulas.
//
// Sidecar binary layout (little-endian):
//   bytes  0..3   : magic = "SSM1"           (4 bytes, ASCII)
//   bytes  4..7   : version = 1              (uint32)
//   bytes  8..11  : T (frame count)          (uint32)
//   bytes 12..15  : hopSec                    (float32)
//   bytes 16..31  : reserved, zero-filled    (16 bytes)
//   bytes 32..    : T*T similarity bytes      (row-major, S[i,j] in 0..255)
//
// Cell quantisation: cosine similarity in [-1, +1] -> 0..255 via
//   byte = round(clamp(0.5 + 0.5 * cos, 0, 1) * 255).
// 0 = orthogonal/anti-correlated; 128 ~ neutral; 255 = identical frame.
// The diagonal is always 255.
//
// Thread model: blocking on the calling thread. Expect ~5-10 s for a
// 4-minute song on a modern desktop core. The caller is expected to run
// the generator on a background thread (QtConcurrent or similar).

#pragma once

#include <QString>
#include <cstdint>
#include <vector>

class SsmGenerator {
public:
    // Hard cap on the matrix dimension. For T=2048 the sidecar is
    // 32 + 2048*2048 = ~4 MB on disk, which we deem the comfortable
    // upper bound for a per-track sidecar. Tracks longer than ~17 min
    // at 0.5 s hop hit the cap; the generator switches to a 2 s hop
    // first (covers up to ~68 min), then truncates the analysis window
    // to keep T <= MAX_T.
    static constexpr int   MAX_T = 2048;
    static constexpr float kShortHopSec      = 0.5f;
    static constexpr float kLongHopSec       = 2.0f;
    // Threshold for switching from short to long hop. Picked so that a
    // 10 min track at 0.5 s hop produces T = 1200 (still well under
    // MAX_T), but a 12 min track at 0.5 s would land at T = 1440 --
    // still fine, but the matrix density at 0.5 s is overkill for
    // long-form material. Anything past 10 min decimates to 2 s.
    static constexpr double kLongHopThreshSec = 600.0;
    // MFCC analysis window length in samples (matches MfccAnalyzer).
    static constexpr int   kFrameSamples     = 2048;
    static constexpr int   kNumMelFilters    = 40;
    // We drop coefficient 0 (overall loudness) when computing cosine
    // similarity -- it's dominated by gain and would mask the timbral
    // signal we actually care about. So the SSM lives in a 12-D space.
    static constexpr int   kNumCoeffs        = 13;
    static constexpr int   kFeatureDim       = kNumCoeffs - 1;
    // Mel filterbank coverage. Same as MfccAnalyzer; 80 Hz to 8 kHz
    // brackets the musically interesting energy on most material.
    static constexpr float kMelFLow          = 80.0f;
    static constexpr float kMelFHigh         = 8000.0f;

    struct Stats {
        int     T        = 0;
        float   hopSec   = 0.0f;
        double  decodeSec      = 0.0;   // FLAC decode wall time
        double  mfccSec        = 0.0;   // T frames of MFCC extraction
        double  similaritySec  = 0.0;   // T*T cosine similarity compute
        double  writeSec       = 0.0;   // sidecar serialisation
        bool    success  = false;
        QString error;
    };

    // Synchronous generator. If sidecarPath is empty, writes to
    // flacPath + ".ssm". Returns Stats with success=false and the
    // diagnostic message on error.
    static Stats generateForFile(const QString& flacPath,
                                 const QString& sidecarPath = QString());

    // Verify a sidecar's magic + version match. Fast (header only).
    static bool isValidSidecar(const QString& sidecarPath);

    // ----- Verification-harness entry points (no FLAC dependency) ----------
    //
    // Generate an SSM from a mono float buffer in memory. Used by the
    // synthetic ABA test: skip the decode + write hop entirely, hand
    // back the quantised matrix as bytes.
    //
    // Hop selection follows the same rules as generateForFile().
    // The output vector is sized T*T and indexed row-major.
    static Stats generateFromMonoSamples(const std::vector<float>& mono,
                                         double sampleRate,
                                         std::vector<uint8_t>& matrixOut);

    // Compute one MFCC vector from a window of mono samples. Exposed for
    // tests; production code calls it implicitly via the path above.
    // Returns kNumCoeffs floats. The first (DC / loudness) coefficient
    // is included for completeness; cosine similarity ignores it.
    static void debugMfccForWindow(const float* mono, int n,
                                   double sampleRate,
                                   float* outCoeffs /* size kNumCoeffs */);

    // Write a pre-computed matrix to disk in the sidecar format. Returns
    // false if the path is unwritable. Used by tests and by the file-
    // backed path internally.
    static bool writeSidecar(const QString& sidecarPath,
                             int T, float hopSec,
                             const uint8_t* matrix /* T*T bytes */);

    // Read a sidecar into memory. Returns false if the magic / version /
    // size don't add up. matrixOut is resized to T*T on success.
    struct SidecarHeader {
        uint32_t version = 0;
        uint32_t T       = 0;
        float    hopSec  = 0.0f;
    };
    static bool readSidecar(const QString& sidecarPath,
                            SidecarHeader& hdrOut,
                            std::vector<uint8_t>& matrixOut);
};
