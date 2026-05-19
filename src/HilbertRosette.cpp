#include "HilbertRosette.h"
#include "AudioFeatures.h"

#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>


// ---------------------------------------------------------------------------
//  Constants + helpers
// ---------------------------------------------------------------------------

namespace {

constexpr int kNBands         = HilbertRosette::N_BANDS;         // 8
constexpr int kFloatsPerVtx   = 5;     // (corner.xy, center.xy, bandIdx)
constexpr int kVerticesPerDot = 4;
constexpr int kIndicesPerDot  = 6;
constexpr int kDotsVertexBytes =
    kNBands * kVerticesPerDot * kFloatsPerVtx * int(sizeof(float));
constexpr int kDotsIndexBytes  =
    kNBands * kIndicesPerDot  * int(sizeof(uint16_t));

constexpr float kPi_f = 3.14159265358979323846f;

// Per-frame dot UBO. std140 padding rules: float=4, vec2=8, vec4=16,
// struct end aligned to 16.
struct DotUbo {
    float viewportPx[2];     // off 0   sz 8
    float sigma;             // off 8   sz 4
    float intensity;         // off 12  sz 4
    // 8 band hue colors (RGB premultiplied); vec4 alignment.
    float bandColor[8][4];   // off 16  sz 128
};                           // total: 144 (mult of 16)
static_assert(sizeof(DotUbo) == 144, "DotUbo std140 layout drift");

// Decay / composite UBO. Same layout ScopeRenderer uses, kept compatible
// with shaders/scope_decay if we ever wanted to share -- but ours has
// no qt_Matrix because our vert shader doesn't take one.
struct DecayUbo {
    float decayFactor;       // off 0   sz 4
    float _pad[3];           // off 4   sz 12   -> align next vec4 to 16
    float tint[4];           // off 16  sz 16
};                           // total: 32
static_assert(sizeof(DecayUbo) == 32, "DecayUbo std140 layout drift");

QShader loadShader(const QString& resourcePath)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

// Map HSV(h, s, v) -> linear sRGB. h in [0,1], s/v in [0,1]. Used to
// pick 8 evenly-spaced hues for the dots; we want them visibly distinct
// rather than a viridis-style gradient.
void hsv2rgb(float h, float s, float v, float* r, float* g, float* b)
{
    const float i_f = std::floor(h * 6.0f);
    const float f = h * 6.0f - i_f;
    const int   i = int(i_f) % 6;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (i) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}

}  // namespace


// ---------------------------------------------------------------------------
//  HilbertRosetteImpl -- lives on the render thread
// ---------------------------------------------------------------------------

class HilbertRosetteImpl : public QQuickRhiItemRenderer {
public:
    explicit HilbertRosetteImpl(HilbertRosette* item) : m_item(item) {}

    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void releaseAccumResources();
    void ensureAccumResources(const QSize& size);
    void buildPipelinesIfNeeded();
    void uploadStaticGeometry(QRhiResourceUpdateBatch* batch);

    HilbertRosette* m_item;
    QRhi*           m_rhi = nullptr;
    QSize           m_pixelSize;

    // Ping-pong accumulation textures + RT descriptors.
    QRhiTexture*              m_accum[2]      = { nullptr, nullptr };
    QRhiTextureRenderTarget*  m_accumRt[2]    = { nullptr, nullptr };
    QRhiRenderPassDescriptor* m_accumRpDesc   = nullptr;
    int  m_accumRead  = 0;
    int  m_accumWrite = 1;
    bool m_accumPrimed = false;

    QRhiSampler* m_linearSampler = nullptr;

    // Dot pipeline -- one Gaussian splat per band.
    QRhiBuffer*                 m_dotVB   = nullptr;
    QRhiBuffer*                 m_dotIB   = nullptr;
    QRhiBuffer*                 m_dotUBO  = nullptr;
    QRhiShaderResourceBindings* m_dotSrb  = nullptr;
    QRhiGraphicsPipeline*       m_dotPipe = nullptr;

