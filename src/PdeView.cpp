// PdeView -- see PdeView.h for the design intent.
//
// RHI plumbing follows the MilkdropView + HilbertRosette template:
//   * GUI thread owns the QObject and stages audio + property changes
//     into atomics + a mutex-guarded struct.
//   * QQuickRhiItemRenderer runs on the render thread; synchronize()
//     snapshots state under the GUI-blocking barrier, render() executes
//     the solver + display passes.
//
// Resource layout for Gray-Scott:
//   * Two RGBA16F ping-pong textures sized to the item -- pattern grid
//     resolution matches the on-screen pixel size, so "1 pixel = 1
//     grid cell". Format is RGBA16F because Qt's RHI exposes
//     RGBA16F / RGBA32F but not RG16F; 16-bit float is plenty for
//     u, v in [0, 1] and halves the memory bandwidth vs 32F.
//   * One sampler with REPEAT addressing (the textureOffset() neighbour
//     fetches wrap correctly at the borders -- the Gray-Scott PDE on a
//     torus is mathematically clean; clamp would create discontinuities
//     that show up as static lines along the borders).
//   * Three pipelines: solver (samples prev state, writes new state),
//     GS display (samples state, writes visible target), and Chladni
//     display (no texture sample, writes visible target).
//
// Reset codepath: when the renderer sees m_resetGsPending, it
// repaints both ping-pong textures with the (u=1, v=0) flat background
// plus a small disc of (u=0.5, v=0.5) at the centre. The very first
// frame after construction also triggers a reset so the GS pattern
// always starts in a known state.

#include "PdeView.h"
#include "AudioFeatures.h"

#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int kFsVertexBytes = 12 * int(sizeof(float));

// std140 UBO layouts. Order *must* match the binding=1 block in the
// matching fragment shaders.

struct PdeSolverUbo {
    float invResolution[2];    // off 0   sz 8
    float feed;                // off 8   sz 4
    float kill;                // off 12  sz 4
    float Du;                  // off 16  sz 4
    float Dv;                  // off 20  sz 4
    float dt;                  // off 24  sz 4
    float _pad0;               // off 28  sz 4   -> align next vec4
    float fluxImpulse[4];      // off 32  sz 16
};                             // total 48 (multiple of 16)
static_assert(sizeof(PdeSolverUbo) == 48, "PdeSolverUbo std140 drift");

struct PdeDisplayUbo {
    float colorA[4];           // off 0   sz 16
    float colorB[4];           // off 16  sz 16
    float lowThr;              // off 32  sz 4
    float highThr;             // off 36  sz 4
    float edgeStrength;        // off 40  sz 4
    float gain;                // off 44  sz 4
    float invResolution[2];    // off 48  sz 8   -> next member starts at 56
    float _pad0[2];            // off 56  sz 8   -> total 64 (multiple of 16)
};                             // total 64 (multiple of 16)
static_assert(sizeof(PdeDisplayUbo) == 64, "PdeDisplayUbo std140 drift");

struct PdeChladniUbo {
    float m, n;                // off 0   sz 8
    float bass, mid;           // off 8   sz 8
    float treb, rms;           // off 16  sz 8
    float time, lineWidth;     // off 24  sz 8
    float lineColor[4];        // off 32  sz 16
    float bgColor[4];          // off 48  sz 16
};                             // total 64 (multiple of 16)
static_assert(sizeof(PdeChladniUbo) == 64, "PdeChladniUbo std140 drift");

// Tiny qt-style UBO (mat4 + opacity). 80 bytes (64 + 4 + 12 pad). Shared
// shader layout used by every fragment shader at binding 0.
struct QtUbo {
    float mat[16];
    float opacity;
    float pad[3];
};
static_assert(sizeof(QtUbo) == 80, "QtUbo std140 drift");

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

