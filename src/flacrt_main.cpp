// Round-trip integrity check for the FLAC encoder.
//
//   flacrt_cli <input.flac> [<input.flac> ...]
//
// For each file: decode -> re-encode at compression -8 -> re-decode ->
// byte-compare PCM. A pass means the encoder produced output that
// libFLAC's decoder reads back identically — the lossless contract.
// On a CD rip this is sufficient: if the int16 PCM survives the
// round trip, the AccurateRip / CTDB checksums by construction do too.

#include "FlacDecode.h"
#include "FlacEncode.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList args = app.arguments();
    if (args.size() < 2) {
        err << "usage: flacrt_cli <input.flac> [<input.flac> ...]\n";
        return 2;
    }

    const std::string tmpPath = "/tmp/flacrt_out.flac";
    int rc = 0;
    for (int i = 1; i < args.size(); ++i) {
        const QString name = args[i];
        out << name << "\n";
        out.flush();

        const auto orig = flacdecode::decodeFile(name.toStdString());
        if (!orig) {
            err << "  decode failed (not Red Book CD audio?)\n";
            rc = 1;
            continue;
        }

        if (!flacencode::encodeCdAudioToFile(tmpPath, orig->pcm.data(),
                                             orig->frames)) {
            err << "  re-encode failed\n";
            rc = 1;
            continue;
        }

        const auto round = flacdecode::decodeFile(tmpPath);
        if (!round) {
            err << "  decode of re-encoded file failed\n";
            rc = 1;
            continue;
        }

        if (round->frames != orig->frames) {
            err << QStringLiteral("  FAIL: frame count differs (orig %1, round %2)\n")
                       .arg(orig->frames).arg(round->frames);
            rc = 1;
            continue;
        }
        if (std::memcmp(orig->pcm.data(), round->pcm.data(),
                        orig->pcm.size() * sizeof(int16_t)) != 0) {
            err << "  FAIL: PCM differs\n";
            rc = 1;
            continue;
        }
        out << QStringLiteral("  PASS  (%1 frames bit-identical)\n")
                   .arg(orig->frames);
        out.flush();
    }

    std::remove(tmpPath.c_str());
    return rc;
}