    // Decay (full-screen) pipeline: sample prev accum, mult by decayFactor.
    QRhiBuffer*                 m_fsVB           = nullptr;
    QRhiBuffer*                 m_decayUBO       = nullptr;
    QRhiShaderResourceBindings* m_decaySrbRead[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*       m_decayPipe      = nullptr;

    // Composite (full-screen) pipeline: sample fresh accum, output as-is
    // (multiplied by tint = white) into the visible color target.
    QRhiBuffer*                 m_compositeUBO   = nullptr;
    QRhiShaderResourceBindings* m_compositeSrbRead[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*       m_compositePipe  = nullptr;
    QRhiRenderPassDescriptor*   m_compositeRpDesc = nullptr;

    bool m_staticUploaded = false;

    // GUI -> render thread state snapshot.
    std::array<float, kNBands> m_env   {};
    std::array<float, kNBands> m_phase {};
    // We DO NOT use m_instFreq directly at render time -- the dot's
    // angular position is base_b + phase_b already, no extra integration.
    float m_trailTau    = 0.15f;
    float m_dotRadius   = 8.0f;
    float m_ringRadius  = 0.40f;

    // Per-band base angles + colors -- constant for the lifetime of the
    // renderer (analyzer band count is compile-time). Computed once on
    // first init.
    std::array<float, kNBands>            m_baseAngle {};
    std::array<std::array<float,4>,kNBands> m_bandColor {};
    bool m_bandsBaked = false;

    // Per-frame scratch.
    std::vector<float> m_dotVbScratch;
};


// ---------------------------------------------------------------------------
//  initialize -- one-shot resource creation
// ---------------------------------------------------------------------------

void HilbertRosetteImpl::initialize(QRhiCommandBuffer* /*cb*/)
{
    m_rhi = rhi();
    if (!m_rhi) return;

    if (!m_linearSampler) {
        m_linearSampler = m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        m_linearSampler->create();
    }
    if (!m_dotVB) {
        m_dotVB = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                   QRhiBuffer::VertexBuffer,
                                   kDotsVertexBytes);
        m_dotVB->create();
    }
    if (!m_dotIB) {
        m_dotIB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                   QRhiBuffer::IndexBuffer,
                                   kDotsIndexBytes);
        m_dotIB->create();
    }
    if (!m_dotUBO) {
        m_dotUBO = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                    QRhiBuffer::UniformBuffer,
                                    sizeof(DotUbo));
        m_dotUBO->create();
    }
    if (!m_fsVB) {
        m_fsVB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                  QRhiBuffer::VertexBuffer,
                                  12 * sizeof(float));
        m_fsVB->create();
    }
    if (!m_decayUBO) {
        m_decayUBO = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                      QRhiBuffer::UniformBuffer,
                                      sizeof(DecayUbo));
        m_decayUBO->create();
    }
    if (!m_compositeUBO) {
        m_compositeUBO = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                          QRhiBuffer::UniformBuffer,
                                          sizeof(DecayUbo));
        m_compositeUBO->create();
    }

    // Bake band-base-angles + hues now. Each band sits at i / N_BANDS
    // around the circle, starting at -pi/2 (12 o'clock) so the lowest
    // band lives at the top.
    if (!m_bandsBaked) {
        for (int b = 0; b < kNBands; ++b) {
            m_baseAngle[b] = -kPi_f * 0.5f
                           + 2.0f * kPi_f * float(b) / float(kNBands);
            // Hue stripes across the band index for high visual distinct-
            // ness. Saturation 0.85, value 1.0 keeps every dot bright.
            float r, g, bl;
            hsv2rgb(float(b) / float(kNBands), 0.85f, 1.0f, &r, &g, &bl);
            m_bandColor[b] = { r, g, bl, 1.0f };
        }
        m_bandsBaked = true;
    }
}


