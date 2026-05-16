#include "MfccTrajectory.h"
#include "AudioFeatures.h"

#include <QFile>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>


// ---------------------------------------------------------------------------
//  Constants + helpers
// ---------------------------------------------------------------------------

namespace {

constexpr int kFeatureDim     = MfccTrajectory::FEATURE_DIM;     // 12
constexpr int kTrajectoryLen  = MfccTrajectory::TRAJECTORY_LEN;  // 600
constexpr int kSegmentCount   = kTrajectoryLen - 1;              // 599

// Per-segment vertex layout: 4 verts, each (p0xyz, p1xyz, ageA, ageB,
// corner_uv) = 3 + 3 + 1 + 1 + 2 = 10 floats. 599 segments * 4 verts *
// 10 floats * 4 B = ~94 KB VB. Index buffer: 599 * 6 indices * 2 B
// = ~7 KB.
constexpr int kFloatsPerVertex = 10;
constexpr int kVerticesPerSeg  = 4;
constexpr int kIndicesPerSeg   = 6;
constexpr int kSegmentVertexBytes =
    kSegmentCount * kVerticesPerSeg * kFloatsPerVertex * int(sizeof(float));
constexpr int kSegmentIndexBytes  =
    kSegmentCount * kIndicesPerSeg * int(sizeof(uint16_t));

// Eigenvalue floor for the Jacobi convergence test. We declare the matrix
// "diagonal enough" when every off-diagonal element is below this.
constexpr float kJacobiEps      = 1e-7f;
constexpr int   kJacobiMaxSweeps = 64;   // 12x12 typically converges in
                                         // ~10-15 sweeps; cap for safety.

// std140 UBO layout for the trajectory shader. std140 alignment rules:
//   float -> 4 B, vec2 -> 8 B, vec4 / mat4 -> 16 B, struct end -> vec4.
// Each member's offset must be a multiple of its alignment. We pad out
// to keep the struct size a 16-byte multiple so the GPU can ignore tail.
struct TrajUbo {
    float viewProj[16];   //  off 0,  size 64
    float viewportPx[2];  //  off 64, size 8
    float lineWidth;      //  off 72, size 4
    float maxAge;         //  off 76, size 4
    float headColor[4];   //  off 80, size 16
    float tailColor[4];   //  off 96, size 16
    float fogStart;       //  off 112, size 4
    float fogEnd;         //  off 116, size 4
    float _pad[2];        //  off 120-127 -> struct end on 16-B multiple
};                        //  total: 128 B
static_assert(sizeof(TrajUbo) == 128, "TrajUbo layout drift");

QShader loadShader(const QString& resourcePath)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QShader::fromSerialized(f.readAll());
}

// Build a perspective + lookAt matrix product in row-major 4x4. We hand-
// roll because we're not pulling in QMatrix4x4 just for this -- and the
// matrix shape we need (camera orbit, 3D->2D, no model transform) is
// trivial enough that doing it inline is cleaner than wrapping Qt's
// vector / matrix types and dealing with row vs column conventions.

inline std::array<float, 16> mat4Mul(const std::array<float, 16>& a,
                                     const std::array<float, 16>& b)
{
    std::array<float, 16> r {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    }
    return r;
}

// Perspective. fovY in radians, aspect = w/h.
std::array<float, 16> mat4Perspective(float fovY, float aspect,
                                      float zNear, float zFar)
{
    const float f = 1.0f / std::tan(0.5f * fovY);
    const float zRange = zNear - zFar;
    return {
        f / aspect, 0.0f, 0.0f,                        0.0f,
        0.0f,       f,    0.0f,                        0.0f,
        0.0f,       0.0f, (zFar + zNear) / zRange,     2.0f * zFar * zNear / zRange,
        0.0f,       0.0f, -1.0f,                       0.0f
    };
}

// Right-handed look-at: eye -> center, up world axis.
std::array<float, 16> mat4LookAt(const std::array<float, 3>& eye,
                                 const std::array<float, 3>& center,
                                 const std::array<float, 3>& up)
{
    auto sub = [](auto a, auto b) {
        return std::array<float, 3>{ a[0]-b[0], a[1]-b[1], a[2]-b[2] };
    };
    auto cross = [](auto a, auto b) {
        return std::array<float, 3>{
            a[1]*b[2]-a[2]*b[1],
            a[2]*b[0]-a[0]*b[2],
            a[0]*b[1]-a[1]*b[0]
        };
    };
    auto norm = [](auto a) {
        const float n = std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
        const float inv = (n > 1e-9f) ? 1.0f / n : 0.0f;
        return std::array<float, 3>{ a[0]*inv, a[1]*inv, a[2]*inv };
    };
    auto dot = [](auto a, auto b) {
        return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
    };

    const auto f = norm(sub(center, eye));   // forward
    const auto s = norm(cross(f, up));       // right
    const auto u = cross(s, f);              // recomputed up

    return {
        s[0],  s[1],  s[2],  -dot(s, eye),
        u[0],  u[1],  u[2],  -dot(u, eye),
        -f[0], -f[1], -f[2],  dot(f, eye),
        0.0f,  0.0f,  0.0f,   1.0f
    };
}

}  // namespace


