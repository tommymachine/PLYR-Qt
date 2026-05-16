// MilkdropView — see MilkdropView.h for the design intent.
//
// RHI plumbing pattern follows ScopeRenderer's three-pass design:
// dynamic buffers built per-frame, persistent textures and pipelines
// created once, ping-pong managed by the renderer-side instance.
//
// Resource layout:
//   * Two RGBA8 ping-pong textures (m_warp[0/1]) sized to the item.
//   * One per-direction TextureRenderTarget (m_warpRt[0/1]).
//   * One shared linear-clamp sampler.
//   * Warp pipeline: 32×24 mesh, per-vertex UV uploaded as a separate
//     dynamic vertex buffer (paired with an immutable position buffer).
//   * Composite pipeline: fullscreen triangle, samples the just-written
//     warp texture, writes to renderTarget().
//
// The runtime lives on the render thread (member of MilkdropViewImpl);
// preset reloads happen at the start of each render() if the item-side
// flag is set. The runtime's compile/parse path is heavy but only fires
// on preset change, so jitter is bounded.
//
// References used while writing this:
//   * QQuickRhiItem docs (Qt 6.7) for the render-thread lifecycle.
//   * Existing ScopeRenderer.cpp as a known-good template.
//   * Geiss's MilkDrop preset spec for the per-frame / per-vertex
//     semantics in MilkdropRuntime.

#include "MilkdropView.h"
#include "MilkdropRuntime.h"
#include "AudioFeatures.h"

#include <QFile>
#include <QUrl>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

// Mesh resolution. Geiss's reference renderer uses 32×24 for the
// low-detail preset preview; 48×32 for the live view. v1 ships 32×24
// because each (i, j) costs one projectm-eval execution and 768 calls
// per frame is comfortable.
constexpr int kMeshW = 32;
constexpr int kMeshH = 24;
constexpr int kMeshVertexCount = kMeshW * kMeshH;

// 2 floats per position, 2 floats per UV. Positions are static (the
// grid never moves); UVs are dynamic (recomputed every frame).
constexpr int kPosBufBytes = kMeshVertexCount * 2 * int(sizeof(float));
constexpr int kUvBufBytes  = kMeshVertexCount * 2 * int(sizeof(float));

// Indexed triangle list. Per (kMeshW-1) × (kMeshH-1) cell, two
// triangles = 6 indices. With 31 × 23 = 713 cells, 4278 indices.
constexpr int kCellCount = (kMeshW - 1) * (kMeshH - 1);
constexpr int kIndexCount = kCellCount * 6;
constexpr int kIndexBufBytes = kIndexCount * int(sizeof(uint16_t));

// Std140 UBO layouts. Order MUST match the layout blocks in the
// matching .vert / .frag shaders.

struct WarpUbo {
    float qt_Matrix[16];   // off  0, mat4
    float qt_Opacity;      // off 64
    float decay;           // off 68
    float darkenCenter;    // off 72
    float _pad0;           // off 76 — align next vec4 to 16
    float waveColor[4];    // off 80, vec4
};                         // total: 96 B

struct CompositeUbo {
    float qt_Matrix[16];   // off  0
    float qt_Opacity;      // off 64
    float gamma;           // off 68
    float _pad0;           // off 72
    float _pad1;           // off 76
};                         // total: 80 B  (must be a multiple of 16:
                           //                Qt's UBO size alignment).
                           //                Actually 80 % 16 == 0, so OK.

constexpr std::array<float, 16> kIdentity4 = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
};

inline void writeIdentity(float* dst) {
    std::memcpy(dst, kIdentity4.data(), 16 * sizeof(float));
}

QShader loadShader(const QString& resourcePath)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

// QFile-friendly resolution for paths that may be qrc:/-prefixed URLs.
QString resolvePresetPath(const QString& s)
{
    if (s.startsWith(QStringLiteral("qrc:"))) {
        QUrl u(s);
        // QFile treats `:/path` as a resource. The Qt QFile API
        // accepts both `qrc:/path` and `:/path` since 6.x, but be
        // explicit so the codepath is the same on every platform.
        QString p = u.path();
        if (!p.startsWith(QChar(':'))) p.prepend(QChar(':'));
        return p;
    }
    if (s.startsWith(QStringLiteral("file:"))) {
        return QUrl(s).toLocalFile();
    }
    return s;
}

}  // namespace


