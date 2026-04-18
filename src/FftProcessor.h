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
#include <array>
#include <vector>

class QAudioBuffer;
class QAudioFormat;

class FftProcessor : public QObject {
    Q_OBJECT
public:
    static constexpr int BAND_COUNT = 16;
    static constexpr int FFT_SIZE   = 2048;

    explicit FftProcessor(QObject* parent = nullptr);
    ~FftProcessor() override;

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
    std::array<float, BAND_COUNT> m_bands {};
    std::array<float, BAND_COUNT> m_peaks {};
    std::array<int,   BAND_COUNT> m_peakHoldFrames {};
};