// ---------------------------------------------------------------------------
//  Jacobi symmetric-matrix eigendecomposition (12x12 in our case).
//
//  Classic Jacobi rotation: repeatedly find the largest off-diagonal
//  element (p, q), build the rotation that zeros it, apply the rotation
//  to A from both sides and accumulate it into the eigenvector matrix V.
//  After enough sweeps the matrix is diagonal and the eigenvalues sit on
//  the diagonal, the eigenvectors are the columns of V.
//
//  Symmetric matrices guarantee real eigenvalues + orthogonal V. For a
//  12x12 matrix typical convergence is ~10-15 sweeps to 1e-7 -- we cap
//  at 64 for safety.
//
//  The function is templated by N so we can run unit tests on 3x3 and
//  13x13 identity in the verification harness without duplicating code.
//
//  Returns the number of sweeps actually performed (for diagnostics).
// ---------------------------------------------------------------------------

template <int N>
int jacobiEigen(float* A, float* V, float* eigsOut)
{
    // V := identity
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            V[i * N + j] = (i == j) ? 1.0f : 0.0f;

    int sweeps = 0;
    while (sweeps < kJacobiMaxSweeps) {
        // Off-diagonal Frobenius norm. Once this drops below kJacobiEps
        // (relative to the diagonal scale, but with a small absolute
        // floor for the identity case) we declare convergence.
        float offSum = 0.0f;
        float diagAbs = 0.0f;
        for (int i = 0; i < N; ++i) {
            diagAbs += std::abs(A[i * N + i]);
            for (int j = 0; j < N; ++j) {
                if (i == j) continue;
                offSum += A[i * N + j] * A[i * N + j];
            }
        }
        offSum = std::sqrt(offSum);
        const float scale = std::max(diagAbs, 1.0f);
        if (offSum < kJacobiEps * scale) break;

        // One Jacobi sweep: rotate every (p, q) pair once in a fixed
        // order. This is the "cyclic" variant, simpler than the "max
        // off-diagonal" one but with the same asymptotic convergence
        // and friendlier branch behaviour.
        for (int p = 0; p < N - 1; ++p) {
            for (int q = p + 1; q < N; ++q) {
                const float apq = A[p * N + q];
                if (std::abs(apq) < 1e-12f) continue;

                const float app = A[p * N + p];
                const float aqq = A[q * N + q];

                // theta = cot(2*phi), tan(phi) = sign(theta) / (|theta| + sqrt(theta^2 + 1)).
                // The numerically-stable form avoids tan-overflow when
                // app == aqq (rotation angle = pi/4 exactly).
                float t;
                if (std::abs(app - aqq) < 1e-12f) {
                    t = (apq > 0.0f ? 1.0f : -1.0f);
                } else {
                    const float theta = (aqq - app) / (2.0f * apq);
                    const float sgn = (theta >= 0.0f) ? 1.0f : -1.0f;
                    t = sgn / (std::abs(theta) + std::sqrt(theta * theta + 1.0f));
                }
                const float c = 1.0f / std::sqrt(t * t + 1.0f);
                const float s = t * c;

                // Apply rotation to A. Row/col p and q change; we exploit
                // symmetry to update only the upper triangle then mirror.
                A[p * N + p] = app - t * apq;
                A[q * N + q] = aqq + t * apq;
                A[p * N + q] = 0.0f;
                A[q * N + p] = 0.0f;

                for (int k = 0; k < N; ++k) {
                    if (k == p || k == q) continue;
                    const float akp = A[k * N + p];
                    const float akq = A[k * N + q];
                    A[k * N + p] = c * akp - s * akq;
                    A[p * N + k] = A[k * N + p];
                    A[k * N + q] = s * akp + c * akq;
                    A[q * N + k] = A[k * N + q];
                }

                // Accumulate the rotation into V (eigenvectors as
                // columns). After convergence V[:, j] is the
                // eigenvector for eigenvalue A[j, j].
                for (int k = 0; k < N; ++k) {
                    const float vkp = V[k * N + p];
                    const float vkq = V[k * N + q];
                    V[k * N + p] = c * vkp - s * vkq;
                    V[k * N + q] = s * vkp + c * vkq;
                }
            }
        }
        ++sweeps;
    }

    if (eigsOut) {
        for (int i = 0; i < N; ++i) eigsOut[i] = A[i * N + i];
    }
    return sweeps;
}