// ---------------------------------------------------------------------------
//  Accumulation textures (resize-aware ping-pong)
// ---------------------------------------------------------------------------

void HilbertRosetteImpl::releaseAccumResources()
{
    for (int i = 0; i < 2; ++i) {
        if (m_accumRt[i])  { m_accumRt[i]->destroy();  delete m_accumRt[i];  m_accumRt[i] = nullptr; }
        if (m_accum[i])    { m_accum[i]->destroy();    delete m_accum[i];    m_accum[i]   = nullptr; }
        if (m_decaySrbRead[i])     { m_decaySrbRead[i]->destroy();     delete m_decaySrbRead[i];     m_decaySrbRead[i]     = nullptr; }
        if (m_compositeSrbRead[i]) { m_compositeSrbRead[i]->destroy(); delete m_compositeSrbRead[i]; m_compositeSrbRead[i] = nullptr; }
    }
    if (m_accumRpDesc) { m_accumRpDesc->destroy(); delete m_accumRpDesc; m_accumRpDesc = nullptr; }
    m_accumPrimed = false;
}

void HilbertRosetteImpl::ensureAccumResources(const QSize& size)
{
    if (!m_rhi || size.width() <= 0 || size.height() <= 0) return;
    if (m_accum[0] && m_pixelSize == size) return;

    releaseAccumResources();
    m_pixelSize = size;

    for (int i = 0; i < 2; ++i) {
        m_accum[i] = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                       QRhiTexture::RenderTarget);
        m_accum[i]->create();
        QRhiTextureRenderTargetDescription desc{ QRhiColorAttachment(m_accum[i]) };
        m_accumRt[i] = m_rhi->newTextureRenderTarget(desc);
        if (!m_accumRpDesc) {
            m_accumRpDesc = m_accumRt[i]->newCompatibleRenderPassDescriptor();
        }
        m_accumRt[i]->setRenderPassDescriptor(m_accumRpDesc);
        m_accumRt[i]->create();
    }

    for (int i = 0; i < 2; ++i) {
        const int readIdx = 1 - i;
        m_decaySrbRead[i] = m_rhi->newShaderResourceBindings();
        m_decaySrbRead[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::FragmentStage, m_decayUBO),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_accum[readIdx], m_linearSampler)
        });
        m_decaySrbRead[i]->create();

        m_compositeSrbRead[i] = m_rhi->newShaderResourceBindings();
        m_compositeSrbRead[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::FragmentStage, m_compositeUBO),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_accum[readIdx], m_linearSampler)
        });
        m_compositeSrbRead[i]->create();
    }

    m_accumRead  = 0;
    m_accumWrite = 1;
    m_accumPrimed = false;
}


// ---------------------------------------------------------------------------
//  Pipeline construction (lazy, after first render target sight)
// ---------------------------------------------------------------------------

