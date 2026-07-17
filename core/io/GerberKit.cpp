#include "GerberKit.h"

#include <algorithm>
#include <optional>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "io/ExcellonIo.h"
#include "io/GerberIo.h"

namespace viki {

namespace {

// A layer role: name + default color + paint rank (lower = painted first, so
// copper sits at the bottom of the stack and outline/drill land on top).
struct Role {
    QString name;
    uint32_t rgb = 0xAAAAAA;
    int rank = 70;
};

// Default colors, chosen readable on the dark canvas: top copper red, bottom
// copper blue, masks green, silkscreen yellow/white, paste gray, outline
// magenta, drill black (visible over the copper it perforates).
constexpr uint32_t kTopCopperRgb = 0xE53935;
constexpr uint32_t kBotCopperRgb = 0x3D7EFF;
constexpr uint32_t kInnerCopperRgb = 0xCC8033;
constexpr uint32_t kTopMaskRgb = 0x2FBF71;
constexpr uint32_t kBotMaskRgb = 0x1F8A50;
constexpr uint32_t kTopSilkRgb = 0xF2D544;
constexpr uint32_t kBotSilkRgb = 0xF0F0F0;
constexpr uint32_t kTopPasteRgb = 0xB0B7BD;
constexpr uint32_t kBotPasteRgb = 0x848C93;
constexpr uint32_t kTopPadsRgb = 0xFF8A65;
constexpr uint32_t kBotPadsRgb = 0x7FA8FF;
constexpr uint32_t kMechRgb = 0x8FA3B0;
constexpr uint32_t kOutlineRgb = 0xFF00FF;
constexpr uint32_t kKeepoutRgb = 0x9C27B0; // dim purple, distinct from outline
constexpr uint32_t kDrillRgb = 0x000000;
constexpr uint32_t kDrillNpthRgb = 0x3C3C3C;
constexpr uint32_t kOtherRgb = 0xAAAAAA;

// Paint ranks (see Role). Keepout paints FIRST: a real GKO can be a FILLED
// zone (S5M0PCBB: solid rectangle over the antenna area) and must never
// mask the copper above it.
enum : int {
    kRankKeepout = 5,
    kRankBotCopper = 10,
    kRankInnerCopper = 12,
    kRankTopCopper = 20,
    kRankBotPads = 25,
    kRankTopPads = 26,
    kRankBotPaste = 30,
    kRankTopPaste = 31,
    kRankBotMask = 35,
    kRankTopMask = 36,
    kRankBotSilk = 40,
    kRankTopSilk = 41,
    kRankMech = 60,
    kRankOther = 70,
    kRankOutline = 90,
    kRankDrill = 95,
    kRankDrillNpth = 96,
};

Role drillRole(bool plated)
{
    return plated ? Role{QStringLiteral("Drill"), kDrillRgb, kRankDrill}
                  : Role{QStringLiteral("Drill-NPTH"), kDrillNpthRgb, kRankDrillNpth};
}

// Extensions that are never fabrication data (Altium reports and side files).
bool isIgnoredExtension(const QString& ext)
{
    static const QStringList kIgnored = {
        QStringLiteral("DRR"), QStringLiteral("REP"),  QStringLiteral("EXTREP"),
        QStringLiteral("RUL"), QStringLiteral("LDP"),  QStringLiteral("APR"),
        QStringLiteral("APR_LIB"), QStringLiteral("HTML"), QStringLiteral("HTM"),
        QStringLiteral("PDF"), QStringLiteral("CSV"),  QStringLiteral("XML"),
        QStringLiteral("ZIP"), QStringLiteral("BAK"),  QStringLiteral("VKD"),
        QStringLiteral("DXF"), QStringLiteral("DWG"),  QStringLiteral("STEP"),
        QStringLiteral("STP"), QStringLiteral("LOG")};
    return kIgnored.contains(ext);
}

// Content sniff on the first few KB. Extensions lie ("Status Report.Txt" is
// not a drill file); the header never does: Excellon starts an M48 block,
// Gerber carries RS-274X words (%FS/%MO/%ADD/G04 comments/D-codes).
enum class Sniff { Excellon, Gerber, Unknown };

Sniff sniffFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Sniff::Unknown;
    const QByteArray head = f.read(4096);
    // M48 at the start of a line = Excellon header block.
    if (head.startsWith("M48") || head.contains("\nM48") || head.contains("\rM48"))
        return Sniff::Excellon;
    if (head.contains("%FS") || head.contains("%MO") || head.contains("%ADD") ||
        head.contains("G04") || head.contains("D01*") || head.contains("G36*"))
        return Sniff::Gerber;
    return Sniff::Unknown;
}

// Layer names must be single tokens (the command grammar splits on spaces).
QString sanitizeToken(const QString& raw)
{
    QString out;
    for (const QChar c : raw)
        out.append(c.isLetterOrNumber() || c == QLatin1Char('_') ||
                           c == QLatin1Char('-')
                       ? c
                       : QLatin1Char('-'));
    while (out.contains(QLatin1String("--")))
        out.replace(QLatin1String("--"), QLatin1String("-"));
    if (out.startsWith(QLatin1Char('-')))
        out.remove(0, 1);
    if (out.endsWith(QLatin1Char('-')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("Layer") : out;
}

// Role from the file extension. `outlinePriority` >= 0 marks an outline
// CANDIDATE (0 = GKO, 1 = GM1, 2 = GM13): the PLAUSIBLE candidate with the
// lowest priority wins the "Outline" name (see the election below), the
// fallback role applies to the losers.
std::optional<Role> roleFromExtension(const QString& ext, int& outlinePriority)
{
    outlinePriority = -1;
    if (ext == QLatin1String("GTL"))
        return Role{QStringLiteral("Top-Copper"), kTopCopperRgb, kRankTopCopper};
    if (ext == QLatin1String("GBL"))
        return Role{QStringLiteral("Bottom-Copper"), kBotCopperRgb, kRankBotCopper};
    if (ext == QLatin1String("GTS"))
        return Role{QStringLiteral("Top-Mask"), kTopMaskRgb, kRankTopMask};
    if (ext == QLatin1String("GBS"))
        return Role{QStringLiteral("Bottom-Mask"), kBotMaskRgb, kRankBotMask};
    if (ext == QLatin1String("GTO"))
        return Role{QStringLiteral("Top-Silk"), kTopSilkRgb, kRankTopSilk};
    if (ext == QLatin1String("GBO"))
        return Role{QStringLiteral("Bottom-Silk"), kBotSilkRgb, kRankBotSilk};
    if (ext == QLatin1String("GTP"))
        return Role{QStringLiteral("Top-Paste"), kTopPasteRgb, kRankTopPaste};
    if (ext == QLatin1String("GBP"))
        return Role{QStringLiteral("Bottom-Paste"), kBotPasteRgb, kRankBotPaste};
    if (ext == QLatin1String("GPT"))
        return Role{QStringLiteral("Top-Pads"), kTopPadsRgb, kRankTopPads};
    if (ext == QLatin1String("GPB"))
        return Role{QStringLiteral("Bottom-Pads"), kBotPadsRgb, kRankBotPads};
    if (ext == QLatin1String("GKO")) {
        outlinePriority = 0;
        return Role{QStringLiteral("Keepout"), kKeepoutRgb, kRankKeepout};
    }
    if (ext.startsWith(QLatin1String("GM")) && ext.size() > 2) {
        bool numOk = false;
        const int n = ext.mid(2).toInt(&numOk);
        if (numOk) {
            if (n == 1)
                outlinePriority = 1; // Altium's usual home for the contour
            else if (n == 13)
                outlinePriority = 2; // observed fallback (S5M0PCBA)
            return Role{QStringLiteral("Mech-%1").arg(n), kMechRgb, kRankMech};
        }
    }
    // Protel mid-layers .G1 ... .G16 (inner copper).
    if (ext.size() > 1 && ext.startsWith(QLatin1Char('G'))) {
        bool numOk = false;
        const int n = ext.mid(1).toInt(&numOk);
        if (numOk)
            return Role{QStringLiteral("Copper-Mid%1").arg(n), kInnerCopperRgb,
                        kRankInnerCopper};
    }
    return std::nullopt;
}

// Role from the X2 TF.FileFunction attribute (prevails over the extension).
std::optional<Role> roleFromFileFunction(const QString& value, int& outlinePriority)
{
    outlinePriority = -1;
    const QStringList parts = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return std::nullopt;
    const QString f0 = parts.first().trimmed().toLower();
    const bool top = parts.contains(QLatin1String("Top"), Qt::CaseInsensitive);
    const bool bot = parts.contains(QLatin1String("Bot"), Qt::CaseInsensitive) ||
                     parts.contains(QLatin1String("Bottom"), Qt::CaseInsensitive);

    if (f0 == QLatin1String("copper")) {
        if (top)
            return Role{QStringLiteral("Top-Copper"), kTopCopperRgb, kRankTopCopper};
        if (bot)
            return Role{QStringLiteral("Bottom-Copper"), kBotCopperRgb,
                        kRankBotCopper};
        const QString l = parts.size() > 1 ? sanitizeToken(parts[1].trimmed())
                                           : QStringLiteral("Inner");
        return Role{QStringLiteral("Copper-%1").arg(l), kInnerCopperRgb,
                    kRankInnerCopper};
    }
    if (f0 == QLatin1String("soldermask"))
        return bot ? Role{QStringLiteral("Bottom-Mask"), kBotMaskRgb, kRankBotMask}
                   : Role{QStringLiteral("Top-Mask"), kTopMaskRgb, kRankTopMask};
    if (f0 == QLatin1String("legend"))
        return bot ? Role{QStringLiteral("Bottom-Silk"), kBotSilkRgb, kRankBotSilk}
                   : Role{QStringLiteral("Top-Silk"), kTopSilkRgb, kRankTopSilk};
    if (f0 == QLatin1String("paste"))
        return bot ? Role{QStringLiteral("Bottom-Paste"), kBotPasteRgb, kRankBotPaste}
                   : Role{QStringLiteral("Top-Paste"), kTopPasteRgb, kRankTopPaste};
    if (f0 == QLatin1String("profile"))
        return Role{QStringLiteral("Outline"), kOutlineRgb, kRankOutline};
    if (f0 == QLatin1String("keepout") || f0 == QLatin1String("keep-out")) {
        outlinePriority = 0;
        return Role{QStringLiteral("Keepout"), kKeepoutRgb, kRankKeepout};
    }
    if (f0 == QLatin1String("mechanical")) {
        bool numOk = false;
        const int n = parts.size() > 1 ? parts[1].trimmed().toInt(&numOk) : 0;
        return Role{numOk ? QStringLiteral("Mech-%1").arg(n)
                          : QStringLiteral("Mech"),
                    kMechRgb, kRankMech};
    }
    if (f0 == QLatin1String("plated")) // drill data in Gerber format
        return Role{QStringLiteral("Drill"), kDrillRgb, kRankDrill};
    if (f0 == QLatin1String("nonplated"))
        return Role{QStringLiteral("Drill-NPTH"), kDrillNpthRgb, kRankDrillNpth};
    return Role{sanitizeToken(parts.first().trimmed()), kOtherRgb, kRankOther};
}

// One file ready to import (drill files split into up to two jobs).
struct ImportJob {
    QString path;
    QString base; // file name, for messages
    Role role;
    bool isDrill = false;
    GerberFile gerber;   // when !isDrill
    ExcellonFile drill;  // when isDrill (already filtered by plated flag)
};

// Splits an Excellon file into the hits of plated (or non-plated) tools.
ExcellonFile filterDrill(const ExcellonFile& file, bool plated)
{
    ExcellonFile out;
    out.format = file.format;
    out.unit = file.unit;
    out.warnings = file.warnings;
    out.sawEnd = file.sawEnd;
    for (const auto& [num, tool] : file.tools)
        if (tool.plated == plated)
            out.tools[num] = tool;
    for (const ExcellonHit& h : file.hits)
        if (out.tools.count(h.tool))
            out.hits.push_back(h);
    for (const ExcellonSlot& s : file.drillSlots)
        if (out.tools.count(s.tool))
            out.drillSlots.push_back(s);
    return out;
}

// Endpoint-based bounding box of a parsed Gerber file (aperture footprints
// and arc sagitta ignored — plenty for the coverage heuristic below).
struct KitBbox {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid = false;
    void add(const Vec2d& p)
    {
        if (!valid) {
            x0 = x1 = p.x;
            y0 = y1 = p.y;
            valid = true;
            return;
        }
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    void add(const KitBbox& o)
    {
        if (!o.valid)
            return;
        add(Vec2d{o.x0, o.y0});
        add(Vec2d{o.x1, o.y1});
    }
    double width() const { return valid ? x1 - x0 : 0.0; }
    double height() const { return valid ? y1 - y0 : 0.0; }
};

KitBbox fileBbox(const GerberFile& f)
{
    KitBbox b;
    for (const GerberObject& o : f.objects) {
        switch (o.kind) {
        case GerberObjKind::Draw:
        case GerberObjKind::Arc:
            b.add(o.from);
            b.add(o.to);
            break;
        case GerberObjKind::Flash:
            b.add(o.to);
            break;
        case GerberObjKind::Region:
            for (const GerberContour& c : o.contours) {
                b.add(c.start);
                for (const GerberContourSeg& s : c.segs)
                    b.add(s.to);
            }
            break;
        }
    }
    return b;
}

// True when the file contains at least one stroked object (Draw/Arc). A
// candidate made ONLY of filled regions is a keepout zone, never a contour.
bool hasStrokes(const GerberFile& f)
{
    for (const GerberObject& o : f.objects)
        if (o.kind == GerberObjKind::Draw || o.kind == GerberObjKind::Arc)
            return true;
    return false;
}

} // namespace

bool looksLikeGerberOrExcellon(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    return sniffFile(info.absoluteFilePath()) != Sniff::Unknown;
}

GerberKitResult importGerberKit(Document& doc, const QString& path)
{
    GerberKitResult res;

    const QFileInfo info(path);
    if (!info.exists()) {
        res.error = QStringLiteral("no such file or directory: %1").arg(path);
        return res;
    }
    QStringList paths;
    if (info.isDir()) {
        const QDir dir(path);
        const auto entries =
            dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo& e : entries)
            paths << e.absoluteFilePath();
    } else {
        paths << info.absoluteFilePath();
    }
    if (paths.isEmpty()) {
        res.error = QStringLiteral("empty directory: %1").arg(path);
        return res;
    }

    // ---- recognize + parse every candidate --------------------------------
    struct Candidate {
        QString path, base;
        Role role;
        int outlinePriority = -1; // 0 = GKO/keepout, 1 = GM1; -1 = never
        bool roleFromX2 = false;
        GerberFile gerber;
    };
    std::vector<Candidate> gerbers;
    std::vector<ImportJob> jobs;
    bool haveExplicitOutline = false; // X2 Profile beats the GKO/GM1 election
    // Valid fab files with ZERO drawable objects (Altium ships e.g. a GKO
    // that is just a header + M02). They must not make an open FAIL: a
    // kit skips them, and a lone empty file opens as an empty document
    // with a warning (gerbv behavior).
    int emptyFabFiles = 0;

    for (const QString& p : paths) {
        const QFileInfo fi(p);
        const QString base = fi.fileName();
        const QString ext = fi.suffix().toUpper();
        if (isIgnoredExtension(ext)) {
            res.skipped << QStringLiteral("%1: report/support file").arg(base);
            continue;
        }
        const Sniff sniff = sniffFile(p);
        if (sniff == Sniff::Unknown) {
            res.skipped << QStringLiteral("%1: not a Gerber/Excellon file "
                                          "(content sniff)").arg(base);
            continue;
        }

        // A directory can contain stray/damaged files: skip an unparseable
        // one with a warning instead of making the whole board unopenable.
        // An EXPLICIT single file keeps the parse failure as a hard error.
        const auto parseFailed = [&](const QString& err) {
            if (!info.isDir()) {
                res.error = QStringLiteral("%1: %2").arg(base, err);
                return true; // caller returns res
            }
            res.skipped << QStringLiteral("%1: parse error, layer skipped").arg(base);
            res.warnings << QStringLiteral("%1: %2").arg(base, err);
            return false;
        };

        if (sniff == Sniff::Excellon) {
            const ExcellonParseResult r = parseExcellon(p);
            if (!r.ok) {
                if (parseFailed(r.error))
                    return res;
                continue;
            }
            for (const QString& w : r.file.warnings)
                res.warnings << QStringLiteral("%1: %2").arg(base, w);
            if (r.file.hits.empty() && r.file.drillSlots.empty()) {
                ++emptyFabFiles;
                res.skipped << QStringLiteral("%1: no drill hits").arg(base);
                continue;
            }
            // Plated and non-plated tools land on separate layers.
            for (const bool plated : {true, false}) {
                ExcellonFile part = filterDrill(r.file, plated);
                part.warnings.clear(); // already reported once above
                if (part.hits.empty() && part.drillSlots.empty())
                    continue;
                ImportJob job;
                job.path = p;
                job.base = base;
                job.role = drillRole(plated);
                job.isDrill = true;
                job.drill = std::move(part);
                jobs.push_back(std::move(job));
            }
            continue;
        }

        // Gerber candidate.
        const GerberParseResult r = parseGerber(p);
        if (!r.ok) {
            if (parseFailed(r.error))
                return res;
            continue;
        }
        for (const QString& w : r.file.warnings)
            res.warnings << QStringLiteral("%1: %2").arg(base, w);
        if (r.file.objects.empty()) {
            ++emptyFabFiles;
            res.skipped << QStringLiteral("%1: no graphical objects "
                                          "(empty layer skipped)").arg(base);
            continue;
        }
        Candidate c;
        c.path = p;
        c.base = base;
        c.gerber = r.file;
        // X2 TF.FileFunction prevails over the extension when present.
        const QString fileFunction =
            c.gerber.fileAttributes.value(QStringLiteral(".FileFunction"));
        std::optional<Role> role;
        if (!fileFunction.isEmpty()) {
            role = roleFromFileFunction(fileFunction, c.outlinePriority);
            c.roleFromX2 = role.has_value();
        }
        if (!role)
            role = roleFromExtension(ext, c.outlinePriority);
        if (!role) {
            // Sniffed as Gerber but nameless: keep it, named after the file.
            role = Role{QStringLiteral("Gbr-%1").arg(sanitizeToken(
                            ext.isEmpty() ? fi.completeBaseName().toUpper() : ext)),
                        kOtherRgb, kRankOther};
            res.warnings << QStringLiteral(
                "%1: unrecognized extension, imported as layer %2")
                                .arg(base, role->name);
        }
        if (role->name == QLatin1String("Outline"))
            haveExplicitOutline = true;
        c.role = *role;
        gerbers.push_back(std::move(c));
    }

    // ---- outline election (tolerant heuristic) -----------------------------
    // Altium's GKO is often empty (dropped above) OR a real keepout ZONE
    // (S5M0PCBB: one filled rectangle over the antenna area), with the true
    // contour drawn on GM1 or GM13 (S5M0PCBA). Unless an X2 Profile already
    // claimed "Outline", the PLAUSIBLE candidate with the lowest priority
    // (GKO, then GM1, then GM13) wins; losers keep their fallback role.
    // Plausible = has at least one stroke (a regions-only file is a filled
    // zone, not a contour) AND spans most of the board along AT LEAST one
    // axis (>= 60 % of the union of all Gerber layers). One axis only: the
    // reference PCBB draws just the shaped top edge on GM1 (68 % of the
    // width, 24 % of the height) — still the closest thing to a contour the
    // kit has, while its GKO keepout covers under 20 % of either axis.
    if (!haveExplicitOutline) {
        KitBbox board;
        for (const Candidate& c : gerbers)
            board.add(fileBbox(c.gerber));
        const auto plausibleContour = [&](const Candidate& c) {
            if (!hasStrokes(c.gerber))
                return false;
            const KitBbox b = fileBbox(c.gerber);
            if (!b.valid || !board.valid)
                return false;
            constexpr double kCover = 0.6, kTol = 1e-9;
            return b.width() + kTol >= kCover * board.width() ||
                   b.height() + kTol >= kCover * board.height();
        };
        Candidate* winner = nullptr;
        for (Candidate& c : gerbers)
            if (c.outlinePriority >= 0 &&
                (!winner || c.outlinePriority < winner->outlinePriority) &&
                plausibleContour(c))
                winner = &c;
        if (winner)
            winner->role = Role{QStringLiteral("Outline"), kOutlineRgb, kRankOutline};
    }

    for (Candidate& c : gerbers) {
        ImportJob job;
        job.path = c.path;
        job.base = c.base;
        job.role = c.role;
        job.gerber = std::move(c.gerber);
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) {
        // Everything recognized was VALID but empty (header + M02): open an
        // empty document with a warning instead of failing — gerbv opens
        // such files too, and Altium routinely ships an empty GKO/GM1.
        if (emptyFabFiles > 0) {
            res.warnings << QStringLiteral(
                "%1 valid but empty fabrication file(s) in %2 — nothing to draw")
                                .arg(emptyFabFiles)
                                .arg(path);
            res.ok = true;
            return res;
        }
        res.error =
            QStringLiteral("no Gerber or Excellon files recognized in %1").arg(path);
        return res;
    }

    // ---- paint order + unique layer names ----------------------------------
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const ImportJob& a, const ImportJob& b) {
                         if (a.role.rank != b.role.rank)
                             return a.role.rank < b.role.rank;
                         return a.role.name < b.role.name;
                     });
    QStringList taken;
    for (ImportJob& job : jobs) {
        QString name = job.role.name;
        for (int suffix = 2; taken.contains(name); ++suffix)
            name = QStringLiteral("%1-%2").arg(job.role.name).arg(suffix);
        job.role.name = name;
        taken << name;
    }