// Explicit instantiations. The verification harness (mfccverify_cli)
// calls jacobiEigen<3>() and jacobiEigen<13>() to unit-test against
// known matrices; without these the harness would fail to link.
// jacobiEigen<12>() is the one used at runtime for the actual PCA.
template int jacobiEigen<3>(float*, float*, float*);
template int jacobiEigen<12>(float*, float*, float*);
template int jacobiEigen<13>(float*, float*, float*);


// ---------------------------------------------------------------------------
//  Renderer (lives on the render thread)
// ---------------------------------------------------------------------------

class MfccTrajectoryImpl : public QQuickRhiItemRenderer {
public:
    explicit MfccTrajectoryImpl(MfccTrajectory* item) : m_item(item) {}

    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    MfccTrajectory* m_item;
    QRhi*           m_rhi = nullptr;
    QSize           m_pixelSize;

    // GPU resources. All allocated once on first initialize() and
    // reused thereafter. Vertex buffer is Dynamic because we restream
    // the segment endpoints every frame; index + UBO are also Dynamic
    // (index stays constant, but we upload once and don't bother with
    // an Immutable buffer since QRhi doesn't allow lazy late-binding
    // of resource batches outside a pass).
    QRhiBuffer*                 m_vb         = nullptr;
    QRhiBuffer*                 m_ib         = nullptr;
    QRhiBuffer*                 m_ubo        = nullptr;
    QRhiShaderResourceBindings* m_srb        = nullptr;
    QRhiGraphicsPipeline*       m_pipe       = nullptr;
    QRhiRenderPassDescriptor*   m_rpDesc     = nullptr;

    bool m_indicesUploaded = false;

    // GUI -> render thread synchronized snapshot.
    std::array<float, kTrajectoryLen * 4> m_xyzAge {};
    int    m_filled = 0;
    float  m_lineWidth = 2.5f;
    float  m_orbitHz   = 1.0f / 60.0f;
    float  m_headRgba[4] = { 0, 0.878f, 1.0f, 1.0f };
    float  m_tailRgba[4] = { 0.358f, 0.118f, 0.588f, 1.0f };

    // Wall-clock start for the camera orbit. Set on first render() call.
    std::chrono::steady_clock::time_point m_renderStart;
    bool m_haveStart = false;

    // Per-frame vertex assembly scratch on the render thread.
    std::vector<float> m_vbScratch;
};


// ---------------------------------------------------------------------------
//  MfccTrajectoryImpl::initialize
// ---------------------------------------------------------------------------

void MfccTrajectoryImpl::initialize(QRhiCommandBuffer* /*cb*/)
{
    m_rhi = rhi();
    if (!m_rhi) return;

    if (!m_vb) {
        m_vb = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                QRhiBuffer::VertexBuffer,
                                kSegmentVertexBytes);
        m_vb->create();
    }
    if (!m_ib) {
        m_ib = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                QRhiBuffer::IndexBuffer,
                                kSegmentIndexBytes);
        m_ib->create();
    }
    if (!m_ubo) {
        m_ubo = m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                 QRhiBuffer::UniformBuffer,
                                 sizeof(TrajUbo));
        m_ubo->create();
    }
}


// ---------------------------------------------------------------------------
//  MfccTrajectoryImpl::synchronize
// ---------------------------------------------------------------------------

void MfccTrajectoryImpl::synchronize(QQuickRhiItem* item)
{
    auto* mt = static_cast<MfccTrajectory*>(item);

    m_lineWidth = mt->lineWidth();
    m_orbitHz   = mt->cameraOrbitHz();

    const QColor hc = mt->headColor();
    m_headRgba[0] = float(hc.redF());
    m_headRgba[1] = float(hc.greenF());
    m_headRgba[2] = float(hc.blueF());
    m_headRgba[3] = 1.0f;
    const QColor tc = mt->tailColor();
    m_tailRgba[0] = float(tc.redF());
    m_tailRgba[1] = float(tc.greenF());
    m_tailRgba[2] = float(tc.blueF());
    m_tailRgba[3] = 1.0f;

    if (mt->m_stagedDirty.exchange(false, std::memory_order_acquire)) {
        QMutexLocker lk(&mt->m_stageMutex);
        std::memcpy(m_xyzAge.data(), mt->m_stagedXyzAge.data(),
                    sizeof(float) * m_xyzAge.size());
        m_filled = mt->m_stagedFilled;
    }
}


