#include "ScopeRenderer.h"
#include "AudioFeatures.h"

#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>


// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr int kSampleCount  = ScopeRenderer::SAMPLE_COUNT;
constexpr int kSegmentCount = ScopeRenderer::SEGMENT_COUNT;

// Per-segment vertex layout: 4 vertices, each 6 floats (p0xy, p1xy, corner_xy).
// = 4 * 6 * 4 B = 96 B/segment. 1023 segments → ~96 KB. Indexed with 6
// uint16 indices per segment → 1023 * 6 * 2 = 12 KB index buffer.
constexpr int kFloatsPerVertex = 6;
constexpr int kVerticesPerSeg  = 4;
constexpr int kIndicesPerSeg   = 6;
constexpr int kSegmentVertexBytes =
    kSegmentCount * kVerticesPerSeg * kFloatsPerVertex * int(sizeof(float));
constexpr int kSegmentIndexBytes =
    kSegmentCount * kIndicesPerSeg * int(sizeof(uint16_t));

// std140 UBO layouts. std140 alignment rules:
//   float, int  → 4 B align
//   vec2        → 8 B align
//   vec4, mat4  → 16 B align
//   struct itself padded to vec4 alignment.
// Each member's offset must be a multiple of its alignment.
struct SegmentUbo {
    float qt_Matrix[16];   // off 0,  size 64 (mat4)
    float qt_Opacity;      // off 64, size 4
    float _pad0[1];        // off 68, size 4  → align next vec2 to 8
    float viewportPx[2];   // off 72, size 8  (vec2)
    float sigma;           // off 80, size 4
    float intensity;       // off 84, size 4
    float _pad1[2];        // off 88-95 → struct end on 16-byte multiple
};                         // total: 96 B

struct DecayUbo {
    float qt_Matrix[16];   // off 0,  size 64
    float qt_Opacity;      // off 64, size 4
    float decayFactor;     // off 68, size 4
    float _pad0[2];        // off 72-79 → align next vec4 to 16
    float tint[4];         // off 80, size 16 (vec4)
};                         // total: 96 B

QShader loadShader(const QString& resourcePath)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

// Identity 4×4 matrix as a flat array, std140 row-major mirror.
constexpr std::array<float, 16> kIdentity4 = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1
};

inline void writeIdentity(float* dst) { std::memcpy(dst, kIdentity4.data(), 16 * sizeof(float)); }

}  // namespace


// ---------------------------------------------------------------------------
//  ScopeRendererImpl — runs on the render thread.
// ---------------------------------------------------------------------------

class ScopeRendererImpl : public QQuickRhiItemRenderer {
public:
    explicit ScopeRendererImpl(ScopeRenderer* item) : m_item(item) {}

    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void releaseAccumResources();
    void ensureAccumResources(const QSize& size);

    ScopeRenderer* m_item;
    QRhi*          m_rhi = nullptr;
    QSize          m_pixelSize;

    // Persistent ping-pong accumulation textures + their render targets.
    QRhiTexture*               m_accum[2]   = { nullptr, nullptr };
    QRhiTextureRenderTarget*   m_accumRt[2] = { nullptr, nullptr };
    QRhiRenderPassDescriptor*  m_accumRpDesc = nullptr;
    int m_accumRead  = 0;   // sampled this frame (previous result)
    int m_accumWrite = 1;   // rendered into this frame
    bool m_accumPrimed = false;  // false until first decay pass has run

    // Sampler (linear, clamp) used for both decay and composite passes.
    QRhiSampler* m_linearSampler = nullptr;

    // Segment pipeline (1023 quads, additive blend into accumulation).
    QRhiBuffer*               m_segVB     = nullptr;
    QRhiBuffer*               m_segIB     = nullptr;
    QRhiBuffer*               m_segUBO    = nullptr;
    QRhiShaderResourceBindings* m_segSrb  = nullptr;
    QRhiGraphicsPipeline*     m_segPipe   = nullptr;