// half-float encoding (16-bit IEEE 754).
// Reference: IEEE 754-2008 binary16 (uncopyrightable). Used only for
// the GS seed upload -- the runtime solver writes float values
// directly from the fragment shader.
inline uint16_t f32_to_f16(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    uint16_t sign = uint16_t((x >> 16) & 0x8000u);
    int32_t  expo = int32_t((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (expo <= 0) {
        if (expo < -10) return sign;                          // zero / underflow
        mant = (mant | 0x800000u) >> (1 - expo);
        if (mant & 0x1000u) mant += 0x2000u;                  // round half-up
        return uint16_t(sign | (mant >> 13));
    }
    if (expo >= 31) {                                         // overflow -> inf
        return uint16_t(sign | 0x7C00u | (mant ? 1u : 0u));
    }
    if (mant & 0x1000u) {                                     // round half-up
        mant += 0x2000u;
        if (mant & 0x800000u) { mant = 0; expo++; }
        if (expo >= 31) return uint16_t(sign | 0x7C00u);
    }
    return uint16_t(sign | (uint16_t(expo) << 10) | (mant >> 13));
}

// Build the (u, v) seed buffer in RGBA16F layout for a WxH grid. u=1
// everywhere; v=0 everywhere except inside a centred disc of radius
// `seedR` (in pixels) where (u, v) = (0.5, 0.5). RGBA16F is 8 bytes
// per pixel.
void buildGsSeed(int W, int H, int seedR, std::vector<uint16_t>& out)
{
    out.assign(size_t(W) * size_t(H) * 4, 0);
    const uint16_t kOne  = f32_to_f16(1.0f);
    const uint16_t kHalf = f32_to_f16(0.5f);
    const uint16_t kZero = f32_to_f16(0.0f);
    const int cx = W / 2;
    const int cy = H / 2;
    const int r2 = seedR * seedR;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const bool inside = (dx * dx + dy * dy) <= r2;
            uint16_t* p = &out[size_t(y) * size_t(W) * 4 + size_t(x) * 4];
            p[0] = inside ? kHalf : kOne;
            p[1] = inside ? kHalf : kZero;
            p[2] = kZero;
            p[3] = kOne;
        }
    }
}

}  // namespace


// ---------------------------------------------------------------------------
//  PdeViewImpl -- render-thread renderer
// ---------------------------------------------------------------------------

class PdeViewImpl : public QQuickRhiItemRenderer {
public:
    explicit PdeViewImpl(PdeView* item) : m_item(item) {}

    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void releaseStateResources();
    void ensureStateResources(const QSize& size);
    void buildPipelinesIfNeeded();
    void uploadStaticGeometry(QRhiResourceUpdateBatch* batch);
    void seedGrayScott(QRhiResourceUpdateBatch* batch);

    PdeView* m_item = nullptr;
    QRhi*    m_rhi  = nullptr;
    QSize    m_pixelSize;

    // -- GUI-thread snapshot (filled in synchronize()) ----------------------
    PdeView::Mode             m_mode = PdeView::Mode::GrayScott;
    PdeView::StagedAudio      m_audio;
    bool                      m_resetGsPending = false;

    float   m_chladniM = 5.0f, m_chladniN = 7.0f;
    float   m_chladniLineW = 1.0f;
    float   m_chladniLine[4] = {1,1,1,1};
    float   m_chladniBg[4]   = {0.027f,0.039f,0.071f,1.0f};

    float   m_gsFeedBase = 0.035f;
    float   m_gsKillBase = 0.062f;
    float   m_gsDu = 0.16f, m_gsDv = 0.08f;
    float   m_gsDt = 1.0f;
    int     m_gsSteps = 8;
    float   m_gsColorA[4] = {0.078f,0.027f,0.165f,1.0f};
    float   m_gsColorB[4] = {1.0f,0.902f,0.761f,1.0f};

    int     m_pendingImpulses = 0;

    // -- Persistent GPU resources -------------------------------------------

    // Solver state ping-pong (RGBA16F).
    QRhiTexture*              m_state[2]     = { nullptr, nullptr };
    QRhiTextureRenderTarget*  m_stateRt[2]   = { nullptr, nullptr };
    QRhiRenderPassDescriptor* m_stateRpDesc  = nullptr;
    int                       m_readIdx  = 0;
    int                       m_writeIdx = 1;
    bool                      m_stateSeeded = false;

    QRhiSampler*              m_repeatSampler = nullptr;

    QRhiBuffer*               m_fsVB = nullptr;

    // qt-style UBOs (matrix + opacity), one per pipeline so they don't
    // collide when we update them at different points in the render
    // graph. Each is 80 bytes; total 240 bytes -- trivial.
    QRhiBuffer*               m_qtUboSolver   = nullptr;
    QRhiBuffer*               m_qtUboGsDisp   = nullptr;
    QRhiBuffer*               m_qtUboChladni  = nullptr;

