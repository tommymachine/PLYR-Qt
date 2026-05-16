// Stage 2 placeholder: CD-TEXT lead-in pack reader.
//
// The full implementation per METADATA_PIPELINE_AUTOMATED.md "CD-TEXT
// via MMC" reads `IOCDMedia::readTOC(buffer, format=0x05, ...)` on macOS
// and walks the 18-byte pack stream (TITLE/PERFORMER/COMPOSER/UPC_EAN/
// ISRC, plus the SIZE_INFO charset byte and the CRC-16 over each pack).
// METADATA_PLAN.md §1.3.3-1.3.4 carries the parser sketch.
//
// v1 ships the interface only — `read()` returns std::nullopt. The pipe
// degrades to Stage 3 / Stage 4 cleanly.

#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace concerto::metadata {

struct CdTextMeta {
    QString     albumTitle;
    QString     albumPerformer;
    QString     barcode;          // UPC_EAN pack at track 0
    QStringList trackTitles;      // 1-based; index 0 unused
    QStringList trackPerformers;
    QStringList trackComposers;
    QStringList trackIsrcs;       // ISRC pack at track N (or read via Q-channel)
};

class CdTextReader {
public:
    // `devicePath` is the BSD name (e.g. "disk5") on macOS. Returns
    // nullopt if no CD-TEXT data is on the disc OR the reader is not
    // implemented for this platform.
    static std::optional<CdTextMeta> read(const QString& devicePath);
};

} // namespace concerto::metadata