void HilbertRosetteImpl::buildPipelinesIfNeeded()
{
    if (m_dotPipe) return;
    if (!m_accumRpDesc) return;
    if (!renderTarget()) return;

    m_compositeRpDesc = renderTarget()->renderPassDescriptor();

    // Decay pipeline: full-screen quad -> accum (no blend, writes raw
    // faded pixels).
    QShader decayVS = loadShader(QStringLiteral(":/shaders/hilbert_decay.vert.qsb"));
    QShader decayFS = loadShader(QStringLiteral(":/shaders/hilbert_decay.frag.qsb"));

    QRhiVertexInputLayout fsLayout;
    fsLayout.setBindings({ QRhiVertexInputBinding(2 * sizeof(float)) });
    fsLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)
    });

    m_decayPipe = m_rhi->newGraphicsPipeline();
    m_decayPipe->setTopology(QRhiGraphicsPipeline::Triangles);
    m_decayPipe->setShaderStages({
        { QRhiShaderStage::Vertex,   decayVS },
        { QRhiShaderStage::Fragment, decayFS }
    });
    m_decayPipe->setVertexInputLayout(fsLayout);
    m_decayPipe->setShaderResourceBindings(m_decaySrbRead[0]);
    m_decayPipe->setRenderPassDescriptor(m_accumRpDesc);
    QRhiGraphicsPipeline::TargetBlend tbDecay;
    tbDecay.enable = false;
    m_decayPipe->setTargetBlends({ tbDecay });
    m_decayPipe->create();

    // Composite pipeline: same shaders, but output to colorTexture and
    // multiply by composite-side tint (white in our case).
    m_compositePipe = m_rhi->newGraphicsPipeline();
    m_compositePipe->setTopology(QRhiGraphicsPipeline::Triangles);
    m_compositePipe->setShaderStages({
        { QRhiShaderStage::Vertex,   decayVS },
        { QRhiShaderStage::Fragment, decayFS }
    });
    m_compositePipe->setVertexInputLayout(fsLayout);
    m_compositePipe->setShaderResourceBindings(m_compositeSrbRead[0]);
    m_compositePipe->setRenderPassDescriptor(m_compositeRpDesc);
    QRhiGraphicsPipeline::TargetBlend tbComp;
    tbComp.enable = false;
    m_compositePipe->setTargetBlends({ tbComp });
    m_compositePipe->create();

    // Dot pipeline: per-dot quad, additive blend into accum.
    QShader dotVS = loadShader(QStringLiteral(":/shaders/hilbert_dot.vert.qsb"));
    QShader dotFS = loadShader(QStringLiteral(":/shaders/hilbert_dot.frag.qsb"));

    QRhiVertexInputLayout dotLayout;
    dotLayout.setBindings({
        QRhiVertexInputBinding(kFloatsPerVtx * sizeof(float))
    });
    dotLayout.setAttributes({
        // a_corner (vec2)
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        // a_center (vec2)
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)),
        // a_bandIdx (float)
        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float,  4 * sizeof(float)),
    });

    m_dotSrb = m_rhi->newShaderResourceBindings();
    m_dotSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage |
            QRhiShaderResourceBinding::FragmentStage,
            m_dotUBO)
    });
    m_dotSrb->create();

    m_dotPipe = m_rhi->newGraphicsPipeline();
    m_dotPipe->setTopology(QRhiGraphicsPipeline::Triangles);
    m_dotPipe->setShaderStages({
        { QRhiShaderStage::Vertex,   dotVS },
        { QRhiShaderStage::Fragment, dotFS }
    });
    m_dotPipe->setVertexInputLayout(dotLayout);
    m_dotPipe->setShaderResourceBindings(m_dotSrb);
    m_dotPipe->setRenderPassDescriptor(m_accumRpDesc);
    QRhiGraphicsPipeline::TargetBlend tbAdd;
    tbAdd.enable    = true;
    tbAdd.srcColor  = QRhiGraphicsPipeline::One;
    tbAdd.dstColor  = QRhiGraphicsPipeline::One;
    tbAdd.opColor   = QRhiGraphicsPipeline::Add;
    tbAdd.srcAlpha  = QRhiGraphicsPipeline::One;
    tbAdd.dstAlpha  = QRhiGraphicsPipeline::One;
    tbAdd.opAlpha   = QRhiGraphicsPipeline::Add;
    m_dotPipe->setTargetBlends({ tbAdd });
    m_dotPipe->create();
}


// ---------------------------------------------------------------------------
//  Static buffers (indices + fs quad)
// ---------------------------------------------------------------------------