    // Solver pipeline.
    QRhiBuffer*               m_solverUbo = nullptr;
    QRhiShaderResourceBindings* m_solverSrb[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*     m_solverPipe = nullptr;

    // Gray-Scott display pipeline (writes visible target).
    QRhiBuffer*               m_gsDisplayUbo = nullptr;
    QRhiShaderResourceBindings* m_gsDisplaySrb[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*     m_gsDisplayPipe = nullptr;
    QRhiRenderPassDescriptor* m_visibleRpDesc = nullptr;

    // Chladni pipeline (writes visible target).
    QRhiBuffer*               m_chladniUbo = nullptr;
    QRhiShaderResourceBindings* m_chladniSrb = nullptr;
    QRhiGraphicsPipeline*     m_chladniPipe = nullptr;

    bool m_staticUploaded = false;

    std::chrono::steady_clock::time_point m_startTime{};
    bool m_haveStart = false;

    // xorshift32 for impulse positioning. Seeded with a fixed value so
    // runs are reproducible; the seed is the renderer's own constant.
    uint32_t m_impulseSeed = 0xC0FFEE13u;
    uint32_t nextRand() {
        uint32_t x = m_impulseSeed;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        m_impulseSeed = x;
        return x;
    }
    float m_pendingImpulseX = 0.5f;
    float m_pendingImpulseY = 0.5f;
};


// ---------------------------------------------------------------------------
//  initialize -- create resources that don't depend on item size.
// ---------------------------------------------------------------------------

void PdeViewImpl::initialize(QRhiCommandBuffer* /*cb*/)
{
    m_rhi = rhi();
    if (!m_rhi) return;

    if (!m_repeatSampler) {
        m_repeatSampler = m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::Repeat, QRhiSampler::Repeat);
        m_repeatSampler->create();
    }
    if (!m_fsVB) {
        m_fsVB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                  QRhiBuffer::VertexBuffer,
                                  kFsVertexBytes);
        m_fsVB->create();
    }
    if (!m_solverUbo) {
        m_solverUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                       QRhiBuffer::UniformBuffer,
                                       sizeof(PdeSolverUbo));
        m_solverUbo->create();
    }
    if (!m_gsDisplayUbo) {
        m_gsDisplayUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                          QRhiBuffer::UniformBuffer,
                                          sizeof(PdeDisplayUbo));
        m_gsDisplayUbo->create();
    }
    if (!m_chladniUbo) {
        m_chladniUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                        QRhiBuffer::UniformBuffer,
                                        sizeof(PdeChladniUbo));
        m_chladniUbo->create();
    }
    if (!m_qtUboSolver) {
        m_qtUboSolver = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                         QRhiBuffer::UniformBuffer,
                                         sizeof(QtUbo));
        m_qtUboSolver->create();
    }
    if (!m_qtUboGsDisp) {
        m_qtUboGsDisp = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                         QRhiBuffer::UniformBuffer,
                                         sizeof(QtUbo));
        m_qtUboGsDisp->create();
    }
    if (!m_qtUboChladni) {
        m_qtUboChladni = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                          QRhiBuffer::UniformBuffer,
                                          sizeof(QtUbo));
        m_qtUboChladni->create();
    }
}

void PdeViewImpl::releaseStateResources()
{
    for (int i = 0; i < 2; ++i) {
        if (m_stateRt[i])      { m_stateRt[i]->destroy();      delete m_stateRt[i];      m_stateRt[i] = nullptr; }
        if (m_state[i])        { m_state[i]->destroy();        delete m_state[i];        m_state[i]   = nullptr; }
        if (m_solverSrb[i])    { m_solverSrb[i]->destroy();    delete m_solverSrb[i];    m_solverSrb[i] = nullptr; }
        if (m_gsDisplaySrb[i]) { m_gsDisplaySrb[i]->destroy(); delete m_gsDisplaySrb[i]; m_gsDisplaySrb[i] = nullptr; }
    }
    if (m_stateRpDesc) { m_stateRpDesc->destroy(); delete m_stateRpDesc; m_stateRpDesc = nullptr; }
    m_stateSeeded = false;
}