// ---------------------------------------------------------------------------
//  MilkdropViewImpl — runs on the render thread
// ---------------------------------------------------------------------------

class MilkdropViewImpl : public QQuickRhiItemRenderer {
public:
    explicit MilkdropViewImpl(MilkdropView* item) : m_item(item) {}

    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void releaseWarpResources();
    void ensureWarpResources(const QSize& size);
    void uploadStaticBuffers(QRhiResourceUpdateBatch* upload);

    MilkdropView* m_item = nullptr;
    QRhi*         m_rhi  = nullptr;
    QSize         m_pixelSize;

    // The expression-runtime instance. Lives on the render thread.
    std::unique_ptr<MilkdropRuntime> m_runtime;

    // Render-thread snapshot of staged state from the GUI thread.
    MilkdropView::StagedAudio m_audio;
    QString m_pendingPresetPath;
    bool    m_presetReloadRequested = false;
    bool    m_runtimeReady = false;
    QString m_lastReportedError;     // set after a load attempt, reflected to item

    // Frame-time tracking for runtime.advance().
    std::chrono::steady_clock::time_point m_lastTick{};
    bool m_havePrevTick = false;

    // Persistent GPU resources.
    QRhiSampler*               m_linearSampler = nullptr;

    // Warp ping-pong.
    QRhiTexture*               m_warp[2]   = { nullptr, nullptr };
    QRhiTextureRenderTarget*   m_warpRt[2] = { nullptr, nullptr };
    QRhiRenderPassDescriptor*  m_warpRpDesc = nullptr;
    int                        m_readIdx  = 0;
    int                        m_writeIdx = 1;
    bool                       m_warpPrimed = false;

    // Warp pipeline.
    QRhiBuffer*               m_posVB  = nullptr;
    QRhiBuffer*               m_uvVB   = nullptr;
    QRhiBuffer*               m_meshIB = nullptr;
    QRhiBuffer*               m_warpUbo = nullptr;
    QRhiShaderResourceBindings* m_warpSrb[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*      m_warpPipe = nullptr;

    // Composite pipeline.
    QRhiBuffer*               m_fsVB = nullptr;
    QRhiBuffer*               m_compositeUbo = nullptr;
    QRhiShaderResourceBindings* m_compositeSrb[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*      m_compositePipe = nullptr;
    QRhiRenderPassDescriptor*  m_compositeRpDesc = nullptr;

    bool m_staticUploaded = false;

    // Scratch buffer for the per-frame UV table from MilkdropRuntime.
    // Allocated once; reused every frame so the render-thread hot path
    // is allocation-free.
    std::array<float, kMeshVertexCount * 2> m_uvScratch{};
};


// ---------------------------------------------------------------------------
//  Resource lifetime
// ---------------------------------------------------------------------------

void MilkdropViewImpl::initialize(QRhiCommandBuffer* /*cb*/)
{
    m_rhi = rhi();
    if (!m_rhi) return;

    if (!m_runtime) {
        m_runtime = std::make_unique<MilkdropRuntime>();
    }

    if (!m_linearSampler) {
        m_linearSampler = m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        m_linearSampler->create();
    }

    if (!m_posVB) {
        m_posVB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                   QRhiBuffer::VertexBuffer, kPosBufBytes);
        m_posVB->create();
    }
    if (!m_uvVB) {
        m_uvVB = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                  QRhiBuffer::VertexBuffer, kUvBufBytes);
        m_uvVB->create();
    }
    if (!m_meshIB) {
        m_meshIB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                    QRhiBuffer::IndexBuffer, kIndexBufBytes);
        m_meshIB->create();
    }
    if (!m_warpUbo) {
        m_warpUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                     QRhiBuffer::UniformBuffer,
                                     sizeof(WarpUbo));
        m_warpUbo->create();
    }
    if (!m_compositeUbo) {
        m_compositeUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                          QRhiBuffer::UniformBuffer,
                                          sizeof(CompositeUbo));
        m_compositeUbo->create();
    }
    if (!m_fsVB) {
        m_fsVB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                  QRhiBuffer::VertexBuffer,
                                  12 * sizeof(float));
        m_fsVB->create();
    }
}