void HilbertRosetteImpl::uploadStaticGeometry(QRhiResourceUpdateBatch* batch)
{
    if (m_staticUploaded) return;

    // Dot index buffer: 6 indices per dot, two CCW triangles.
    std::vector<uint16_t> dotIdx(size_t(kNBands * kIndicesPerDot));
    for (int d = 0; d < kNBands; ++d) {
        const uint16_t base = uint16_t(d * kVerticesPerDot);
        uint16_t* o = dotIdx.data() + d * kIndicesPerDot;
        o[0] = base + 0; o[1] = base + 1; o[2] = base + 2;
        o[3] = base + 2; o[4] = base + 1; o[5] = base + 3;
    }
    batch->uploadStaticBuffer(m_dotIB, 0, kDotsIndexBytes, dotIdx.data());

    // Fullscreen quad VB (two CCW triangles spanning NDC [-1,+1]).
    static const float fs[12] = {
        -1.f, -1.f,   1.f, -1.f,   -1.f,  1.f,
        -1.f,  1.f,   1.f, -1.f,    1.f,  1.f
    };
    batch->uploadStaticBuffer(m_fsVB, 0, sizeof(fs), fs);

    m_staticUploaded = true;
}


// ---------------------------------------------------------------------------
//  synchronize -- GUI -> render-thread state copy
// ---------------------------------------------------------------------------

void HilbertRosetteImpl::synchronize(QQuickRhiItem* item)
{
    auto* hr = static_cast<HilbertRosette*>(item);

    m_trailTau   = hr->trailTau();
    m_dotRadius  = hr->dotRadius();
    m_ringRadius = hr->ringRadius();

    if (hr->m_stagedDirty.exchange(false, std::memory_order_acquire)) {
        QMutexLocker lk(&hr->m_stageMutex);
        m_env   = hr->m_stagedEnv;
        m_phase = hr->m_stagedPhase;
    }
}


// ---------------------------------------------------------------------------
//  render -- three-pass pipeline (decay+dots into accum, then composite)
// ---------------------------------------------------------------------------

