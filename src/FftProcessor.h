// Consumes QAudioBuffers, produces 16 smoothed band magnitudes for the
// visualizer. Mirrors the Swift FFTProcessor.
//
// Audio-thread side: `pushBuffer(const QAudioBuffer&)` copies samples
// into a ring buffer under a short mutex.
// Render-thread side: `bands()` reads the latest samples out of the
// ring, runs a 2048-point FFT via KissFFT, folds the magnitudes into
// 16 log-spaced bands (30Hz–16kHz), applies dB scaling, and updates
// peak-hold values.

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QVariantList>
#include <QVector4D>
#include <array>
#include <vector>

class QAudioBuffer;
class QAudioFormat;

class FftProcessor : public QObject {
    Q_OBJECT

    // Bands and peaks re-packaged as four vec4s each, for the shader-based
    // visualizer. Each QVector4D binds 1:1 to a std140 vec4 uniform.
    // `updated()` is emitted by `fillBandsAndPeaks()`, so QML bindings
    // on these properties re-evaluate every tick that refresh() fires.
    Q_PROPERTY(QVector4D b0 READ b0 NOTIFY updated)
    Q_PROPERTY(QVector4D b1 READ b1 NOTIFY updated)
    Q_PROPERTY(QVector4D b2 READ b2 NOTIFY updated)
    Q_PROPERTY(QVector4D b3 READ b3 NOTIFY updated)
    Q_PROPERTY(QVector4D p0 READ p0 NOTIFY updated)
    Q_PROPERTY(QVector4D p1 READ p1 NOTIFY updated)
    Q_PROPERTY(QVector4D p2 READ p2 NOTIFY updated)
    Q_PROPERTY(QVector4D p3 READ p3 NOTIFY updated)

    // EQ-panel tap: 10-band energy + peak-hold at the ISO octave centers
    // that match the parametric EQ. Same FFT snapshot as the 16-band list,
    // just re-integrated into 10 half-octave windows.
    Q_PROPERTY(QVariantList eqBands READ eqBandsList NOTIFY updated)
    Q_PROPERTY(QVariantList eqPeaks READ eqPeaksList NOTIFY updated)

public:
    static constexpr int BAND_COUNT    = 16;
    static constexpr int EQ_BAND_COUNT = 10;
    static constexpr int FFT_SIZE      = 2048;

    explicit FftProcessor(QObject* parent = nullptr);
    ~FftProcessor() override;

    // Call from QML at the desired frame rate. Runs the FFT, updates
    // smoothed bands + peak-hold values, emits `updated()` so shader
    // uniforms refresh.
    Q_INVOKABLE void refresh() {
        float tmp[2 * BAND_COUNT];
        fillBandsAndPeaks(tmp);  // also emits `updated()`
    }

    QVector4D b0() const { return {m_bands[ 0], m_bands[ 1], m_bands[ 2], m_bands[ 3]}; }
    QVector4D b1() const { return {m_bands[ 4], m_bands[ 5], m_bands[ 6], m_bands[ 7]}; }
    QVector4D b2() const { return {m_bands[ 8], m_bands[ 9], m_bands[10], m_bands[11]}; }
    QVector4D b3() const { return {m_bands[12], m_bands[13], m_bands[14], m_bands[15]}; }
    QVector4D p0() const { return {m_peaks[ 0], m_peaks[ 1], m_peaks[ 2], m_peaks[ 3]}; }
    QVector4D p1() const { return {m_peaks[ 4], m_peaks[ 5], m_peaks[ 6], m_peaks[ 7]}; }
    QVector4D p2() const { return {m_peaks[ 8], m_peaks[ 9], m_peaks[10], m_peaks[11]}; }
    QVector4D p3() const { return {m_peaks[12], m_peaks[13], m_peaks[14], m_peaks[15]}; }

    // Call on the audio thread whenever a new buffer has been rendered.
    // Internally converts to mono float, stashes in the ring under a
    // short lock, and returns.
    void pushBuffer(const QAudioBuffer& buf);

    // Raw-PCM variant: called from PcmPipe as the sink pulls audio
    // through. `bytes` must be a multiple of `fmt.bytesPerSample()` *
    // `fmt.channelCount()`.
    void pushPcm(const char* data, qint64 bytes, const QAudioFormat& fmt);

    // Return 16 smoothed band magnitudes (0..1) followed by 16 peak-hold
    // values (0..1) — 32 floats total. Call from the render thread.
    // Runs the FFT each call; designed to be cheap enough at 60 Hz.
    Q_INVOKABLE QVariantList bandsAndPeaks();

    // Non-QVariant fast path for the render thread.
    //   `out` must have room for 2 * BAND_COUNT floats.
    //   Returns true if the buffer was filled.
    bool fillBandsAndPeaks(float* out);

    // Read-sides for the EQ-band property bindings.
    QVariantList eqBandsList() const;
    QVariantList eqPeaksList() const;

signals:
    void updated();     // emit when bands change

private:
    // Ring buffer for raw mono samples.
    std::vector<float> m_ring;
    int                m_writeIndex = 0;
    QMutex             m_ringMutex;

    // Scratch + FFT handle + Hann window.
    std::vector<float> m_snapshot;
    std::vector<float> m_windowed;
    std::vector<float> m_window;
    struct FftImpl;
    std::unique_ptr<FftImpl> m_fft;

    // Smoothed state (render-thread-only).
    std::array<float, BAND_COUNT>    m_bands {};
    std::array<float, BAND_COUNT>    m_peaks {};
    std::array<int,   BAND_COUNT>    m_peakHoldFrames {};

    // Parallel state for the EQ's 10 ISO-octave tap.
    std::array<float, EQ_BAND_COUNT> m_eqBands {};
    std::array<float, EQ_BAND_COUNT> m_eqPeaks {};
    std::array<int,   EQ_BAND_COUNT> m_eqPeakHoldFrames {};
};