void MilkdropViewImpl::releaseWarpResources()
{
    for (int i = 0; i < 2; ++i) {
        if (m_warpRt[i])     { m_warpRt[i]->destroy();     delete m_warpRt[i];     m_warpRt[i] = nullptr; }
        if (m_warp[i])       { m_warp[i]->destroy();       delete m_warp[i];       m_warp[i]   = nullptr; }
        if (m_warpSrb[i])    { m_warpSrb[i]->destroy();    delete m_warpSrb[i];    m_warpSrb[i] = nullptr; }
        if (m_compositeSrb[i]) { m_compositeSrb[i]->destroy(); delete m_compositeSrb[i]; m_compositeSrb[i] = nullptr; }
    }
    if (m_warpRpDesc) { m_warpRpDesc->destroy(); delete m_warpRpDesc; m_warpRpDesc = nullptr; }
    m_warpPrimed = false;
}

void MilkdropViewImpl::ensureWarpResources(const QSize& size)
{
    if (!m_rhi || size.width() <= 0 || size.height() <= 0) return;
    if (m_warp[0] && m_pixelSize == size) return;

    releaseWarpResources();
    m_pixelSize = size;

    for (int i = 0; i < 2; ++i) {
        m_warp[i] = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                      QRhiTexture::RenderTarget);
        m_warp[i]->create();
        QRhiTextureRenderTargetDescription desc{ QRhiColorAttachment(m_warp[i]) };
        m_warpRt[i] = m_rhi->newTextureRenderTarget(desc);
        if (!m_warpRpDesc) {
            m_warpRpDesc = m_warpRt[i]->newCompatibleRenderPassDescriptor();
        }
        m_warpRt[i]->setRenderPassDescriptor(m_warpRpDesc);
        m_warpRt[i]->create();
    }

    // SRBs: warp[write] samples warp[read]. composite[write] also
    // samples warp[write], because composite runs after the warp pass.
    for (int i = 0; i < 2; ++i) {
        const int readIdx = 1 - i;

        m_warpSrb[i] = m_rhi->newShaderResourceBindings();
        m_warpSrb[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_warpUbo),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_warp[readIdx], m_linearSampler)
        });
        m_warpSrb[i]->create();

        m_compositeSrb[i] = m_rhi->newShaderResourceBindings();
        m_compositeSrb[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_compositeUbo),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_warp[i],   // index `i` = the texture we just wrote
                m_linearSampler)
        });
        m_compositeSrb[i]->create();
    }

    m_readIdx  = 0;
    m_writeIdx = 1;
    m_warpPrimed = false;
}

void MilkdropViewImpl::uploadStaticBuffers(QRhiResourceUpdateBatch* upload)
{
    if (m_staticUploaded) return;

    // Mesh position buffer — 32×24 vertices in clip-space [-1, +1]^2.
    std::vector<float> pos(kMeshVertexCount * 2);
    for (int j = 0; j < kMeshH; ++j) {
        const float fy = (kMeshH > 1) ? float(j) / float(kMeshH - 1) : 0.5f;
        const float ny = fy * 2.0f - 1.0f;
        for (int i = 0; i < kMeshW; ++i) {
            const float fx = (kMeshW > 1) ? float(i) / float(kMeshW - 1) : 0.5f;
            const float nx = fx * 2.0f - 1.0f;
            const int idx = 2 * (j * kMeshW + i);
            pos[idx + 0] = nx;
            pos[idx + 1] = ny;
        }
    }
    upload->uploadStaticBuffer(m_posVB, 0, kPosBufBytes, pos.data());

    // Mesh index buffer — two triangles per cell, CCW. With
    // setMirrorVertically(true) on the item the runtime UV table is
    // also in top-left origin, so the rendered orientation matches.
    std::vector<uint16_t> idx(kIndexCount);
    int k = 0;
    for (int j = 0; j < kMeshH - 1; ++j) {
        for (int i = 0; i < kMeshW - 1; ++i) {
            const uint16_t a = uint16_t(j * kMeshW + i);
            const uint16_t b = uint16_t(j * kMeshW + i + 1);
            const uint16_t c = uint16_t((j + 1) * kMeshW + i);
            const uint16_t d = uint16_t((j + 1) * kMeshW + i + 1);
            // Triangle 1: a, b, c. Triangle 2: c, b, d.
            idx[k++] = a; idx[k++] = b; idx[k++] = c;
            idx[k++] = c; idx[k++] = b; idx[k++] = d;
        }
    }
    upload->uploadStaticBuffer(m_meshIB, 0, kIndexBufBytes, idx.data());

    // Fullscreen quad for the composite pass (six positions, no UV;
    // the vert derives UV from gl_Position).
    static const float fs[12] = {
        -1.f, -1.f,   1.f, -1.f,   -1.f,  1.f,
        -1.f,  1.f,   1.f, -1.f,    1.f,  1.f
    };
    upload->uploadStaticBuffer(m_fsVB, 0, sizeof(fs), fs);

    m_staticUploaded = true;
}