    // Decay / composite pipeline (fullscreen quad + sampler).
    QRhiBuffer*               m_fsVB           = nullptr;
    QRhiBuffer*               m_decayUBO       = nullptr;
    QRhiShaderResourceBindings* m_decaySrbRead[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*     m_decayPipe      = nullptr;   // → accum (no blend)
    QRhiBuffer*               m_compositeUBO   = nullptr;
    QRhiShaderResourceBindings* m_compositeSrbRead[2] = { nullptr, nullptr };
    QRhiGraphicsPipeline*     m_compositePipe  = nullptr;   // → colorTexture (no blend)

    // Render-pass descriptor for compositing onto the QQuickRhiItem's
    // own color target (created lazily once we see the live target).
    QRhiRenderPassDescriptor* m_compositeRpDesc = nullptr;

    // Cached segment indices — never change, uploaded once.
    bool m_indicesUploaded = false;

    // Synchronized snapshot of state pulled from the GUI thread.
    std::vector<float> m_endpointsPx;
    float  m_sigma         = 1.5f;
    float  m_decayFactor   = 0.5f;
    float  m_beamIntensity = 1.0f;
    float  m_tint[4]       = { 0.2f, 1.0f, 0.2f, 1.0f };

    // Scratch for the per-frame segment vertex assembly. Heap-allocated
    // once on the render thread to keep the 96 KB off the thread's stack.
    std::vector<float> m_vbScratch;
};


// ---------------------------------------------------------------------------
//  ScopeRendererImpl::initialize — first-frame resource creation
// ---------------------------------------------------------------------------

void ScopeRendererImpl::initialize(QRhiCommandBuffer* /*cb*/)
{
    m_rhi = rhi();
    if (!m_rhi) return;

    if (!m_linearSampler) {
        m_linearSampler = m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        m_linearSampler->create();
    }

    if (!m_segVB) {
        m_segVB = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                   QRhiBuffer::VertexBuffer,
                                   kSegmentVertexBytes);
        m_segVB->create();
    }
    if (!m_segIB) {
        m_segIB = m_rhi->newBuffer(QRhiBuffer::Immutable,
                                   QRhiBuffer::IndexBuffer,
                                   kSegmentIndexBytes);
        m_segIB->create();
    }
    if (!m_segUBO) {
        m_segUBO = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                    QRhiBuffer::UniformBuffer,
                                    sizeof(SegmentUbo));
        m_segUBO->create();
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
}


// ---------------------------------------------------------------------------
//  ScopeRendererImpl::releaseAccumResources / ensureAccumResources
// ---------------------------------------------------------------------------

void ScopeRendererImpl::releaseAccumResources()
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

void ScopeRendererImpl::ensureAccumResources(const QSize& size)
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

    // Build per-direction SRBs that read from the *other* accum texture.
    // Frame N samples accum[read] and writes accum[write]; the SRB binds
    // accum[read] at binding=1 of the decay/composite shader.
    for (int i = 0; i < 2; ++i) {
        const int readIdx = 1 - i;

        m_decaySrbRead[i] = m_rhi->newShaderResourceBindings();
        m_decaySrbRead[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_decayUBO),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_accum[readIdx], m_linearSampler)
        });
        m_decaySrbRead[i]->create();

        m_compositeSrbRead[i] = m_rhi->newShaderResourceBindings();
        m_compositeSrbRead[i]->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_compositeUBO),
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
//  ScopeRendererImpl::synchronize — GUI ⇄ render-thread state copy
// ---------------------------------------------------------------------------