// ---------------------------------------------------------------------------
//  MfccTrajectoryImpl::render
// ---------------------------------------------------------------------------

void MfccTrajectoryImpl::render(QRhiCommandBuffer* cb)
{
    if (!m_rhi) return;
    QRhiTexture* color = colorTexture();
    if (!color) return;
    const QSize size = color->pixelSize();
    if (size.width() <= 0 || size.height() <= 0) return;

    // Build pipeline lazily once the target descriptor is live.
    if (!m_pipe) {
        m_rpDesc = renderTarget()->renderPassDescriptor();

        QShader vs = loadShader(QStringLiteral(":/shaders/mfcc_trajectory.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/mfcc_trajectory.frag.qsb"));

        QRhiVertexInputLayout layout;
        layout.setBindings({
            QRhiVertexInputBinding(kFloatsPerVertex * sizeof(float))
        });
        layout.setAttributes({
            // a_p0 (vec3), a_p1 (vec3), a_ageA (float), a_ageB (float),
            // a_corner (vec2)
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
            QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)),
            QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float,  6 * sizeof(float)),
            QRhiVertexInputAttribute(0, 3, QRhiVertexInputAttribute::Float,  7 * sizeof(float)),
            QRhiVertexInputAttribute(0, 4, QRhiVertexInputAttribute::Float2, 8 * sizeof(float))
        });

        m_srb = m_rhi->newShaderResourceBindings();
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
                m_ubo)
        });
        m_srb->create();

        m_pipe = m_rhi->newGraphicsPipeline();
        m_pipe->setTopology(QRhiGraphicsPipeline::Triangles);
        m_pipe->setShaderStages({
            { QRhiShaderStage::Vertex,   vs },
            { QRhiShaderStage::Fragment, fs }
        });
        m_pipe->setVertexInputLayout(layout);
        m_pipe->setShaderResourceBindings(m_srb);
        m_pipe->setRenderPassDescriptor(m_rpDesc);
        // Standard alpha blending. We emit premultiplied-alpha color so
        // SRC * 1 + DST * (1-SRC.a) is straight alpha-over-black.
        QRhiGraphicsPipeline::TargetBlend tb;
        tb.enable    = true;
        tb.srcColor  = QRhiGraphicsPipeline::One;
        tb.dstColor  = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        tb.opColor   = QRhiGraphicsPipeline::Add;
        tb.srcAlpha  = QRhiGraphicsPipeline::One;
        tb.dstAlpha  = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        tb.opAlpha   = QRhiGraphicsPipeline::Add;
        m_pipe->setTargetBlends({ tb });
        m_pipe->create();
    }

    auto* upload = m_rhi->nextResourceUpdateBatch();

    if (!m_indicesUploaded) {
        // 6 indices per segment, two CCW triangles: (0,1,2) + (2,1,3).
        std::vector<uint16_t> idx(size_t(kSegmentCount * kIndicesPerSeg));
        for (int s = 0; s < kSegmentCount; ++s) {
            const uint16_t base = uint16_t(s * kVerticesPerSeg);
            uint16_t* o = idx.data() + s * kIndicesPerSeg;
            o[0] = base + 0; o[1] = base + 1; o[2] = base + 2;
            o[3] = base + 2; o[4] = base + 1; o[5] = base + 3;
        }
        upload->updateDynamicBuffer(m_ib, 0, kSegmentIndexBytes, idx.data());
        m_indicesUploaded = true;
    }

    // ---- Build the per-segment vertex buffer ---------------------------
    // For each segment i in [0, m_filled-1):
    //   p0 = m_xyzAge[i],  p1 = m_xyzAge[i+1]
    //   ageA = age(i),     ageB = age(i+1)   in [0, 1]
    // For segments beyond m_filled we emit a degenerate quad at the
    // origin with zero alpha so it draws nothing.

    static const float corners[4][2] = {
        { -1.f, -1.f }, {  1.f, -1.f },
        { -1.f,  1.f }, {  1.f,  1.f }
    };

    const size_t vbFloats =
        size_t(kSegmentCount) * kVerticesPerSeg * kFloatsPerVertex;
    if (m_vbScratch.size() < vbFloats) m_vbScratch.resize(vbFloats);

    const int filledSegs = std::max(0, m_filled - 1);
    const float invMaxAge = (filledSegs > 0) ? 1.0f / float(filledSegs)
                                              : 1.0f;

    for (int s = 0; s < kSegmentCount; ++s) {
        float p0x = 0.0f, p0y = 0.0f, p0z = 0.0f;
        float p1x = 0.0f, p1y = 0.0f, p1z = 0.0f;
        float ageA = -1.0f, ageB = -1.0f;        // negative = invisible

        if (s < filledSegs) {
            const int i0 = s;
            const int i1 = s + 1;
            p0x = m_xyzAge[i0 * 4 + 0];
            p0y = m_xyzAge[i0 * 4 + 1];
            p0z = m_xyzAge[i0 * 4 + 2];
            p1x = m_xyzAge[i1 * 4 + 0];
            p1y = m_xyzAge[i1 * 4 + 1];
            p1z = m_xyzAge[i1 * 4 + 2];
            // age in [0, 1]: 0 = oldest segment, 1 = newest. The
            // fragment shader interpolates ageA -> ageB across the line
            // and uses it for both color and alpha.
            ageA = float(s) * invMaxAge;
            ageB = float(s + 1) * invMaxAge;
        }

        float* o = m_vbScratch.data() + s * kVerticesPerSeg * kFloatsPerVertex;
        for (int c = 0; c < kVerticesPerSeg; ++c) {
            o[0] = p0x; o[1] = p0y; o[2] = p0z;
            o[3] = p1x; o[4] = p1y; o[5] = p1z;
            o[6] = ageA;
            o[7] = ageB;
            o[8] = corners[c][0];
            o[9] = corners[c][1];
            o += kFloatsPerVertex;
        }
    }
    upload->updateDynamicBuffer(m_vb, 0, kSegmentVertexBytes,
                                m_vbScratch.data());

    // ---- Camera matrix -------------------------------------------------
    if (!m_haveStart) {
        m_renderStart = std::chrono::steady_clock::now();
        m_haveStart = true;
    }
    using namespace std::chrono;
    const float t = duration_cast<duration<float>>(
                        steady_clock::now() - m_renderStart).count();
    const float theta = t * m_orbitHz * 2.0f * float(M_PI);
    constexpr float kPi_f = 3.14159265358979323846f;

    const std::array<float, 3> eye = {
        2.8f * std::cos(theta),
        1.2f,
        2.8f * std::sin(theta)
    };
    const std::array<float, 3> center = { 0.0f, 0.0f, 0.0f };
    const std::array<float, 3> up     = { 0.0f, 1.0f, 0.0f };
    const auto view = mat4LookAt(eye, center, up);
    const float aspect = float(size.width()) / float(size.height());
    const auto proj = mat4Perspective(45.0f * kPi_f / 180.0f, aspect,
                                      0.1f, 100.0f);
    const auto viewProj = mat4Mul(proj, view);

    // ---- UBO upload ----------------------------------------------------
    {
        TrajUbo u{};
        std::memcpy(u.viewProj, viewProj.data(), sizeof(u.viewProj));
        u.viewportPx[0] = float(size.width());
        u.viewportPx[1] = float(size.height());
        u.lineWidth     = m_lineWidth;
        u.maxAge        = 1.0f;
        std::memcpy(u.headColor, m_headRgba, sizeof(u.headColor));
        std::memcpy(u.tailColor, m_tailRgba, sizeof(u.tailColor));
        u.fogStart      = 0.1f;
        u.fogEnd        = 6.5f;
        u.lineWidth     = m_lineWidth;
        upload->updateDynamicBuffer(m_ubo, 0, sizeof(u), &u);
    }

    // ---- Single pass: draw segment quads with alpha-over-black --------
    cb->beginPass(renderTarget(), QColor(0, 0, 0, 0), { 1.0f, 0 }, upload);
    const QRhiViewport vp(0, 0, float(size.width()), float(size.height()));
    cb->setViewport(vp);
    cb->setGraphicsPipeline(m_pipe);
    cb->setShaderResources(m_srb);
    {
        QRhiCommandBuffer::VertexInput vin(m_vb, 0);
        cb->setVertexInput(0, 1, &vin,
                           m_ib, 0, QRhiCommandBuffer::IndexUInt16);
        cb->drawIndexed(kSegmentCount * kIndicesPerSeg);
    }
    cb->endPass();

    // Camera animates -- request another frame even when no new MFCC
    // arrives, so the orbit and fade keep moving.
    update();
}