void PdeViewImpl::ensureStateResources(const QSize& size)
{
    if (!m_rhi || size.width() <= 0 || size.height() <= 0) return;
    if (m_state[0] && m_pixelSize == size) return;

    releaseStateResources();
    m_pixelSize = size;

    for (int i = 0; i < 2; ++i) {
        m_state[i] = m_rhi->newTexture(QRhiTexture::RGBA16F, size, 1,
                                       QRhiTexture::RenderTarget);
        m_state[i]->create();
        QRhiTextureRenderTargetDescription rtDesc{
            QRhiColorAttachment(m_state[i])
        };
        m_stateRt[i] = m_rhi->newTextureRenderTarget(rtDesc);
        if (!m_stateRpDesc) {
            m_stateRpDesc = m_stateRt[i]->newCompatibleRenderPassDescriptor();
        }
        m_stateRt[i]->setRenderPassDescriptor(m_stateRpDesc);
        m_stateRt[i]->create();
    }

    // SRBs for solver + GS display. Each SRB index `i` samples from
    // m_state[1 - i] -- so when the writer is index `i`, the sampler
    // reads the OTHER texture. After each substep we swap the indices.
    for (int i = 0; i < 2; ++i) {
        const int readIdx = 1 - i;

        m_solverSrb[i] = m_rhi->newShaderResourceBindings();
        m_solverSrb[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_qtUboSolver),
            QRhiShaderResourceBinding::uniformBuffer(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_solverUbo),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage,
                m_state[readIdx], m_repeatSampler)
        });
        m_solverSrb[i]->create();

        m_gsDisplaySrb[i] = m_rhi->newShaderResourceBindings();
        m_gsDisplaySrb[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_qtUboGsDisp),
            QRhiShaderResourceBinding::uniformBuffer(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_gsDisplayUbo),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage,
                m_state[readIdx], m_repeatSampler)
        });
        m_gsDisplaySrb[i]->create();
    }

    m_readIdx  = 0;
    m_writeIdx = 1;
    m_stateSeeded = false;
}

void PdeViewImpl::buildPipelinesIfNeeded()
{
    if (m_solverPipe) return;
    if (!renderTarget()) return;
    if (!m_state[0] || !m_stateRpDesc) return;

    m_visibleRpDesc = renderTarget()->renderPassDescriptor();

    QShader qVS         = loadShader(QStringLiteral(":/shaders/pde_quad.vert.qsb"));
    QShader solverFS    = loadShader(QStringLiteral(":/shaders/pde_grayscott.frag.qsb"));
    QShader gsDispFS    = loadShader(QStringLiteral(":/shaders/pde_grayscott_display.frag.qsb"));
    QShader chladniFS   = loadShader(QStringLiteral(":/shaders/pde_chladni.frag.qsb"));

    QRhiVertexInputLayout fsLayout;
    fsLayout.setBindings({ QRhiVertexInputBinding(2 * sizeof(float)) });
    fsLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)
    });

    m_chladniSrb = m_rhi->newShaderResourceBindings();
    m_chladniSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage |
            QRhiShaderResourceBinding::FragmentStage,
            m_qtUboChladni),
        QRhiShaderResourceBinding::uniformBuffer(
            1, QRhiShaderResourceBinding::FragmentStage,
            m_chladniUbo)
    });
    m_chladniSrb->create();

    auto buildPipe = [&](QRhiGraphicsPipeline*& outPipe,
                         const QShader& vs, const QShader& fs,
                         QRhiShaderResourceBindings* srb,
                         QRhiRenderPassDescriptor* rpDesc)
    {
        outPipe = m_rhi->newGraphicsPipeline();
        outPipe->setTopology(QRhiGraphicsPipeline::Triangles);
        outPipe->setShaderStages({
            { QRhiShaderStage::Vertex,   vs },
            { QRhiShaderStage::Fragment, fs }
        });
        outPipe->setVertexInputLayout(fsLayout);
        outPipe->setShaderResourceBindings(srb);
        outPipe->setRenderPassDescriptor(rpDesc);
        QRhiGraphicsPipeline::TargetBlend tb;
        tb.enable = false;
        outPipe->setTargetBlends({ tb });
        outPipe->create();
    };

    buildPipe(m_solverPipe,    qVS, solverFS,  m_solverSrb[0],    m_stateRpDesc);
    buildPipe(m_gsDisplayPipe, qVS, gsDispFS,  m_gsDisplaySrb[0], m_visibleRpDesc);
    buildPipe(m_chladniPipe,   qVS, chladniFS, m_chladniSrb,      m_visibleRpDesc);
}

void PdeViewImpl::uploadStaticGeometry(QRhiResourceUpdateBatch* batch)
{
    if (m_staticUploaded) return;
    static const float fs[12] = {
        -1.f, -1.f,   1.f, -1.f,   -1.f,  1.f,
        -1.f,  1.f,   1.f, -1.f,    1.f,  1.f
    };
    batch->uploadStaticBuffer(m_fsVB, 0, sizeof(fs), fs);
    m_staticUploaded = true;
}

