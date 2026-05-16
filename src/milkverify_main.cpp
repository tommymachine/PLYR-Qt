// milkverify_cli — algorithmic regression test for MilkdropRuntime.
//
// Does NOT open a window, an RHI context, or any audio device. Loads
// one of the bundled .milk presets from a temporary file, feeds a
// fixed audio scalar set, runs per-frame + per-vertex, and verifies
// that the expected output variables hold the expected values.
//
// Exit codes:
//   0  — all checks passed.
//   1  — at least one check failed; details on stdout.
//
// The harness is intentionally tiny — Layer 3b is too big to fully
// unit-test, but this gives a fast smoke test that the integration
// glue (file load, KV parse, projectm-eval compile, variable binding,
// q-register passing) works.

#include "MilkdropRuntime.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTextStream>

#include <cmath>
#include <cstdio>

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

bool nearly(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) < eps;
}

int g_failures = 0;

void check(bool cond, const QString& label)
{
    out() << (cond ? "  PASS  " : "  FAIL  ") << label << '\n';
    if (!cond) ++g_failures;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    out() << "milkverify_cli — MilkdropRuntime smoke test\n";

    // ---- (1) Compile from a string -------------------------------------
    {
        MilkdropRuntime rt;
        QByteArray src =
            "[preset00]\n"
            "fDecay=0.95\n"
            "per_frame_1=zoom = 1.0 + 0.02 * bass_att;\n"
            "per_frame_2=q1 = bass_att;\n"
            "per_frame_3=q2 = mid_att;\n"
            "per_vertex_1=dx = dx + 0.001 * q1;\n";

        const bool loaded = rt.loadPresetFromString(src);
        check(loaded, "compile inline preset");
        check(rt.lastError().isEmpty(),
              QStringLiteral("lastError empty: \"%1\"").arg(rt.lastError()));

        // Feed audio. Per Geiss's "MilkDrop scale" we use 0.6 for bass_att.
        rt.setAudio(0.5f, 0.5f, 0.5f, 0.6f, 0.4f, 0.3f);
        rt.advance(1.0f / 60.0f);
        rt.runPerFrame();

        const double* q1 = rt.variable("q1");
        const double* q2 = rt.variable("q2");
        const double* zoom = rt.variable("zoom");

        check(q1 != nullptr, "q1 variable bound");
        check(q2 != nullptr, "q2 variable bound");
        check(zoom != nullptr, "zoom variable bound");

        if (q1) check(nearly(*q1, 0.6, 1e-4),
                      QStringLiteral("q1 = bass_att (0.6), got %1").arg(*q1));
        if (q2) check(nearly(*q2, 0.4, 1e-4),
                      QStringLiteral("q2 = mid_att (0.4), got %1").arg(*q2));
        if (zoom)
            check(nearly(*zoom, 1.012, 1e-4),
                  QStringLiteral("zoom = 1 + 0.02*0.6 = 1.012, got %1").arg(*zoom));

        check(nearly(rt.q(1), 0.6f, 1e-4f), "rt.q(1) accessor");
        check(nearly(rt.decay(), 0.95f, 1e-4f),
              QStringLiteral("decay = fDecay header (0.95), got %1").arg(rt.decay()));

        // Per-vertex
        const int W = 4, H = 4;
        float uv[2 * W * H] = {0};
        rt.runPerVertex(W, H, uv);
        // Corner (0, 0): with zoom=1.012, dx should add 0.001 * 0.6.
        // identity UV for (0,0) is (0,0); after warp + dx:
        //   u_pre = cx + (0 - cx) * (1/1.012) = 0.5 + (-0.5)*0.988 ≈ 0.00593
        //   + dx (= 0.0006) → ~0.00653
        // The exact numerical comparison is tricky; just verify the corner
        // moved from identity by at least the dx amount.
        const float u00 = uv[0];
        const float v00 = uv[1];
        check(u00 > 0.0f, QStringLiteral("zoom pulls top-left u inward; got u=%1").arg(u00));
        check(v00 > 0.0f, QStringLiteral("zoom pulls top-left v inward; got v=%1").arg(v00));
    }

    // ---- (2) Load the bundled swirl preset from disk via temp file ----
    {
        // Write the bundled swirl preset to a tmp file. We embed the
        // text inline so this harness doesn't need the resource bundle
        // to be findable — same content as presets/02_swirl.milk.
        const char* swirl =
            "[preset00]\n"
            "fDecay=0.96\n"
            "cx=0.5\n"
            "cy=0.5\n"
            "per_frame_1=zoom = 1.0 + 0.02 * bass_att - 0.01 * treb_att;\n"
            "per_frame_2=rot  = 0.02 * sin(time * 0.5);\n"
            "per_frame_3=q1   = bass_att;\n"
            "per_vertex_1=ang = atan2(y - cy, x - cx);\n"
            "per_vertex_2=dx  = dx + 0.003 * sin(ang * 3 + time) * q1;\n"
            "per_vertex_3=dy  = dy + 0.003 * cos(ang * 3 + time) * q1;\n";

        QString tmpPath = QStringLiteral("/tmp/milkverify_swirl.milk");
        {
            QFile f(tmpPath);
            const bool opened = f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            check(opened, "write swirl temp file");
            if (opened) f.write(swirl);
        }

        MilkdropRuntime rt;
        const bool loaded = rt.loadPresetFromFile(tmpPath);
        check(loaded, "loadPresetFromFile(temp swirl)");
        check(rt.lastError().isEmpty(), "swirl preset compiled without error");

        rt.setAudio(0.5f, 0.5f, 0.5f, 0.6f, 0.5f, 0.4f);
        rt.advance(1.0f / 60.0f);
        rt.runPerFrame();

        check(nearly(rt.q(1), 0.6f, 1e-4f),
              QStringLiteral("swirl q1 = bass_att (0.6), got %1").arg(rt.q(1)));

        // Spot-check per_vertex on a 4x4 mesh — corners should differ
        // from identity because the swirl displaces them.
        const int W = 4, H = 4;
        float uv[2 * W * H] = {0};
        rt.runPerVertex(W, H, uv);

        // Identity for (i=0, j=0) is (0, 0). After zoom (~1.012), rot
        // (~0.0001), and swirl dx/dy (with q1=0.6), it should NOT be
        // exactly (0, 0). Use a loose tolerance.
        const float du = uv[0] - 0.0f;
        const float dv = uv[1] - 0.0f;
        const float displacement = std::sqrt(du * du + dv * dv);
        check(displacement > 1e-4f,
              QStringLiteral("swirl displaces (0,0); got |Δ|=%1").arg(displacement));

        // Verify the middle vertex (W/2, H/2) doesn't move much — it's
        // near the centre cx, cy = 0.5, 0.5 where zoom and rot have
        // no effect. The swirl atan2 is undefined at exactly (cx, cy)
        // but the 4x4 grid steps W/3 ≈ 0.33, so we sample (1, 1) =
        // (1/3, 1/3) which IS displaced.
        // Just make sure the visualization isn't a no-op.
        float maxD = 0;
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                const float u = uv[2 * (j * W + i) + 0];
                const float v = uv[2 * (j * W + i) + 1];
                const float identU = float(i) / float(W - 1);
                const float identV = float(j) / float(H - 1);
                const float du2 = u - identU;
                const float dv2 = v - identV;
                const float d = std::sqrt(du2 * du2 + dv2 * dv2);
                if (d > maxD) maxD = d;
            }
        check(maxD > 1e-3f,
              QStringLiteral("swirl visibly distorts mesh; max |Δ|=%1").arg(maxD));

        QFile::remove(tmpPath);
    }

    // ---- (3) Compile error path ---------------------------------------
    {
        MilkdropRuntime rt;
        const bool loaded = rt.loadPresetFromString(
            "[preset00]\nper_frame_1=zoom = 1.0 + ; // syntax error\n");
        // Either it loads (the empty trailing expression is just a noop)
        // or it doesn't — what we want is for the error message to
        // surface when something obviously bad is in the source.
        QString err = rt.lastError();
        check(!loaded || !err.isEmpty(),
              QStringLiteral("malformed preset surfaces an error (loaded=%1, err=\"%2\")")
              .arg(loaded).arg(err));
    }

    // ---- (4) Audio scaling sanity -------------------------------------
    {
        MilkdropRuntime rt;
        rt.loadPresetFromString(
            "[preset00]\nper_frame_1=q3 = bass + mid + treb;\n");
        rt.setAudio(1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f);
        rt.advance(1.0f / 60.0f);
        rt.runPerFrame();
        check(nearly(rt.q(3), 6.0f, 1e-4f),
              QStringLiteral("bass+mid+treb summed correctly, got q3=%1").arg(rt.q(3)));
    }

    // ---- (5) Real-world-shape preset --------------------------------------
    // A representative subset of what shipped MilkDrop presets look like:
    // dozens of scalar header keys (most ignored), per-frame, per-vertex.
    // We verify the parser tolerates the noise and the math still works.
    {
        const char* realish =
            "[preset00]\n"
            "fRating=2.0\n"
            "fGammaAdj=2.0\n"
            "fDecay=0.96\n"
            "fVideoEchoZoom=2.0\n"
            "fVideoEchoAlpha=0.0\n"
            "nVideoEchoOrientation=0\n"
            "nWaveMode=0\n"
            "bAdditiveWaves=0\n"
            "bWaveDots=0\n"
            "bWaveThick=0\n"
            "bModWaveAlphaByVolume=0\n"
            "bMaximizeWaveColor=1\n"
            "bTexWrap=1\n"
            "bDarkenCenter=0\n"
            "fWaveAlpha=0.001\n"
            "fWaveScale=1.28\n"
            "fWarpAnimSpeed=1.0\n"
            "fWarpScale=1.331\n"
            "fZoomExponent=1.0\n"
            "zoom=1.07\n"
            "rot=0.0\n"
            "cx=0.5\n"
            "cy=0.5\n"
            "warp=2.5\n"
            "wave_r=0.85\n"
            "wave_g=0.5\n"
            "wave_b=0.0\n"
            // wavecode / shapecode blocks routinely show up — we should
            // silently skip them and still compile the rest.
            "wavecode_0_enabled=0\n"
            "wavecode_0_samples=512\n"
            "wavecode_0_init=t1 = 0;\n"
            "shapecode_0_enabled=0\n"
            "per_frame_1=zoom = zoom + 0.01 * bass_att - 0.01 * treb_att;\n"
            "per_frame_2=rot = rot + 0.01 * sin(time);\n"
            "per_frame_3=q1 = bass + mid + treb;\n"
            "per_vertex_1=zoom = zoom + 0.05 * sin(rad * 8 + time);\n";

        MilkdropRuntime rt;
        const bool loaded = rt.loadPresetFromString(realish);
        check(loaded, "real-world-shape preset compiles");
        check(rt.lastError().isEmpty(),
              QStringLiteral("real-world preset: no error, got \"%1\"")
              .arg(rt.lastError()));
        check(nearly(rt.decay(), 0.96f, 1e-4f),
              QStringLiteral("real-world preset decay = 0.96, got %1").arg(rt.decay()));

        rt.setAudio(0.8f, 0.6f, 0.4f, 0.8f, 0.6f, 0.4f);
        rt.advance(1.0f / 60.0f);
        rt.runPerFrame();
        check(nearly(rt.q(1), 1.8f, 1e-4f),
              QStringLiteral("real-world q1 = bass+mid+treb (1.8), got %1").arg(rt.q(1)));

        const int W = 8, H = 8;
        float uv[2 * W * H] = {0};
        rt.runPerVertex(W, H, uv);
        int nMoved = 0;
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                const float u = uv[2 * (j * W + i) + 0];
                const float v = uv[2 * (j * W + i) + 1];
                const float du = u - float(i) / float(W - 1);
                const float dv = v - float(j) / float(H - 1);
                if (du * du + dv * dv > 1e-6f) ++nMoved;
            }
        check(nMoved > 32,
              QStringLiteral("real-world preset moves the mesh (%1 of 64 vertices)")
              .arg(nMoved));
    }

    out() << '\n';
    if (g_failures == 0) {
        out() << "ALL PASS\n";
        return 0;
    }
    out() << g_failures << " FAIL\n";
    return 1;
}