// ---------------------------------------------------------------------------
//  MfccTrajectory (GUI thread)
// ---------------------------------------------------------------------------

MfccTrajectory::MfccTrajectory(QQuickItem* parent)
    : QQuickRhiItem(parent)
    , m_analyzer(new MfccAnalyzer(this))
{
    setAlphaBlending(true);
    setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA8);

    connect(m_analyzer, &MfccAnalyzer::mfccUpdated,
            this, &MfccTrajectory::onMfccUpdated,
            Qt::DirectConnection);
}

MfccTrajectory::~MfccTrajectory() = default;

QQuickRhiItemRenderer* MfccTrajectory::createRenderer()
{
    return new MfccTrajectoryImpl(this);
}


// --- Property setters / source binding -------------------------------------

void MfccTrajectory::setAudioSource(AudioFeatures* s)
{
    if (m_source == s) return;
    m_source = s;
    m_analyzer->setAudioSource(s);
    emit audioSourceChanged();
}

void MfccTrajectory::setHeadColor(const QColor& c)
{
    if (m_headColor == c) return;
    m_headColor = c;
    emit headColorChanged();
    update();
}

void MfccTrajectory::setTailColor(const QColor& c)
{
    if (m_tailColor == c) return;
    m_tailColor = c;
    emit tailColorChanged();
    update();
}