void PdeViewImpl::seedGrayScott(QRhiResourceUpdateBatch* batch)
{
    if (!m_rhi || !m_state[0] || !m_state[1]) return;
    const int W = m_pixelSize.width();
    const int H = m_pixelSize.height();
    if (W <= 0 || H <= 0) return;

    const int seedR = std::clamp(int(0.04f * float(std::min(W, H))),
                                 6, 40);
    std::vector<uint16_t> seed;
    buildGsSeed(W, H, seedR, seed);

    QRhiTextureSubresourceUploadDescription desc(
        seed.data(),
        int(seed.size() * sizeof(uint16_t)));
    QRhiTextureUploadEntry e(0, 0, desc);
    QRhiTextureUploadDescription up;
    up.setEntries({ e });
    batch->uploadTexture(m_state[0], up);
    batch->uploadTexture(m_state[1], up);
    m_readIdx  = 0;
    m_writeIdx = 1;
    m_stateSeeded = true;
}


// ---------------------------------------------------------------------------
//  synchronize
// ---------------------------------------------------------------------------

void PdeViewImpl::synchronize(QQuickRhiItem* item)
{
    auto* pv = static_cast<PdeView*>(item);

    const bool active = pv->m_active.load(std::memory_order_relaxed);

    if (active && pv->m_audioDirty.exchange(false, std::memory_order_acquire)) {
        QMutexLocker lk(&pv->m_stagedMutex);
        m_audio = pv->m_staged;
    }

    m_mode = pv->m_mode;
    pv->m_modeChanged.exchange(false, std::memory_order_acquire);   // drain

    if (pv->m_gsResetRequested.exchange(false, std::memory_order_acquire)) {
        m_resetGsPending = true;
    }

    m_chladniM     = pv->m_chladniM;
    m_chladniN     = pv->m_chladniN;
    m_chladniLineW = pv->m_chladniLineW;
    m_chladniLine[0] = float(pv->m_chladniLine.redF());
    m_chladniLine[1] = float(pv->m_chladniLine.greenF());
    m_chladniLine[2] = float(pv->m_chladniLine.blueF());
    m_chladniLine[3] = 1.0f;
    m_chladniBg[0] = float(pv->m_chladniBg.redF());
    m_chladniBg[1] = float(pv->m_chladniBg.greenF());
    m_chladniBg[2] = float(pv->m_chladniBg.blueF());
    m_chladniBg[3] = 1.0f;

    m_gsFeedBase = pv->m_gsFeedBase;
    m_gsKillBase = pv->m_gsKillBase;
    m_gsDu       = pv->m_gsDu;
    m_gsDv       = pv->m_gsDv;
    m_gsDt       = pv->m_gsDt;
    m_gsSteps    = pv->m_gsSteps;
    m_gsColorA[0] = float(pv->m_gsColorA.redF());
    m_gsColorA[1] = float(pv->m_gsColorA.greenF());
    m_gsColorA[2] = float(pv->m_gsColorA.blueF());
    m_gsColorA[3] = 1.0f;
    m_gsColorB[0] = float(pv->m_gsColorB.redF());
    m_gsColorB[1] = float(pv->m_gsColorB.greenF());
    m_gsColorB[2] = float(pv->m_gsColorB.blueF());
    m_gsColorB[3] = 1.0f;

    if (active) {
        m_pendingImpulses = pv->m_pendingFluxImpulses.exchange(0,
                                std::memory_order_acquire);
    } else {
        // Drain anything that snuck in while we were inactive so it
        // doesn't fire as a backlog the moment we reactivate.
        pv->m_pendingFluxImpulses.store(0, std::memory_order_release);
        m_pendingImpulses = 0;
    }
}


// ---------------------------------------------------------------------------
//  render
// ---------------------------------------------------------------------------