// ---------------------------------------------------------------------------
//  synchronize — GUI ⇄ render-thread state copy
// ---------------------------------------------------------------------------

void MilkdropViewImpl::synchronize(QQuickRhiItem* item)
{
    auto* mv = static_cast<MilkdropView*>(item);

    if (mv->m_audioDirty.exchange(false, std::memory_order_acquire)) {
        QMutexLocker lk(&mv->m_stagedMutex);
        m_audio = mv->m_staged;
    }

    if (mv->m_presetDirty.exchange(false, std::memory_order_acquire)) {
        m_pendingPresetPath = mv->m_presetPath;
        m_presetReloadRequested = true;
    }

    // Bubble any error from the runtime back into the item-side
    // Q_PROPERTY. We do this *after* render() has had a chance to
    // attempt loading, so the m_lastError reads come from a completed
    // attempt.
    if (mv->m_lastError != m_lastReportedError) {
        mv->m_lastError = m_lastReportedError;
        // The signal must be emitted from the QObject's owning thread;
        // synchronize() runs on the render thread, but Qt's GUI<->render
        // sync barrier guarantees the GUI thread is blocked here, so a
        // direct emit is safe.
        emit mv->lastErrorChanged();
    }
}


// ---------------------------------------------------------------------------
//  render
// ---------------------------------------------------------------------------