void MfccTrajectory::setLineWidth(float v)
{
    v = std::clamp(v, 0.5f, 64.0f);
    if (m_lineWidth == v) return;
    m_lineWidth = v;
    emit lineWidthChanged();
    update();
}

void MfccTrajectory::setCameraOrbitHz(float v)
{
    v = std::clamp(v, 0.0f, 4.0f);
    if (m_cameraOrbitHz == v) return;
    m_cameraOrbitHz = v;
    emit cameraOrbitHzChanged();
    update();
}

void MfccTrajectory::resetPCA()
{
    m_havePca       = false;
    m_hopsSeen      = 0;
    m_hopsSinceRecompute = 0;
    m_trajWrite     = 0;
    m_filledRows.store(0, std::memory_order_release);
    std::fill(m_trajectory.begin(), m_trajectory.end(), 0.0f);
    std::fill(m_mean.begin(),       m_mean.end(),       0.0f);
    std::fill(m_pca.begin(),        m_pca.end(),        0.0f);
    std::fill(m_eigs.begin(),       m_eigs.end(),       0.0f);
    stageTrajectory();
}


// --- Hop handler -----------------------------------------------------------

void MfccTrajectory::onMfccUpdated()
{
    ++m_hopsSeen;
    ++m_hopsSinceRecompute;

    // Defer the first PCA until we have at least a half-window of data;
    // with under ~60 vectors the covariance is too noisy to deliver a
    // stable basis. Until then we project into the zero basis (all
    // points at origin).
    const int kMinSamplesForFirstPca = 60;
    if (!m_havePca && m_hopsSeen >= kMinSamplesForFirstPca) {
        recomputePCA();
        m_havePca = true;
        m_hopsSinceRecompute = 0;
    } else if (m_havePca && m_hopsSinceRecompute >= PCA_RECOMPUTE_EVERY) {
        recomputePCA();
        m_hopsSinceRecompute = 0;
    }

    // Pull the latest 13-D coefficient vector. We use coefficients
    // 1..12 (drop index 0 -- gross loudness).
    std::array<float, MfccAnalyzer::N_COEFFS> raw {};
    if (!m_analyzer->fillLatestMfcc(raw.data(), int(raw.size()))) return;

    std::array<float, FEATURE_DIM> feat {};
    for (int i = 0; i < FEATURE_DIM; ++i) feat[i] = raw[i + 1];

    // Project. If we haven't done a first PCA yet the basis is zero, so
    // all points land at the origin -- the trajectory will appear as a
    // single dot for the first ~1 s. Once the PCA fires, the points
    // begin spreading out organically.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (m_havePca) {
        for (int i = 0; i < FEATURE_DIM; ++i) {
            const float v = feat[i] - m_mean[i];
            x += v * m_pca[i * 3 + 0];
            y += v * m_pca[i * 3 + 1];
            z += v * m_pca[i * 3 + 2];
        }
        // Per-eigenvalue scaling: the three axes have raw variances
        // m_eigs[0..2]. Without renormalisation the first axis swamps
        // the other two (typical eigenvalue ratio is 3-10x), making the
        // 3D structure look like a 1D ribbon. Scale by 1/sqrt(eig) so
        // each axis carries unit variance, then divide by an absolute
        // factor so the whole point cloud lives comfortably in [-1, 1].
        constexpr float kScale = 0.6f;
        auto axisScale = [](float eig) {
            return (eig > 1e-6f) ? 1.0f / std::sqrt(eig) : 0.0f;
        };
        x *= kScale * axisScale(m_eigs[0]);
        y *= kScale * axisScale(m_eigs[1]);
        z *= kScale * axisScale(m_eigs[2]);
    }

    // Append.
    m_trajectory[m_trajWrite * 3 + 0] = x;
    m_trajectory[m_trajWrite * 3 + 1] = y;
    m_trajectory[m_trajWrite * 3 + 2] = z;
    m_trajWrite = (m_trajWrite + 1) % TRAJECTORY_LEN;
    const int prev = m_filledRows.load(std::memory_order_relaxed);
    if (prev < TRAJECTORY_LEN)
        m_filledRows.store(prev + 1, std::memory_order_release);

    stageTrajectory();
    emit trajectoryChanged();
}


