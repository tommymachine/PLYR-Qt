// Standalone CLI tool to exercise the FlacTags reader.
// Usage: flactest <path-to-file.flac>
//
// Prints the parsed tags + STREAMINFO duration.

#include "FlacTags.h"

#include <QCoreApplication>
#include <QStringList>
#include <QTextStream>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    if (argc < 2) {
        err << "usage: flactest <file.flac>\n";
        return 2;
    }

    const auto path = QString::fromUtf8(argv[1]);
    const auto tags = FlacTags::read(path);
    if (!tags) {
        err << "failed to read: " << path << "\n";
        return 1;
    }

    out << path << "\n";
    out << "  duration: " << (*tags).duration << " s\n";

    // Sort keys for stable output.
    auto keys = (*tags).tags.keys();
    std::sort(keys.begin(), keys.end());
    for (const auto& k : keys) {
        out << "  " << k << " = " << (*tags).tags[k] << "\n";
    }
    return 0;
}