void ScopeRendererImpl::synchronize(QQuickRhiItem* item)
{
    auto* sr = static_cast<ScopeRenderer*>(item);

    m_sigma         = sr->sigma();
    m_beamIntensity = sr->beamIntensity();
    QColor c = sr->beamColor();
    m_tint[0] = float(c.redF());
    m_tint[1] = float(c.greenF());
    m_tint[2] = float(c.blueF());
    m_tint[3] = 1.0f;

    // tau → per-frame multiplier: e^(-dt/τ). Assume 60 Hz refresh (dt = 1/60).
    const float dt  = 1.0f / 60.0f;
    const float tau = std::max(1e-3f, sr->decay());
    m_decayFactor   = std::exp(-dt / tau);

    if (sr->m_endpointsDirty.exchange(false, std::memory_order_acquire)) {
        QMutexLocker lk(&sr->m_stageMutex);
        m_endpointsPx.resize(sr->m_stagedEndpointsPx.size());
        std::memcpy(m_endpointsPx.data(),
                    sr->m_stagedEndpointsPx.data(),
                    sr->m_stagedEndpointsPx.size() * sizeof(float));
    } else if (m_endpointsPx.empty()) {
        m_endpointsPx.assign(kSampleCount * 2, 0.0f);
    }
}


// ---------------------------------------------------------------------------
//  ScopeRendererImpl::render — three-pass pipeline
// ---------------------------------------------------------------------------