// --- PCA recompute ---------------------------------------------------------

void MfccTrajectory::recomputePCA(float* sweepsOut)
{
    auto t0 = std::chrono::steady_clock::now();

    // Snapshot the recent MFCC window. fillRecentMfcc returns rows in
    // chronological order (oldest first). The 13-D vectors include
    // coefficient 0; we drop it on the way into the working buffer.
    constexpr int kRawCols = MfccAnalyzer::N_COEFFS;
    std::array<float, MfccAnalyzer::RECENT_ROWS * kRawCols> raw {};
    const int rows = m_analyzer->fillRecentMfcc(raw.data(),
                                                MfccAnalyzer::RECENT_ROWS);
    if (rows < 8) return;   // covariance is meaningless on a tiny window

    // Drop coefficient 0 into m_pcaScratch (rows x FEATURE_DIM, row-major).
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < FEATURE_DIM; ++c) {
            m_pcaScratch[r * FEATURE_DIM + c] = raw[r * kRawCols + c + 1];
        }
    }

    // Mean per feature.
    std::array<float, FEATURE_DIM> newMean {};
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < FEATURE_DIM; ++c) {
            newMean[c] += m_pcaScratch[r * FEATURE_DIM + c];
        }
    }
    const float invRows = 1.0f / float(rows);
    for (int c = 0; c < FEATURE_DIM; ++c) newMean[c] *= invRows;

    // Mean-center.
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < FEATURE_DIM; ++c) {
            m_pcaScratch[r * FEATURE_DIM + c] -= newMean[c];
        }
    }

    // Covariance = (X^T X) / (rows - 1).
    std::fill(m_cov.begin(), m_cov.end(), 0.0f);
    for (int r = 0; r < rows; ++r) {
        const float* row = &m_pcaScratch[r * FEATURE_DIM];
        for (int i = 0; i < FEATURE_DIM; ++i) {
            const float vi = row[i];
            for (int j = i; j < FEATURE_DIM; ++j) {
                m_cov[i * FEATURE_DIM + j] += vi * row[j];
            }
        }
    }
    // Mirror upper -> lower and normalize.
    const float invN = 1.0f / float(std::max(1, rows - 1));
    for (int i = 0; i < FEATURE_DIM; ++i) {
        for (int j = i; j < FEATURE_DIM; ++j) {
            const float v = m_cov[i * FEATURE_DIM + j] * invN;
            m_cov[i * FEATURE_DIM + j] = v;
            m_cov[j * FEATURE_DIM + i] = v;
        }
    }

    // Eigendecompose. After this:
    //   m_cov[i, i] = eigenvalue i
    //   m_vec[:, i] = eigenvector i  (column-major in the FEATURE_DIM x
    //                                 FEATURE_DIM matrix)
    std::array<float, FEATURE_DIM> eigs {};
    const int sweeps = jacobiEigen<FEATURE_DIM>(m_cov.data(),
                                                m_vec.data(),
                                                eigs.data());
    if (sweepsOut) *sweepsOut = float(sweeps);
    m_dbgLastSweeps = sweeps;

    // Sort eigenvalue indices descending (largest first).
    std::array<int, FEATURE_DIM> order {};
    for (int i = 0; i < FEATURE_DIM; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return eigs[a] > eigs[b]; });

    // Top-3 eigenvectors as a (FEATURE_DIM x 3) matrix. The previous
    // basis lives in m_pca with the same shape (eigenvectors in
    // columns). For sign-stability we match each new top-3 vector with
    // the previously stored top-3 vector that has the highest absolute
    // dot product and sign-flip if needed. This matters because a
    // Jacobi rotation can return eigenvectors in either +v or -v form
    // (both are valid eigenvectors); without this fix the visualization
    // would mirror on every PCA recompute.
    std::array<float, FEATURE_DIM * 3> newPca {};
    std::array<float, 3>               newEigs {};
    std::array<bool,  3>               usedPrev = { false, false, false };

    for (int k = 0; k < 3; ++k) {
        const int srcIdx = order[k];
        // Extract the column-vector m_vec[:, srcIdx].
        std::array<float, FEATURE_DIM> v {};
        for (int i = 0; i < FEATURE_DIM; ++i)
            v[i] = m_vec[i * FEATURE_DIM + srcIdx];

        if (m_havePca) {
            // Find the previous column with the highest |dot|. We match
            // greedily so two new vectors don't both pair to the same
            // old one.
            int bestPrev = -1;
            float bestAbs = -1.0f;
            float bestDot =  0.0f;
            for (int p = 0; p < 3; ++p) {
                if (usedPrev[p]) continue;
                float dot = 0.0f;
                for (int i = 0; i < FEATURE_DIM; ++i)
                    dot += v[i] * m_pca[i * 3 + p];
                const float abs = std::abs(dot);
                if (abs > bestAbs) {
                    bestAbs = abs;
                    bestDot = dot;
                    bestPrev = p;
                }
            }
            if (bestPrev >= 0) {
                usedPrev[bestPrev] = true;
                if (bestDot < 0.0f) {
                    for (int i = 0; i < FEATURE_DIM; ++i) v[i] = -v[i];
                    ++m_dbgSignFlips;
                }
                // Place this vector at column bestPrev so axis
                // identities are preserved (axis 0 stays the dominant
                // direction even if it became slightly less dominant
                // than what we'd otherwise pick as axis 0).
                for (int i = 0; i < FEATURE_DIM; ++i)
                    newPca[i * 3 + bestPrev] = v[i];
                newEigs[bestPrev] = eigs[srcIdx];
                continue;
            }
        }
        // First PCA, or no remaining slot: place this vector at column k.
        for (int i = 0; i < FEATURE_DIM; ++i)
            newPca[i * 3 + k] = v[i];
        newEigs[k] = eigs[srcIdx];
        if (m_havePca) usedPrev[k] = true;
    }

    m_pca  = newPca;
    m_eigs = newEigs;
    m_mean = newMean;

    auto t1 = std::chrono::steady_clock::now();
    m_dbgLastUsec = std::chrono::duration_cast<std::chrono::microseconds>
                        (t1 - t0).count();
    ++m_dbgRecomputes;
}