void MilkdropViewImpl::render(QRhiCommandBuffer* cb)
{
    if (!m_rhi) return;

    QRhiTexture* color = colorTexture();
    if (!color) return;
    const QSize size = color->pixelSize();

    ensureWarpResources(size);
    if (!m_warpRt[0]) return;

    // ---------- pipeline build (lazy, on first render) -----------------
    if (!m_warpPipe) {
        QShader warpVS = loadShader(QStringLiteral(":/shaders/milkdrop_warp.vert.qsb"));
        QShader warpFS = loadShader(QStringLiteral(":/shaders/milkdrop_warp.frag.qsb"));

        // Vertex layout: binding 0 = positions (vec2), binding 1 = uvs (vec2).
        QRhiVertexInputLayout warpLayout;
        warpLayout.setBindings({
            QRhiVertexInputBinding(2 * sizeof(float)),
            QRhiVertexInputBinding(2 * sizeof(float))
        });
        warpLayout.setAttributes({
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
            QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float2, 0)
        });

        m_warpPipe = m_rhi->newGraphicsPipeline();
        m_warpPipe->setTopology(QRhiGraphicsPipeline::Triangles);
        m_warpPipe->setShaderStages({
            { QRhiShaderStage::Vertex,   warpVS },
            { QRhiShaderStage::Fragment, warpFS }
        });
        m_warpPipe->setVertexInputLayout(warpLayout);
        m_warpPipe->setShaderResourceBindings(m_warpSrb[0]);
        m_warpPipe->setRenderPassDescriptor(m_warpRpDesc);
        QRhiGraphicsPipeline::TargetBlend tb;
        tb.enable = false;
        m_warpPipe->setTargetBlends({ tb });
        m_warpPipe->create();

        // Composite — fullscreen tri, samples warp, writes visible target.
        m_compositeRpDesc = renderTarget()->renderPassDescriptor();

        QShader compVS = loadShader(QStringLiteral(":/shaders/milkdrop_composite.vert.qsb"));
        QShader compFS = loadShader(QStringLiteral(":/shaders/milkdrop_composite.frag.qsb"));

        QRhiVertexInputLayout fsLayout;
        fsLayout.setBindings({ QRhiVertexInputBinding(2 * sizeof(float)) });
        fsLayout.setAttributes({
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)
        });

        m_compositePipe = m_rhi->newGraphicsPipeline();
        m_compositePipe->setTopology(QRhiGraphicsPipeline::Triangles);
        m_compositePipe->setShaderStages({
            { QRhiShaderStage::Vertex,   compVS },
            { QRhiShaderStage::Fragment, compFS }
        });
        m_compositePipe->setVertexInputLayout(fsLayout);
        m_compositePipe->setShaderResourceBindings(m_compositeSrb[0]);
        m_compositePipe->setRenderPassDescriptor(m_compositeRpDesc);
        QRhiGraphicsPipeline::TargetBlend tbc;
        tbc.enable = false;
        m_compositePipe->setTargetBlends({ tbc });
        m_compositePipe->create();
    }

    // ---------- handle deferred preset load ----------------------------
    if (m_presetReloadRequested) {
        m_presetReloadRequested = false;
        if (m_runtime) {
            if (!m_pendingPresetPath.isEmpty()) {
                const QString resolved = resolvePresetPath(m_pendingPresetPath);
                m_runtimeReady = m_runtime->loadPresetFromFile(resolved);
                m_lastReportedError = m_runtime->lastError();
            } else {
                m_runtimeReady = false;
                m_lastReportedError.clear();
            }
        }
    }

    // ---------- advance the runtime ------------------------------------
    {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        float dt = 1.0f / 60.0f;
        if (m_havePrevTick) {
            const auto deltaUs = std::chrono::duration_cast<
                std::chrono::microseconds>(now - m_lastTick).count();
            dt = std::clamp(float(deltaUs) * 1e-6f, 1.0f / 240.0f, 0.1f);
        }
        m_lastTick = now;
        m_havePrevTick = true;

        if (m_runtimeReady && m_runtime) {
            m_runtime->setAudio(m_audio.bass, m_audio.mid, m_audio.treb,
                                m_audio.bass_att, m_audio.mid_att, m_audio.treb_att);
            m_runtime->advance(dt);
            m_runtime->runPerFrame();
            m_runtime->runPerVertex(kMeshW, kMeshH, m_uvScratch.data());
        } else {
            // No preset loaded — identity UVs so the warp pass is a no-op
            // (sample own pixel back). Combined with decay = 1, this
            // shows whatever the previous frame was, fading slowly.
            for (int j = 0; j < kMeshH; ++j) {
                for (int i = 0; i < kMeshW; ++i) {
                    const float u = (kMeshW > 1) ? float(i) / float(kMeshW - 1) : 0.5f;
                    const float v = (kMeshH > 1) ? float(j) / float(kMeshH - 1) : 0.5f;
                    const int idx = 2 * (j * kMeshW + i);
                    m_uvScratch[idx + 0] = u;
                    m_uvScratch[idx + 1] = v;
                }
            }
        }
    }

    // ---------- per-frame uploads --------------------------------------
    auto* upload = m_rhi->nextResourceUpdateBatch();

    uploadStaticBuffers(upload);

    // UVs
    upload->updateDynamicBuffer(m_uvVB, 0, kUvBufBytes, m_uvScratch.data());

    // Warp UBO
    {
        WarpUbo u{};
        writeIdentity(u.qt_Matrix);
        u.qt_Opacity   = 1.0f;
        u.decay        = m_runtimeReady ? m_runtime->decay()       : 0.96f;
        u.darkenCenter = m_runtimeReady ? m_runtime->darken_center(): 0.0f;
        const float wr = m_runtimeReady ? m_runtime->wave_r() : 1.0f;
        const float wg = m_runtimeReady ? m_runtime->wave_g() : 1.0f;
        const float wb = m_runtimeReady ? m_runtime->wave_b() : 1.0f;
        const float wa = m_runtimeReady ? m_runtime->wave_a() : 0.5f;
        u.waveColor[0] = wr; u.waveColor[1] = wg;
        u.waveColor[2] = wb; u.waveColor[3] = wa;
        upload->updateDynamicBuffer(m_warpUbo, 0, sizeof(u), &u);
    }

    // Composite UBO
    {
        CompositeUbo u{};
        writeIdentity(u.qt_Matrix);
        u.qt_Opacity = 1.0f;
        u.gamma      = m_runtimeReady ? m_runtime->gamma() : 1.0f;
        upload->updateDynamicBuffer(m_compositeUbo, 0, sizeof(u), &u);
    }

    // First frame: prime warp[read] with a non-zero tint so the user
    // doesn't stare at a black screen waiting for the runtime to inject
    // colour. A neutral mid-grey gives every preset something to fade
    // and warp from. Subsequent frames keep the priming bit set so we
    // only do this once.
    if (!m_warpPrimed) {
        const int W = size.width();
        const int H = size.height();
        std::vector<uint8_t> seed(W * H * 4);
        for (int p = 0; p < W * H; ++p) {
            seed[4 * p + 0] = 0x20;
            seed[4 * p + 1] = 0x20;
            seed[4 * p + 2] = 0x20;
            seed[4 * p + 3] = 0xff;
        }
        QRhiTextureSubresourceUploadDescription desc(seed.data(), int(seed.size()));
        QRhiTextureUploadDescription up;
        QRhiTextureUploadEntry e(0, 0, desc);
        up.setEntries({e});
        upload->uploadTexture(m_warp[m_readIdx], up);
        m_warpPrimed = true;
    }

    // ---------- pass A: warp[read] → warp[write] -----------------------
    cb->beginPass(m_warpRt[m_writeIdx],
                  QColor(0, 0, 0, 0),
                  { 1.0f, 0 },
                  upload);
    {
        const QRhiViewport vp(0, 0, float(size.width()), float(size.height()));
        cb->setViewport(vp);
    }
    cb->setGraphicsPipeline(m_warpPipe);
    cb->setShaderResources(m_warpSrb[m_writeIdx]);
    {
        QRhiCommandBuffer::VertexInput vins[2] = {
            QRhiCommandBuffer::VertexInput(m_posVB, 0),
            QRhiCommandBuffer::VertexInput(m_uvVB,  0)
        };
        cb->setVertexInput(0, 2, vins,
                           m_meshIB, 0, QRhiCommandBuffer::IndexUInt16);
        cb->drawIndexed(kIndexCount);
    }
    cb->endPass();

    // ---------- pass B: warp[write] → renderTarget() -------------------
    cb->beginPass(renderTarget(),
                  QColor(0, 0, 0, 1),
                  { 1.0f, 0 });
    {
        const QSize cs = renderTarget()->pixelSize();
        const QRhiViewport vp(0, 0, float(cs.width()), float(cs.height()));
        cb->setViewport(vp);
    }
    cb->setGraphicsPipeline(m_compositePipe);
    cb->setShaderResources(m_compositeSrb[m_writeIdx]);
    {
        QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
    }
    cb->endPass();

    // Swap ping-pong.
    std::swap(m_readIdx, m_writeIdx);

    // Request another frame so the visualizer keeps animating even
    // when the audio features don't fire (silence, pause, etc.).
    update();
}