    // ---- import: the WHOLE kit is one transaction (single undo step).
    // gerberToDocument / excellonToDocument detect the open transaction and
    // join it instead of opening their own (nested transactions assert).
    TransactionScope tx(doc, QStringLiteral("GERBERKIT"));
    for (ImportJob& job : jobs) {
        doc.ensureLayer(job.role.name, job.role.rgb);
        int added = 0;
        if (job.isDrill) {
            const ExcellonImportResult r =
                excellonToDocument(doc, job.drill, job.role.name);
            if (!r.ok) { // TransactionScope rolls everything back
                res.error = QStringLiteral("%1: %2").arg(job.base, r.error);
                return res;
            }
            for (const QString& w : r.warnings)
                res.warnings << QStringLiteral("%1: %2").arg(job.base, w);
            added = r.entities;
        } else {
            const GerberImportResult r =
                gerberToDocument(doc, job.gerber, job.role.name);
            if (!r.ok) {
                res.error = QStringLiteral("%1: %2").arg(job.base, r.error);
                return res;
            }
            // r.warnings re-seeds the parse warnings; only report the new ones.
            for (const QString& w : r.warnings)
                if (!job.gerber.warnings.contains(w))
                    res.warnings << QStringLiteral("%1: %2").arg(job.base, w);
            added = r.entities;
        }
        res.files.push_back(
            {job.path, job.role.name, job.isDrill, job.role.rgb, added});
        res.layers << job.role.name;
        res.entities += added;
    }
    tx.commit();
    res.ok = true;
    return res;
}

} // namespace viki