void PdeViewImpl::render(QRhiCommandBuffer* cb)
{
    if (!m_rhi) return;
    QRhiTexture* color = colorTexture();
    if (!color) return;
    const QSize size = color->pixelSize();

    ensureStateResources(size);
    if (!m_state[0]) return;
    buildPipelinesIfNeeded();
    if (!m_solverPipe || !m_gsDisplayPipe || !m_chladniPipe) return;

    if (!m_haveStart) {
        m_startTime = std::chrono::steady_clock::now();
        m_haveStart = true;
    }
    const auto now = std::chrono::steady_clock::now();
    const float t_sec = std::chrono::duration<float>(now - m_startTime).count();

    auto* upload = m_rhi->nextResourceUpdateBatch();
    uploadStaticGeometry(upload);

    // qt UBOs (identity + opacity 1). Each pipeline needs its own at
    // binding 0 (the per-pipeline shader expects qt_Matrix / qt_Opacity).
    QtUbo qu{};
    writeIdentity(qu.mat);
    qu.opacity = 1.0f;
    upload->updateDynamicBuffer(m_qtUboSolver,  0, sizeof(qu), &qu);
    upload->updateDynamicBuffer(m_qtUboGsDisp,  0, sizeof(qu), &qu);
    upload->updateDynamicBuffer(m_qtUboChladni, 0, sizeof(qu), &qu);

    // If a reset is pending OR this is the first frame in GS mode, paint
    // both ping-pong textures with the seed state. Lives in the same
    // upload batch as the first solver substep so the seed lands before
    // the solver samples it.
    const bool needsSeed = (m_mode == PdeView::Mode::GrayScott)
                       && (m_resetGsPending || !m_stateSeeded);
    if (needsSeed) {
        seedGrayScott(upload);
        m_resetGsPending = false;
    }

    if (m_mode == PdeView::Mode::GrayScott) {
        // Audio-modulated (F, k). Range chosen so the visible behaviour
        // walks across two GS basins of attraction without crossing into
        // divergent territory:
        //   F in [F_base, F_base + 0.020] (bass adds growth)
        //   k in [k_base, k_base + 0.005] (treb adds decay)
        const float bass = std::clamp(m_audio.bass_att, 0.0f, 1.0f);
        const float treb = std::clamp(m_audio.treb_att, 0.0f, 1.0f);
        const float feedLive = std::clamp(m_gsFeedBase + 0.020f * bass,
                                          0.001f, 0.10f);
        const float killLive = std::clamp(m_gsKillBase + 0.005f * treb,
                                          0.001f, 0.10f);
        m_item->m_gsLastFeed.store(feedLive, std::memory_order_relaxed);
        m_item->m_gsLastKill.store(killLive, std::memory_order_relaxed);
        QMetaObject::invokeMethod(m_item, "gsParamsAdvanced",
                                  Qt::QueuedConnection);

        const int substeps = std::clamp(m_gsSteps, 1, 32);

        // Fire pending audio impulse on the first substep only -- gives
        // diffusion a few sub-steps to propagate the bump within the
        // visible frame.
        bool fireImpulse = (m_pendingImpulses > 0);
        if (fireImpulse) {
            const uint32_t a = nextRand();
            const uint32_t b = nextRand();
            m_pendingImpulseX = 0.05f + 0.90f * (float(a & 0xFFFF) / 65535.0f);
            m_pendingImpulseY = 0.05f + 0.90f * (float(b & 0xFFFF) / 65535.0f);
            m_pendingImpulses = 0;
        }

        for (int s = 0; s < substeps; ++s) {
            QRhiResourceUpdateBatch* sub_upload =
                (s == 0) ? upload : m_rhi->nextResourceUpdateBatch();

            PdeSolverUbo sUbo{};
            sUbo.invResolution[0] = 1.0f / float(size.width());
            sUbo.invResolution[1] = 1.0f / float(size.height());
            sUbo.feed = feedLive;
            sUbo.kill = killLive;
            sUbo.Du   = m_gsDu;
            sUbo.Dv   = m_gsDv;
            sUbo.dt   = m_gsDt;
            const bool fireThisSubstep = (s == 0) && fireImpulse;
            sUbo.fluxImpulse[0] = m_pendingImpulseX;
            sUbo.fluxImpulse[1] = m_pendingImpulseY;
            sUbo.fluxImpulse[2] = fireThisSubstep ? 1.0f : 0.0f;
            sUbo.fluxImpulse[3] = 0.0f;
            sub_upload->updateDynamicBuffer(m_solverUbo, 0,
                                            sizeof(sUbo), &sUbo);

            cb->beginPass(m_stateRt[m_writeIdx],
                          QColor(0, 0, 0, 0), { 1.0f, 0 },
                          sub_upload);
            cb->setViewport({0, 0, float(size.width()), float(size.height())});
            cb->setGraphicsPipeline(m_solverPipe);
            cb->setShaderResources(m_solverSrb[m_writeIdx]);
            QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
            cb->setVertexInput(0, 1, &vin);
            cb->draw(6);
            cb->endPass();

            std::swap(m_readIdx, m_writeIdx);
        }

        // Display pass. After `substeps` swaps, the newest state lives
        // at m_readIdx; m_gsDisplaySrb[k] samples m_state[1-k], so pass
        // k = 1 - m_readIdx.
        PdeDisplayUbo dUbo{};
        std::memcpy(dUbo.colorA, m_gsColorA, sizeof(dUbo.colorA));
        std::memcpy(dUbo.colorB, m_gsColorB, sizeof(dUbo.colorB));
        dUbo.lowThr       = 0.10f;
        dUbo.highThr      = 0.55f;
        dUbo.edgeStrength = 0.5f;
        dUbo.gain         = 1.0f;
        dUbo.invResolution[0] = 1.0f / float(size.width());
        dUbo.invResolution[1] = 1.0f / float(size.height());
        auto* dispUpload = m_rhi->nextResourceUpdateBatch();
        dispUpload->updateDynamicBuffer(m_gsDisplayUbo, 0,
                                        sizeof(dUbo), &dUbo);

        cb->beginPass(renderTarget(),
                      QColor(0, 0, 0, 1), { 1.0f, 0 },
                      dispUpload);
        {
            const QSize cs = renderTarget()->pixelSize();
            cb->setViewport({0, 0, float(cs.width()), float(cs.height())});
        }
        cb->setGraphicsPipeline(m_gsDisplayPipe);
        cb->setShaderResources(m_gsDisplaySrb[1 - m_readIdx]);
        QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
        cb->endPass();
    } else {
        // ---- Chladni: stateless. -------------------------------------
        // Map centroid (0..1) to (m, n) in roughly [2, 12] -- high
        // centroid -> denser pattern. Then blend that target with the
        // user-set (m, n) so the user can shift the basin.
        const float cent = std::clamp(m_audio.centroid_norm, 0.0f, 1.0f);
        const float target_m = 2.0f + 10.0f * cent;
        const float target_n = 3.0f + 9.0f  * cent;
        const float biased_m = 0.5f * target_m + 0.5f * m_chladniM;
        const float biased_n = 0.5f * target_n + 0.5f * m_chladniN;

        PdeChladniUbo cUbo{};
        cUbo.m         = biased_m;
        cUbo.n         = biased_n;
        cUbo.bass      = std::clamp(m_audio.bass_att, 0.0f, 2.0f);
        cUbo.mid       = std::clamp(m_audio.mid_att,  0.0f, 2.0f);
        cUbo.treb      = std::clamp(m_audio.treb_att, 0.0f, 2.0f);
        cUbo.rms       = std::clamp(m_audio.rms_att,  0.0f, 2.0f);
        cUbo.time      = t_sec;
        cUbo.lineWidth = m_chladniLineW;
        std::memcpy(cUbo.lineColor, m_chladniLine, sizeof(cUbo.lineColor));
        std::memcpy(cUbo.bgColor,   m_chladniBg,   sizeof(cUbo.bgColor));
        upload->updateDynamicBuffer(m_chladniUbo, 0, sizeof(cUbo), &cUbo);

        cb->beginPass(renderTarget(),
                      QColor(0, 0, 0, 1), { 1.0f, 0 },
                      upload);
        {
            const QSize cs = renderTarget()->pixelSize();
            cb->setViewport({0, 0, float(cs.width()), float(cs.height())});
        }
        cb->setGraphicsPipeline(m_chladniPipe);
        cb->setShaderResources(m_chladniSrb);
        QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
        cb->endPass();
    }

    update();
}


