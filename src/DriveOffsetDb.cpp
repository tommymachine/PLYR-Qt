#include "DriveOffsetDb.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <string>
#include <string_view>

namespace concerto::cd {

namespace {

// Local overrides — drives whose IOKit-reported identity doesn't appear
// in the bundled AccurateRip table because the AR submitters' OS saw a
// different identity. Currently small; grows as we encounter more.
struct Override {
    std::string_view key;     // canonicalized "VENDOR PRODUCT"
    int              offset;
};
constexpr Override kOverrides[] = {
    // Apple's external USB SuperDrive: IOKit reads the enclosure
    // descriptors and reports APPLE / SUPERDRIVE. Under the hood it's
    // a Matshita / Panasonic UJ-8xx mech, but no AR submitter ever saw
    // that identity from macOS, so the upstream table has nothing for
    // this vendor / product pair. Empirical value from our own AR
    // offset scan against the Ravel Edition disc 2 rip.
    {"APPLE SUPERDRIVE", -6},
};

// IOKit vendor → AR-table vendor. AR's HTML preamble (driveoffsets.htm):
//   "JLMS drives listed as Lite-ON, HL-DT-ST as LG Electronics,
//    Matshita as Panasonic"
// AR submitters used the marketing names; modern IOKit / SCSI INQUIRY
// reports the raw labels. We try the alias if a direct lookup misses.
struct VendorAlias {
    std::string_view raw;     // what IOKit reports
    std::string_view ar;      // what AR's table uses
};
constexpr VendorAlias kVendorAliases[] = {
    {"MATSHITA",  "PANASONIC"},
    {"HL-DT-ST",  "LG ELECTRONICS"},   // modern AR submissions
    {"HL-DT-ST",  "LG"},               // OEM-era AR entries
    {"JLMS",      "LITE-ON"},
};

const QHash<QString, int>& tableInstance() {
    static const QHash<QString, int> table = [] {
        QHash<QString, int> t;
        QFile f(QStringLiteral(":/resources/drive_offsets.json"));
        if (!f.open(QIODevice::ReadOnly)) return t;
        const auto doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isArray()) return t;
        const auto arr = doc.array();
        t.reserve(arr.size());
        for (const auto& v : arr) {
            const auto o = v.toObject();
            const QString name = o.value(QStringLiteral("name")).toString();
            if (name.isEmpty()) continue;
            t.insert(name, o.value(QStringLiteral("offset")).toInt());
        }
        return t;
    }();
    return table;
}

std::string makeKey(const std::string& vendor, const std::string& product) {
    if (vendor.empty()) return product;
    if (product.empty()) return vendor;
    return vendor + " " + product;
}

} // namespace

int driveOffsetTableSize() {
    return tableInstance().size();
}

std::vector<std::string> sampleDriveNames(int n) {
    const auto& tbl = tableInstance();
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(std::max(0, n)));
    // QHash isn't ordered; pick whichever the iterator yields first. Good
    // enough for a "did anything load?" sanity check.
    for (auto it = tbl.begin(); it != tbl.end() && int(out.size()) < n; ++it) {
        out.push_back(it.key().toStdString());
    }
    return out;
}

std::optional<int> lookupDriveOffset(const std::string& vendor,
                                     const std::string& product) {
    const std::string key = makeKey(vendor, product);

    // 1. Local overrides win — small list of drives the upstream table
    //    can't reach (USB enclosures masking the mech, etc.).
    for (const auto& o : kOverrides) {
        if (key == o.key) return o.offset;
    }

    // 2. Bundled AR table — direct canonicalized hit.
    const auto& tbl    = tableInstance();
    const QString qkey = QString::fromStdString(key);
    if (const auto it = tbl.find(qkey); it != tbl.end()) {
        return it.value();
    }

    // 3. AR table with the vendor remapped (HL-DT-ST → LG, etc.).
    for (const auto& a : kVendorAliases) {
        if (vendor.size() == a.raw.size()
            && std::equal(vendor.begin(), vendor.end(), a.raw.begin())) {
            std::string aliased(a.ar);
            if (!product.empty()) { aliased += ' '; aliased += product; }
            if (const auto it = tbl.find(QString::fromStdString(aliased));
                it != tbl.end()) {
                return it.value();
            }
        }
    }

    return std::nullopt;
}

} // namespace concerto::cd