void HilbertRosetteImpl::render(QRhiCommandBuffer* cb)
{
    if (!m_rhi) return;
    QRhiTexture* color = colorTexture();
    if (!color) return;
    const QSize size = color->pixelSize();

    ensureAccumResources(size);
    if (!m_accumRt[0]) return;
    buildPipelinesIfNeeded();
    if (!m_dotPipe || !m_decayPipe || !m_compositePipe) return;

    auto* upload = m_rhi->nextResourceUpdateBatch();
    uploadStaticGeometry(upload);

    // -------------------------------------------------------------------
    //  Dot vertex buffer.
    //
    //  Place each dot at center + r * (cos, sin)(base + phase) where:
    //    center = (W/2, H/2) in pixels
    //    r      = (0.3 + env * 0.5) * m_ringRadius * 0.5 * min(W, H)
    //
    //  The +0.3 minimum keeps the dot off the centre even on silence;
    //  the 0.5*env contribution lets a high-amplitude dot swing out to
    //  ~80% of the available radius. (Multiplying by m_ringRadius and
    //  again by 0.5*min(W,H) just rescales "1.0 = touching the edge of
    //  the visible orbit" to pixels.)
    // -------------------------------------------------------------------

    const float W = float(size.width());
    const float H = float(size.height());
    const float cx = W * 0.5f;
    const float cy = H * 0.5f;
    const float rMax = 0.5f * std::min(W, H) * m_ringRadius;
    // sigma in px controls how spread the Gaussian splat is. The
    // fragment uses exp(-d^2 / (2*sigma^2)); we want the visible disk to
    // be roughly m_dotRadius across, so set sigma = dotRadius * 0.5 and
    // build a quad of half-width 3*sigma so the Gaussian fades smoothly.
    const float sigma = std::max(1.0f, m_dotRadius * 0.5f);
    const float halfWidth = 3.0f * sigma;

    static const float corners[4][2] = {
        { -1.f, -1.f }, {  1.f, -1.f },
        { -1.f,  1.f }, {  1.f,  1.f }
    };

    const size_t dotVbFloats =
        size_t(kNBands) * kVerticesPerDot * kFloatsPerVtx;
    if (m_dotVbScratch.size() < dotVbFloats)
        m_dotVbScratch.resize(dotVbFloats);

    for (int b = 0; b < kNBands; ++b) {
        const float env = std::clamp(m_env[b], 0.0f, 1.5f);
        const float r   = (0.3f + env * 0.5f) * rMax;
        const float ang = m_baseAngle[b] + m_phase[b];
        const float dx  = std::cos(ang) * r;
        const float dy  = std::sin(ang) * r;
        const float px  = cx + dx;
        const float py  = cy + dy;

        float* o = m_dotVbScratch.data()
                 + size_t(b) * kVerticesPerDot * kFloatsPerVtx;
        for (int c = 0; c < kVerticesPerDot; ++c) {
            // a_corner: corner-relative in [-halfWidth, +halfWidth]
            // pre-scaled here so the vert shader doesn't need the
            // halfWidth uniform.
            o[0] = corners[c][0] * halfWidth;
            o[1] = corners[c][1] * halfWidth;
            // a_center: dot center in pixel space.
            o[2] = px;
            o[3] = py;
            // a_bandIdx: float band index; the frag shader reads
            // bandColor[int(round(a_bandIdx))] from the UBO array.
            o[4] = float(b);
            o += kFloatsPerVtx;
        }
    }
    upload->updateDynamicBuffer(m_dotVB, 0,
                                kDotsVertexBytes, m_dotVbScratch.data());

    // -------------------------------------------------------------------
    //  UBOs.
    // -------------------------------------------------------------------
    {
        DotUbo u{};
        u.viewportPx[0] = W;
        u.viewportPx[1] = H;
        u.sigma         = sigma;
        // Brightness scale: 1.5 keeps a bright dot legible after the
        // additive accumulate decays. Tunable if it ever blows out.
        u.intensity     = 1.5f;
        for (int b = 0; b < kNBands; ++b) {
            u.bandColor[b][0] = m_bandColor[b][0];
            u.bandColor[b][1] = m_bandColor[b][1];
            u.bandColor[b][2] = m_bandColor[b][2];
            u.bandColor[b][3] = m_bandColor[b][3];
        }
        upload->updateDynamicBuffer(m_dotUBO, 0, sizeof(u), &u);
    }

    // Decay factor = exp(-dt/tau); we assume 60 Hz refresh as the rest
    // of the visualizer stack does.
    const float dt = 1.0f / 60.0f;
    const float tau = std::max(1e-3f, m_trailTau);
    const float decayFactor = std::exp(-dt / tau);

    {
        DecayUbo u{};
        u.decayFactor = m_accumPrimed ? decayFactor : 0.0f;
        u.tint[0] = u.tint[1] = u.tint[2] = u.tint[3] = 1.0f;
        upload->updateDynamicBuffer(m_decayUBO, 0, sizeof(u), &u);
    }
    {
        DecayUbo u{};
        u.decayFactor = 1.0f;
        u.tint[0] = u.tint[1] = u.tint[2] = u.tint[3] = 1.0f;
        upload->updateDynamicBuffer(m_compositeUBO, 0, sizeof(u), &u);
    }

    // -------------------------------------------------------------------
    //  Pass A: clear -> decay-into-accum -> additive dots
    // -------------------------------------------------------------------
    cb->beginPass(m_accumRt[m_accumWrite],
                  QColor(0, 0, 0, 0),
                  { 1.0f, 0 },
                  upload);
    const QRhiViewport accumVp(0, 0,
                               float(size.width()),
                               float(size.height()));
    cb->setViewport(accumVp);
    cb->setGraphicsPipeline(m_decayPipe);
    cb->setShaderResources(m_decaySrbRead[m_accumWrite]);
    {
        QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
    }
    cb->setGraphicsPipeline(m_dotPipe);
    cb->setShaderResources(m_dotSrb);
    {
        QRhiCommandBuffer::VertexInput vin(m_dotVB, 0);
        cb->setVertexInput(0, 1, &vin,
                           m_dotIB, 0, QRhiCommandBuffer::IndexUInt16);
        cb->drawIndexed(kNBands * kIndicesPerDot);
    }
    cb->endPass();

    // -------------------------------------------------------------------
    //  Pass B: composite accum -> visible target.
    // -------------------------------------------------------------------
    cb->beginPass(renderTarget(),
                  QColor(0, 0, 0, 1),
                  { 1.0f, 0 });
    {
        const QSize cs = renderTarget()->pixelSize();
        const QRhiViewport vp(0, 0, float(cs.width()), float(cs.height()));
        cb->setViewport(vp);
    }
    cb->setGraphicsPipeline(m_compositePipe);
    cb->setShaderResources(m_compositeSrbRead[1 - m_accumWrite]);
    {
        QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
    }
    cb->endPass();

    m_accumPrimed = true;
    std::swap(m_accumRead, m_accumWrite);

    // Re-request a frame -- the phosphor fade keeps moving even when no
    // new audio arrives (silence frames decay the prior dots smoothly).
    update();
}