// ---------------------------------------------------------------------------
//  MilkdropView — item-side (GUI thread)
// ---------------------------------------------------------------------------

MilkdropView::MilkdropView(QQuickItem* parent)
    : QQuickRhiItem(parent)
{
    setMirrorVertically(true);     // Match the runtime's top-left UV origin.
    setAlphaBlending(false);
    setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA8);
}

MilkdropView::~MilkdropView() = default;

QQuickRhiItemRenderer* MilkdropView::createRenderer()
{
    return new MilkdropViewImpl(this);
}

void MilkdropView::setAudioSource(AudioFeatures* s)
{
    if (m_audioSource == s) return;
    if (m_audioSource) {
        disconnect(m_audioSource, &AudioFeatures::featuresUpdated,
                   this, &MilkdropView::onFeaturesUpdated);
    }
    m_audioSource = s;
    if (m_audioSource) {
        connect(m_audioSource, &AudioFeatures::featuresUpdated,
                this, &MilkdropView::onFeaturesUpdated,
                Qt::AutoConnection);
    }
    emit audioSourceChanged();
}

void MilkdropView::setPresetPath(const QString& p)
{
    if (m_presetPath == p) return;
    m_presetPath = p;
    m_presetDirty.store(true, std::memory_order_release);
    emit presetPathChanged();
    update();
}

void MilkdropView::onFeaturesUpdated()
{
    if (!m_audioSource) return;
    {
        QMutexLocker lk(&m_stagedMutex);
        // Map AudioFeatures (perceptual / log-magnitude bands) into the
        // MilkDrop scalar convention. Geiss's reference says the
        // baseline should hover around 1.0 for "average" music; bass
        // peaks ~1.5 on a kick, etc. AudioFeatures' bass_att is in
        // [0, ~2] already after envelope follow, so the scale lands
        // close to expectations without extra mapping.
        m_staged.bass     = m_audioSource->bass();
        m_staged.mid      = m_audioSource->mid();
        m_staged.treb     = m_audioSource->treb();
        m_staged.bass_att = m_audioSource->bass_att();
        m_staged.mid_att  = m_audioSource->mid_att();
        m_staged.treb_att = m_audioSource->treb_att();
    }
    m_audioDirty.store(true, std::memory_order_release);
    update();
}