void ScopeRendererImpl::render(QRhiCommandBuffer* cb)
{
    if (!m_rhi) return;

    QRhiTexture* color = colorTexture();
    if (!color) return;
    const QSize size = color->pixelSize();

    ensureAccumResources(size);
    if (!m_accumRt[0]) return;

    // Build composite pipeline lazily once the color RT exists. The RT
    // descriptor is provided by the QQuickRhiItem framework via the
    // base class's renderTarget(), so we ask it for an RP descriptor.
    if (!m_compositePipe) {
        m_compositeRpDesc = renderTarget()->renderPassDescriptor();

        // Build decay pipeline (output: accum, no blending).
        QShader decayVS = loadShader(QStringLiteral(":/shaders/scope_decay.vert.qsb"));
        QShader decayFS = loadShader(QStringLiteral(":/shaders/scope_decay.frag.qsb"));

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
        tbDecay.enable = false;     // writes raw faded pixels
        m_decayPipe->setTargetBlends({ tbDecay });
        m_decayPipe->create();

        // Build composite pipeline (output: colorTexture, no blending).
        m_compositePipe = m_rhi->newGraphicsPipeline();
        m_compositePipe->setTopology(QRhiGraphicsPipeline::Triangles);
        m_compositePipe->setShaderStages({
            { QRhiShaderStage::Vertex,   decayVS },   // same VS
            { QRhiShaderStage::Fragment, decayFS }    // same FS — different uniforms
        });
        m_compositePipe->setVertexInputLayout(fsLayout);
        m_compositePipe->setShaderResourceBindings(m_compositeSrbRead[0]);
        m_compositePipe->setRenderPassDescriptor(m_compositeRpDesc);
        QRhiGraphicsPipeline::TargetBlend tbComp;
        tbComp.enable = false;
        m_compositePipe->setTargetBlends({ tbComp });
        m_compositePipe->create();

        // Build segment pipeline (output: accum, additive blend).
        QShader segVS = loadShader(QStringLiteral(":/shaders/scope_segment.vert.qsb"));
        QShader segFS = loadShader(QStringLiteral(":/shaders/scope_segment.frag.qsb"));

        QRhiVertexInputLayout segLayout;
        segLayout.setBindings({ QRhiVertexInputBinding(kFloatsPerVertex * sizeof(float)) });
        segLayout.setAttributes({
            // a_p0, a_p1, a_corner
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
            QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)),
            QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float2, 4 * sizeof(float))
        });

        m_segSrb = m_rhi->newShaderResourceBindings();
        m_segSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_segUBO)
        });
        m_segSrb->create();

        m_segPipe = m_rhi->newGraphicsPipeline();
        m_segPipe->setTopology(QRhiGraphicsPipeline::Triangles);
        m_segPipe->setShaderStages({
            { QRhiShaderStage::Vertex,   segVS },
            { QRhiShaderStage::Fragment, segFS }
        });
        m_segPipe->setVertexInputLayout(segLayout);
        m_segPipe->setShaderResourceBindings(m_segSrb);
        m_segPipe->setRenderPassDescriptor(m_accumRpDesc);
        QRhiGraphicsPipeline::TargetBlend tbAdd;
        tbAdd.enable    = true;
        tbAdd.srcColor  = QRhiGraphicsPipeline::One;
        tbAdd.dstColor  = QRhiGraphicsPipeline::One;
        tbAdd.opColor   = QRhiGraphicsPipeline::Add;
        tbAdd.srcAlpha  = QRhiGraphicsPipeline::One;
        tbAdd.dstAlpha  = QRhiGraphicsPipeline::One;
        tbAdd.opAlpha   = QRhiGraphicsPipeline::Add;
        m_segPipe->setTargetBlends({ tbAdd });
        m_segPipe->create();
    }

    // --------------------------------------------------------------
    //  Upload per-frame data: segment vertex buffer + 3 UBOs.
    // --------------------------------------------------------------
    auto* upload = m_rhi->nextResourceUpdateBatch();

    // ---- Build segment vertex buffer from m_endpointsPx ------------
    if (m_endpointsPx.size() >= size_t(kSampleCount * 2)) {
        // 4 corners; their (cornerU, cornerV) in [-1,+1]² square.
        static const float corners[4][2] = {
            { -1.f, -1.f },
            {  1.f, -1.f },
            { -1.f,  1.f },
            {  1.f,  1.f }
        };
        const size_t vbFloats =
            size_t(kSegmentCount) * kVerticesPerSeg * kFloatsPerVertex;
        if (m_vbScratch.size() < vbFloats) m_vbScratch.resize(vbFloats);
        for (int s = 0; s < kSegmentCount; ++s) {
            const float p0x = m_endpointsPx[s * 2 + 0];
            const float p0y = m_endpointsPx[s * 2 + 1];
            const float p1x = m_endpointsPx[(s + 1) * 2 + 0];
            const float p1y = m_endpointsPx[(s + 1) * 2 + 1];
            float* o = m_vbScratch.data() + s * kVerticesPerSeg * kFloatsPerVertex;
            for (int c = 0; c < kVerticesPerSeg; ++c) {
                o[0] = p0x; o[1] = p0y;
                o[2] = p1x; o[3] = p1y;
                o[4] = corners[c][0];
                o[5] = corners[c][1];
                o += kFloatsPerVertex;
            }
        }
        upload->updateDynamicBuffer(m_segVB, 0,
                                    kSegmentVertexBytes, m_vbScratch.data());
    }

    // ---- Static-data uploads (one-shot, gated by m_indicesUploaded).
    //      Combined here because they all need a live cb / resource
    //      batch — initialize() runs before any pass starts.
    if (!m_indicesUploaded) {
        // Segment index buffer.
        std::vector<uint16_t> idx(kSegmentCount * kIndicesPerSeg);
        for (int s = 0; s < kSegmentCount; ++s) {
            const uint16_t base = uint16_t(s * kVerticesPerSeg);
            uint16_t* o = idx.data() + s * kIndicesPerSeg;
            o[0] = base + 0; o[1] = base + 1; o[2] = base + 2;
            o[3] = base + 2; o[4] = base + 1; o[5] = base + 3;
        }
        upload->uploadStaticBuffer(m_segIB, 0, kSegmentIndexBytes, idx.data());

        // Fullscreen-quad VB (two CCW triangles spanning [-1,+1]).
        static const float fs[12] = {
            -1.f, -1.f,   1.f, -1.f,   -1.f,  1.f,
            -1.f,  1.f,   1.f, -1.f,    1.f,  1.f
        };
        upload->uploadStaticBuffer(m_fsVB, 0, sizeof(fs), fs);

        m_indicesUploaded = true;
    }

    // ---- Segment UBO ----------------------------------------------
    {
        SegmentUbo u{};
        writeIdentity(u.qt_Matrix);
        u.qt_Opacity   = 1.0f;
        u.viewportPx[0] = float(size.width());
        u.viewportPx[1] = float(size.height());
        u.sigma        = m_sigma;
        u.intensity    = m_beamIntensity;
        upload->updateDynamicBuffer(m_segUBO, 0, sizeof(u), &u);
    }

    // ---- Decay UBO  (decay pass: tint = (1,1,1,1)) ----------------
    {
        DecayUbo u{};
        writeIdentity(u.qt_Matrix);
        u.qt_Opacity  = 1.0f;
        u.decayFactor = m_accumPrimed ? m_decayFactor : 0.0f;  // first frame = clear
        u.tint[0] = u.tint[1] = u.tint[2] = u.tint[3] = 1.0f;
        upload->updateDynamicBuffer(m_decayUBO, 0, sizeof(u), &u);
    }

    // ---- Composite UBO (decay = 1, tint = beamColor) -------------
    {
        DecayUbo u{};
        writeIdentity(u.qt_Matrix);
        u.qt_Opacity  = 1.0f;
        u.decayFactor = 1.0f;
        u.tint[0] = m_tint[0];
        u.tint[1] = m_tint[1];
        u.tint[2] = m_tint[2];
        u.tint[3] = 1.0f;
        upload->updateDynamicBuffer(m_compositeUBO, 0, sizeof(u), &u);
    }

    // --------------------------------------------------------------
    //  Pass A — accum[write] = accum[read] * decay  +  Σ segments (additive).
    //
    //  Both draws share a single render pass so the decay output stays
    //  in the bound target without round-tripping through main memory.
    //  beginPass clears accum[write] to (0,0,0,0); the decay pipeline
    //  (no blending) overwrites the cleared pixels with prev*decay; then
    //  the segment pipeline (additive blend One/One) lays beam intensity
    //  on top.
    // --------------------------------------------------------------
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
    cb->setGraphicsPipeline(m_segPipe);
    cb->setShaderResources(m_segSrb);
    {
        QRhiCommandBuffer::VertexInput vin(m_segVB, 0);
        cb->setVertexInput(0, 1, &vin,
                           m_segIB, 0, QRhiCommandBuffer::IndexUInt16);
        cb->drawIndexed(kSegmentCount * kIndicesPerSeg);
    }
    cb->endPass();

    // --------------------------------------------------------------
    //  Pass B — composite accum[write] * beamColor → visible target.
    // --------------------------------------------------------------
    cb->beginPass(renderTarget(),
                  QColor(0, 0, 0, 1),
                  { 1.0f, 0 });
    {
        const QSize cs = renderTarget()->pixelSize();
        const QRhiViewport vp(0, 0, float(cs.width()), float(cs.height()));
        cb->setViewport(vp);
    }
    cb->setGraphicsPipeline(m_compositePipe);
    // Composite samples whatever we just rendered into — that's
    // accum[write]. m_compositeSrbRead[w] reads accum[1-w], so we pass
    // the opposite index here to read accum[write].
    cb->setShaderResources(m_compositeSrbRead[1 - m_accumWrite]);
    {
        QRhiCommandBuffer::VertexInput vin(m_fsVB, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
    }
    cb->endPass();

    // Swap ping-pong indices for the next frame.
    m_accumPrimed = true;
    std::swap(m_accumRead, m_accumWrite);

    // Request another frame so the phosphor decay continues to fade
    // even when no new audio data arrives (e.g. paused playback shows
    // the last frame's trace decaying to black instead of freezing).
    update();
}