// ---------------------------------------------------------------------------
//  HilbertRosette (GUI thread)
// ---------------------------------------------------------------------------

HilbertRosette::HilbertRosette(QQuickItem* parent)
    : QQuickRhiItem(parent)
    , m_analyzer(new HilbertAnalyzer(this))
{
    setMirrorVertically(true);
    setAlphaBlending(true);
    setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA8);

    connect(m_analyzer, &HilbertAnalyzer::bandsUpdated,
            this, &HilbertRosette::onBandsUpdated,
            Qt::DirectConnection);
}

HilbertRosette::~HilbertRosette() = default;

QQuickRhiItemRenderer* HilbertRosette::createRenderer()
{
    return new HilbertRosetteImpl(this);
}


void HilbertRosette::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    m_source = s;
    m_analyzer->setAudioSource(s);
    emit audioSourceChanged();
}

void HilbertRosette::setTrailTau(float v)
{
    v = std::clamp(v, 0.01f, 5.0f);
    if (m_trailTau == v) return;
    m_trailTau = v;
    emit trailTauChanged();
    update();
}

void HilbertRosette::setDotRadius(float v)
{
    v = std::clamp(v, 1.0f, 64.0f);
    if (m_dotRadius == v) return;
    m_dotRadius = v;
    emit dotRadiusChanged();
    update();
}

void HilbertRosette::setRingRadius(float v)
{
    v = std::clamp(v, 0.05f, 1.0f);
    if (m_ringRadius == v) return;
    m_ringRadius = v;
    emit ringRadiusChanged();
    update();
}

void HilbertRosette::setShowBaseRing(bool v)
{
    if (m_showBaseRing == v) return;
    m_showBaseRing = v;
    emit showBaseRingChanged();
    update();
}

void HilbertRosette::setShowBandLabels(bool v)
{
    if (m_showBandLabels == v) return;
    m_showBandLabels = v;
    emit showBandLabelsChanged();
    update();
}

QVariantList HilbertRosette::bandCenters() const
{
    return m_analyzer ? m_analyzer->bandCenters() : QVariantList{};
}


void HilbertRosette::setActive(bool a)
{
    if (m_active.load(std::memory_order_relaxed) == a) return;
    m_active.store(a, std::memory_order_relaxed);
    if (m_analyzer) m_analyzer->setActive(a);
    emit activeChanged();
}


void HilbertRosette::onBandsUpdated()
{
    if (!m_active.load(std::memory_order_relaxed)) return;
    if (!m_analyzer) return;
    std::array<float, N_BANDS> env {}, phase {}, freq {};
    if (!m_analyzer->fillBandStates(env.data(), phase.data(), freq.data()))
        return;
    {
        QMutexLocker lk(&m_stageMutex);
        m_stagedEnv   = env;
        m_stagedPhase = phase;
        m_stagedFreq  = freq;
    }
    m_stagedDirty.store(true, std::memory_order_release);
    update();
}