// --- Stage to render thread -------------------------------------------------

void MfccTrajectory::stageTrajectory()
{
    const int filled = m_filledRows.load(std::memory_order_acquire);
    // Read the trajectory in chronological order (oldest first).
    const int start = (m_trajWrite + TRAJECTORY_LEN - filled) % TRAJECTORY_LEN;

    {
        QMutexLocker lk(&m_stageMutex);
        // age = i / (filled - 1) computed on the render side from
        // (segment index / filled). We just stage the (x, y, z, _) so
        // we can keep the GPU layout identical to the eventual
        // age-bearing format if we ever decide to bake age in here.
        for (int i = 0; i < filled; ++i) {
            const int src = (start + i) % TRAJECTORY_LEN;
            m_stagedXyzAge[i * 4 + 0] = m_trajectory[src * 3 + 0];
            m_stagedXyzAge[i * 4 + 1] = m_trajectory[src * 3 + 1];
            m_stagedXyzAge[i * 4 + 2] = m_trajectory[src * 3 + 2];
            m_stagedXyzAge[i * 4 + 3] = 0.0f;
        }
        // Zero the tail so old data isn't aliased into the visible quads.
        for (int i = filled; i < TRAJECTORY_LEN; ++i) {
            m_stagedXyzAge[i * 4 + 0] = 0.0f;
            m_stagedXyzAge[i * 4 + 1] = 0.0f;
            m_stagedXyzAge[i * 4 + 2] = 0.0f;
            m_stagedXyzAge[i * 4 + 3] = 0.0f;
        }
        m_stagedFilled = filled;
    }
    m_stagedDirty.store(true, std::memory_order_release);
    update();
}


// --- Debug snapshot (verification harness) ---------------------------------

int MfccTrajectory::debugSnapshot(float* outXYZ, int maxRows, float* outEigs)
{
    if (outEigs) {
        outEigs[0] = m_eigs[0];
        outEigs[1] = m_eigs[1];
        outEigs[2] = m_eigs[2];
    }
    if (!outXYZ || maxRows <= 0) return 0;
    const int filled = m_filledRows.load(std::memory_order_acquire);
    const int n = std::min(filled, maxRows);
    const int start = (m_trajWrite + TRAJECTORY_LEN - filled) % TRAJECTORY_LEN;
    for (int i = 0; i < n; ++i) {
        const int src = (start + i) % TRAJECTORY_LEN;
        outXYZ[i * 3 + 0] = m_trajectory[src * 3 + 0];
        outXYZ[i * 3 + 1] = m_trajectory[src * 3 + 1];
        outXYZ[i * 3 + 2] = m_trajectory[src * 3 + 2];
    }
    return n;
}