// ---------------------------------------------------------------------------
//  ScopeRenderer (item) — lives on the GUI thread
// ---------------------------------------------------------------------------

ScopeRenderer::ScopeRenderer(QQuickItem* parent)
    : QQuickRhiItem(parent)
    , m_sampleL(SAMPLE_COUNT, 0.0f)
    , m_sampleR(SAMPLE_COUNT, 0.0f)
    , m_endpointsXY(SAMPLE_COUNT * 2, 0.0f)
    , m_stagedEndpointsPx(SAMPLE_COUNT * 2, 0.0f)
{
    setMirrorVertically(true);   // QQuickRhiItem's default Y is top-down;
                                 // our shaders use bottom-up NDC.
    setAlphaBlending(true);
    setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA8);
}

ScopeRenderer::~ScopeRenderer() = default;

QQuickRhiItemRenderer* ScopeRenderer::createRenderer()
{
    return new ScopeRendererImpl(this);
}

void ScopeRenderer::setMode(Mode m)
{
    if (m_mode == m) return;
    m_mode = m;
    emit modeChanged();
    rebuildSegmentEndpoints();
}

void ScopeRenderer::setSigma(float v)
{
    v = std::clamp(v, 0.25f, 10.0f);
    if (m_sigma == v) return;
    m_sigma = v;
    emit sigmaChanged();
    update();
}