// ---------------------------------------------------------------------------
//  PdeView (GUI thread)
// ---------------------------------------------------------------------------

PdeView::PdeView(QQuickItem* parent)
    : QQuickRhiItem(parent)
{
    setMirrorVertically(false);
    setAlphaBlending(false);
    setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA8);
}

PdeView::~PdeView() = default;

QQuickRhiItemRenderer* PdeView::createRenderer()
{
    return new PdeViewImpl(this);
}

void PdeView::setAudioSource(AudioFeatures* s)
{
    if (m_audioSource == s) return;
    if (m_audioSource) {
        disconnect(m_audioSource, &AudioFeatures::featuresUpdated,
                   this, &PdeView::onFeaturesUpdated);
        disconnect(m_audioSource, &AudioFeatures::onset,
                   this, &PdeView::onAudioOnset);
    }
    m_audioSource = s;
    if (m_audioSource) {
        connect(m_audioSource, &AudioFeatures::featuresUpdated,
                this, &PdeView::onFeaturesUpdated,
                Qt::AutoConnection);
        connect(m_audioSource, &AudioFeatures::onset,
                this, &PdeView::onAudioOnset,
                Qt::AutoConnection);
    }
    emit audioSourceChanged();
}

void PdeView::setMode(Mode m)
{
    if (m_mode == m) return;
    m_mode = m;
    m_modeChanged.store(true, std::memory_order_release);
    emit modeChanged();
    update();
}

