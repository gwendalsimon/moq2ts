#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace moq2ts {

// The DVB SI tables that a per-program track can keep beside the program
// (draft "Per-Program"). SDT and BAT share PID 0x0011; EIT present/following
// and EIT schedule share PID 0x0012.
enum class SiTable { Nit, Sdt, Bat, EitPresentFollowing, EitSchedule, TdtTot };

inline QSet<SiTable> allSiTables() {
    return {SiTable::Nit, SiTable::Sdt, SiTable::Bat, SiTable::EitPresentFollowing, SiTable::EitSchedule,
            SiTable::TdtTot};
}

// Reads a comma-separated list of nit, sdt, bat, eit-pf, eit-schedule, tdt-tot,
// or all. An unknown name fails, and the error names it.
inline bool parseSiTables(const QString& list, QSet<SiTable>* out, QString* error) {
    struct Name {
        const char* text;
        SiTable table;
    };
    static const Name names[] = {{"nit", SiTable::Nit},
                                 {"sdt", SiTable::Sdt},
                                 {"bat", SiTable::Bat},
                                 {"eit-pf", SiTable::EitPresentFollowing},
                                 {"eit-schedule", SiTable::EitSchedule},
                                 {"tdt-tot", SiTable::TdtTot}};
    QSet<SiTable> tables;
    for (const QString& item : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = item.trimmed();
        if (name == QLatin1String("all")) {
            tables.unite(allSiTables());
            continue;
        }
        bool known = false;
        for (const Name& entry : names) {
            if (name == QLatin1String(entry.text)) {
                tables.insert(entry.table);
                known = true;
            }
        }
        if (!known) {
            if (error != nullptr) {
                *error = QStringLiteral("Unknown SI table \"%1\"; use nit, sdt, bat, eit-pf, eit-schedule, tdt-tot, or all.")
                             .arg(name);
            }
            return false;
        }
    }
    if (out != nullptr) {
        *out = tables;
    }
    return true;
}

}  // namespace moq2ts