void ScopeRenderer::setDecay(float v)
{
    v = std::clamp(v, 0.001f, 5.0f);
    if (m_decay == v) return;
    m_decay = v;
    emit decayChanged();
    update();
}

void ScopeRenderer::setBeamIntensity(float v)
{
    v = std::clamp(v, 0.0f, 16.0f);
    if (m_beamIntensity == v) return;
    m_beamIntensity = v;
    emit beamIntensityChanged();
    update();
}

void ScopeRenderer::setBeamColor(const QColor& c)
{
    if (m_beamColor == c) return;
    m_beamColor = c;
    emit beamColorChanged();
    update();
}

void ScopeRenderer::setStereoSeparated(bool v)
{
    if (m_stereoSeparated == v) return;
    m_stereoSeparated = v;
    emit stereoSeparatedChanged();
    rebuildSegmentEndpoints();
}

void ScopeRenderer::setAxisRotation(float deg)
{
    if (m_axisRotation == deg) return;
    m_axisRotation = deg;
    emit axisRotationChanged();
    rebuildSegmentEndpoints();
}

void ScopeRenderer::setAudioGain(float v)
{
    v = std::clamp(v, 0.0f, 32.0f);
    if (m_audioGain == v) return;
    m_audioGain = v;
    emit audioGainChanged();
    rebuildSegmentEndpoints();
}

void ScopeRenderer::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    if (m_source) {
        disconnect(m_source, &AudioFeatures::featuresUpdated,
                   this, &ScopeRenderer::onFeaturesUpdated);
    }
    m_source = s;
    if (m_source) {
        connect(m_source, &AudioFeatures::featuresUpdated,
                this, &ScopeRenderer::onFeaturesUpdated,
                Qt::AutoConnection);
    }
    emit audioSourceChanged();
}

void ScopeRenderer::onFeaturesUpdated()
{
    if (!m_source) return;
    if (!m_source->fillScopeStereo(m_sampleL.data(), m_sampleR.data(), SAMPLE_COUNT))
        return;
    rebuildSegmentEndpoints();
}