void PdeView::setChladniM(float v)
{
    v = std::clamp(v, 1.0f, 32.0f);
    if (m_chladniM == v) return;
    m_chladniM = v;
    emit chladniMChanged();
    update();
}

void PdeView::setChladniN(float v)
{
    v = std::clamp(v, 1.0f, 32.0f);
    if (m_chladniN == v) return;
    m_chladniN = v;
    emit chladniNChanged();
    update();
}

void PdeView::setChladniLineColor(const QColor& c)
{
    if (m_chladniLine == c) return;
    m_chladniLine = c;
    emit chladniLineColorChanged();
    update();
}

void PdeView::setChladniBgColor(const QColor& c)
{
    if (m_chladniBg == c) return;
    m_chladniBg = c;
    emit chladniBgColorChanged();
    update();
}

void PdeView::setChladniLineWidth(float v)
{
    v = std::clamp(v, 0.1f, 4.0f);
    if (m_chladniLineW == v) return;
    m_chladniLineW = v;
    emit chladniLineWidthChanged();
    update();
}

void PdeView::setGsFeedBase(float v)
{
    v = std::clamp(v, 0.001f, 0.10f);
    if (m_gsFeedBase == v) return;
    m_gsFeedBase = v;
    emit gsFeedBaseChanged();
    update();
}

void PdeView::setGsKillBase(float v)
{
    v = std::clamp(v, 0.001f, 0.10f);
    if (m_gsKillBase == v) return;
    m_gsKillBase = v;
    emit gsKillBaseChanged();
    update();
}

void PdeView::setGsDu(float v)
{
    v = std::clamp(v, 0.01f, 1.0f);
    if (m_gsDu == v) return;
    m_gsDu = v;
    emit gsDuChanged();
    update();
}

void PdeView::setGsDv(float v)
{
    v = std::clamp(v, 0.01f, 1.0f);
    if (m_gsDv == v) return;
    m_gsDv = v;
    emit gsDvChanged();
    update();
}

void PdeView::setGsDt(float v)
{
    // Cap at 1.0 for numerical stability: the explicit-Euler scheme
    // with Du=0.16, Dv=0.08, 5-point Laplacian has a CFL bound of
    // dt < 1/(4*Du) = 1.5625 in pure-diffusion limit; the
    // nonlinear u*v^2 term tightens that further. CPU sweeps show
    // dt=1.5 causes the pattern to collapse; dt=2.0 produces NaN.
    // 1.0 is the largest stable value with comfortable margin.
    v = std::clamp(v, 0.05f, 1.0f);
    if (m_gsDt == v) return;
    m_gsDt = v;
    emit gsDtChanged();
    update();
}

void PdeView::setGsStepsPerFrame(int v)
{
    v = std::clamp(v, 1, 32);
    if (m_gsSteps == v) return;
    m_gsSteps = v;
    emit gsStepsPerFrameChanged();
    update();
}

void PdeView::setGsColorA(const QColor& c)
{
    if (m_gsColorA == c) return;
    m_gsColorA = c;
    emit gsColorAChanged();
    update();
}

void PdeView::setGsColorB(const QColor& c)
{
    if (m_gsColorB == c) return;
    m_gsColorB = c;
    emit gsColorBChanged();
    update();
}

void PdeView::resetGrayScott()
{
    m_gsResetRequested.store(true, std::memory_order_release);
    update();
}

void PdeView::setActive(bool a)
{
    if (m_active.load(std::memory_order_relaxed) == a) return;
    m_active.store(a, std::memory_order_relaxed);
    emit activeChanged();
}

void PdeView::onFeaturesUpdated()
{
    if (!m_active.load(std::memory_order_relaxed)) return;
    if (!m_audioSource) return;
    {
        QMutexLocker lk(&m_stagedMutex);
        m_staged.bass          = m_audioSource->bass();
        m_staged.mid           = m_audioSource->mid();
        m_staged.treb          = m_audioSource->treb();
        m_staged.bass_att      = m_audioSource->bass_att();
        m_staged.mid_att       = m_audioSource->mid_att();
        m_staged.treb_att      = m_audioSource->treb_att();
        m_staged.rms_att       = m_audioSource->rms_att();
        m_staged.flux_norm     = m_audioSource->flux_norm();
        m_staged.centroid_norm = m_audioSource->centroid_norm();
    }
    m_audioDirty.store(true, std::memory_order_release);
    update();
}

void PdeView::onAudioOnset()
{
    if (!m_active.load(std::memory_order_relaxed)) return;
    m_pendingFluxImpulses.fetch_add(1, std::memory_order_acq_rel);
    update();
}