void ScopeRenderer::rebuildSegmentEndpoints()
{
    const float W = float(std::max(1.0, width()));
    const float H = float(std::max(1.0, height()));

    auto clamp01 = [](float v) { return std::clamp(v, -1.0f, 1.0f); };

    if (m_mode == Mode::Oscilloscope) {
        if (m_stereoSeparated) {
            // L in top half (y ∈ [H/2, H]), R in bottom half (y ∈ [0, H/2]).
            // x sweeps across the same width for both. We pack each one
            // by interleaving: even indices = L, odd indices = R? No —
            // we want a continuous trace for each channel. Approach:
            // first SAMPLE_COUNT/2 indices = L sweep; second half = R
            // sweep. Then connect the two halves with a long segment
            // (acceptable visual artifact: it gets dimmed by the 1/L
            // velocity factor anyway).
            const int half = SAMPLE_COUNT / 2;
            for (int i = 0; i < half; ++i) {
                // L sample index — interpolate full L resolution into
                // half the sweep.
                const int li = (i * SAMPLE_COUNT) / half;
                const float u = float(i) / float(half - 1);
                const float sy = clamp01(m_sampleL[li] * m_audioGain);
                m_endpointsXY[i * 2 + 0] = u * W;
                // Top half: y = (3/4 + sy/4) * H
                m_endpointsXY[i * 2 + 1] = (0.75f + 0.25f * sy) * H;
            }
            for (int i = 0; i < half; ++i) {
                const int ri = (i * SAMPLE_COUNT) / half;
                const float u = float(i) / float(half - 1);
                const float sy = clamp01(m_sampleR[ri] * m_audioGain);
                m_endpointsXY[(half + i) * 2 + 0] = u * W;
                m_endpointsXY[(half + i) * 2 + 1] = (0.25f + 0.25f * sy) * H;
            }
        } else {
            // L+R overlaid as 512+512 paired traces. Keep it simple:
            // interleave L then R across the buffer so both channels show.
            const int half = SAMPLE_COUNT / 2;
            for (int i = 0; i < half; ++i) {
                const int li = (i * SAMPLE_COUNT) / half;
                const float u = float(i) / float(half - 1);
                const float sy = clamp01(m_sampleL[li] * m_audioGain);
                m_endpointsXY[i * 2 + 0] = u * W;
                m_endpointsXY[i * 2 + 1] = (0.5f + 0.5f * sy) * H;
            }
            for (int i = 0; i < half; ++i) {
                const int ri = (i * SAMPLE_COUNT) / half;
                const float u = float(i) / float(half - 1);
                const float sy = clamp01(m_sampleR[ri] * m_audioGain);
                m_endpointsXY[(half + i) * 2 + 0] = u * W;
                m_endpointsXY[(half + i) * 2 + 1] = (0.5f + 0.5f * sy) * H;
            }
        }
    } else {
        // Vectorscope: (x, y) = (L-R, L+R) / √2, optionally rotated by
        // (axisRotation - 45°). The canonical Mid/Side frame already
        // places mono on the vertical axis when plotted this way, so
        // axisRotation = 45° is the no-op default.
        const float cx = W * 0.5f, cy = H * 0.5f;
        const float scale = std::min(W, H) * 0.45f;
        const float invSqrt2 = 1.0f / std::sqrt(2.0f);
        for (int i = 0; i < SAMPLE_COUNT; ++i) {
            const float l = m_sampleL[i] * m_audioGain;
            const float r = m_sampleR[i] * m_audioGain;
            // Canonical Mid/Side axes — but we want mono → vertical at
            // rotation=45° default, so we use a slightly different mix:
            //   x_raw = L - R     (Side)
            //   y_raw = L + R     (Mid)
            // With rotation = 45° this lands mono on the y-axis.
            float x = (l - r) * invSqrt2;
            float y = (l + r) * invSqrt2;
            // Optional additional rotation (m_axisRotation - 45° is the
            // "extra" beyond the canonical S/M frame). Defaults give 0.
            const float ang = (m_axisRotation - 45.0f) * float(M_PI) / 180.0f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            const float xr = x * ca - y * sa;
            const float yr = x * sa + y * ca;
            // Clamp to display range; out-of-range samples just clip to
            // the edge. The 1/L velocity factor dims clipping streaks
            // for free.
            const float px = cx + std::clamp(xr, -1.0f, 1.0f) * scale;
            const float py = cy + std::clamp(yr, -1.0f, 1.0f) * scale;
            m_endpointsXY[i * 2 + 0] = px;
            m_endpointsXY[i * 2 + 1] = py;
        }
    }

    {
        QMutexLocker lk(&m_stageMutex);
        std::memcpy(m_stagedEndpointsPx.data(),
                    m_endpointsXY.data(),
                    m_endpointsXY.size() * sizeof(float));
    }
    m_endpointsDirty.store(true, std::memory_order_release);
    update();
}
