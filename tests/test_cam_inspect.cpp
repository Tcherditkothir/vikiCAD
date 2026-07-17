// G2 inspection — "what IS this gerber object?":
//  - the Gerber importer tags entities with their D-code and persists the
//    file's APERTURE TABLE in the layer's camMeta (round-trips .vkd);
//  - the Excellon importer tags hits with their tool and persists the TOOL
//    TABLE the same way;
//  - APERTURES prints a layer's aperture table (text + JSON trailer);
//  - DRILLREPORT prints the hole table by diameter over the LIVE circles;
//  - a kit imported into a VIRGIN document drops the residual layer "0".
//
// Two tiers like the other gerber suites: synthetic data inline, plus the
// real Altium kits under /home/lex/computer/pcb-ref (SKIP when absent) with
// the .REP/.DRR reports as independent ground truths.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <map>
#include <set>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Document.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "io/ExcellonIo.h"
#include "io/GerberIo.h"
#include "io/GerberKit.h"
#include "io/NativeStore.h"

using namespace viki;
using Catch::Approx;

namespace {

const char* kKitRoot = "/home/lex/computer/pcb-ref";

bool kitsPresent()
{
    return QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBA")).exists() &&
           QDir(QLatin1String(kKitRoot) + QLatin1String("/S5M0PCBB")).exists();
}

const Layer* layerNamed(const Document& doc, const char* name)
{
    for (const Layer& l : doc.layers())
        if (l.name == QLatin1String(name))
            return &l;
    return nullptr;
}

// Synthetic RS-274X exercising every table entry kind: a C draw aperture, a
// R flash, an AMPARAMS-described macro flash, a C with a round hole (flash),
// and a G36 region (aperture-less). mm 2.4.
QByteArray inspectionGerber()
{
    return "G04 inspection synthetic*\n"
           "%FSLAX24Y24*%\n"
           "%MOMM*%\n"
           "G01*\n"
           "%ADD10C,0.2*%\n"
           "%ADD11R,1.5X2*%\n"
           "G04:AMPARAMS|DCode=15|XSize=23.62mil|YSize=35.43mil|"
           "CornerRadius=2.13mil|HoleSize=0mil|Usage=FLASHONLY|"
           "Rotation=270.000|XOffset=0mil|YOffset=0mil|HoleType=Round|"
           "Shape=RoundedRectangle|*\n"
           "%AMROUNDEDRECTD15*\n"
           "21,1,0.5,0.8,0,0,270.0*\n"
           "%\n"
           "%ADD15ROUNDEDRECTD15*%\n"
           "%ADD20C,1X0.3*%\n"
           "D10*\n"
           "X0Y0D02*\n"
           "X100000Y0D01*\n"
           "X100000Y50000D01*\n"
           "D11*\n"
           "X20000Y20000D03*\n"
           "D15*\n"
           "X40000Y20000D03*\n"
           "D20*\n"
           "X60000Y20000D03*\n"
           "G36*\n"
           "X0Y-30000D02*\n"
           "X10000D01*\n"
           "Y-20000D01*\n"
           "X0D01*\n"
           "Y-30000D01*\n"
           "G37*\n"
           "M02*\n";
}

// Synthetic Excellon: T1 plated 1 mm (2 hits), T2 non-plated 3 mm (1 hit).
QByteArray inspectionDrill()
{
    return "M48\n"
           ";FILE_FORMAT=2:4\n"
           "INCH,TZ\n"
           ";TYPE=PLATED\n"
           "T1C0.03937\n"
           ";TYPE=NON_PLATED\n"
           "T2C0.11811\n"
           "%\n"
           "T1\n"
           "X10000Y10000\n"
           "X20000Y10000\n"
           "T2\n"
           "X30000Y30000\n"
           "M30\n";
}

void writeFile(const QString& dir, const QString& name, const QByteArray& data)
{
    QFile f(dir + QLatin1Char('/') + name);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// ctx.messages() is a std::vector<QString>; QStringList is handier here.
QStringList msgList(const std::vector<QString>& messages)
{
    QStringList out;
    for (const QString& m : messages)
        out << m;
    return out;
}

// Last '{'-starting message = the machine trailer (same contract as MINDIST).
QJsonObject lastJsonTrailer(const QStringList& messages)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        if (it->startsWith(QLatin1Char('{')))
            return QJsonDocument::fromJson(it->toUtf8()).object();
    return {};
}

// --- .REP / .DRR parsing (duplicated from test_gerber / test_excellon on
// purpose: independent TU, same independent ground truth) -------------------

QMap<QString, std::set<int>> parseRepDcodes(const QString& path)
{
    QMap<QString, std::set<int>> out;
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QString current;
    bool collecting = false;
    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine()).trimmed();
        if (line.startsWith(QStringLiteral("File :"))) {
            current = line.mid(6).trimmed().section(QLatin1Char('.'), -1);
            out[current];
            collecting = false;
            continue;
        }
        if (line.startsWith(QStringLiteral("Used DCodes"))) {
            collecting = true;
            continue;
        }
        if (collecting) {
            if (line.startsWith(QLatin1Char('D')) && line.size() > 1) {
                bool ok = false;
                const int d = line.mid(1).toInt(&ok);
                if (ok)
                    out[current].insert(d);
            } else if (line.startsWith(QLatin1Char('*'))) {
                collecting = false;
            }
        }
    }
    return out;
}

struct DrrTool {
    double mm = 0.0;
    int count = 0;
    bool plated = true;
};

std::map<int, DrrTool> parseDrr(const QString& path, int& total)
{
    std::map<int, DrrTool> out;
    total = -1;
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    while (!f.atEnd()) {
        const QStringList tok =
            QString::fromLatin1(f.readLine()).simplified().split(QLatin1Char(' '));
        if (tok.size() >= 2 && tok[0] == QLatin1String("Totals")) {
            total = tok[1].toInt();
            continue;
        }
        if (tok.size() < 5 || !tok[0].startsWith(QLatin1Char('T')))
            continue;
        bool numOk = false;
        const int num = tok[0].mid(1).toInt(&numOk);
        if (!numOk)
            continue;
        const int round = tok.indexOf(QStringLiteral("Round"));
        if (round < 0 || round + 2 >= tok.size())
            continue;
        DrrTool t;
        t.count = tok[round + 1].toInt();
        t.plated = tok[round + 2] == QLatin1String("PTH");
        for (const QString& s : tok) {
            if (s.startsWith(QLatin1Char('(')) && s.endsWith(QLatin1String("mm)"))) {
                t.mm = s.mid(1, s.size() - 4).toDouble();
                break;
            }
        }
        out[num] = t;
    }
    return out;
}

} // namespace

TEST_CASE("Gerber import: dcode tags + persisted aperture table", "[gerber][g2cam]")
{
    const GerberParseResult r = parseGerberData(inspectionGerber());
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    // The AMPARAMS comment was captured, keyed by D-code.
    REQUIRE(r.file.amParams.contains(15));
    CHECK(r.file.amParams[15].contains(QStringLiteral("Shape=RoundedRectangle")));

    Document doc;
    const GerberImportResult res =
        gerberToDocument(doc, r.file, QStringLiteral("GTL"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    // 1 coalesced polyline + 3 flashes + 1 region.
    REQUIRE(res.entities == 5);

    const auto checkDoc = [](const Document& d) {
        const Layer* layer = nullptr;
        for (const Layer& l : d.layers())
            if (l.name == QLatin1String("GTL"))
                layer = &l;
        REQUIRE(layer != nullptr);

        // Entity tags: the trace polyline carries its stroking aperture,
        // each flash its flashed aperture, the region NO dcode.
        REQUIRE(d.drawOrder().size() == 5);
        const Entity* trace = d.entity(d.drawOrder()[0]);
        const Entity* flashR = d.entity(d.drawOrder()[1]);
        const Entity* flashM = d.entity(d.drawOrder()[2]);
        const Entity* flashH = d.entity(d.drawOrder()[3]);
        const Entity* region = d.entity(d.drawOrder()[4]);
        REQUIRE(dynamic_cast<const PolylineEntity*>(trace) != nullptr);
        REQUIRE(dynamic_cast<const InsertEntity*>(flashR) != nullptr);
        REQUIRE(dynamic_cast<const HatchEntity*>(region) != nullptr);
        CHECK(trace->extra().value(QLatin1String("dcode")).toInt() == 10);
        CHECK(flashR->extra().value(QLatin1String("dcode")).toInt() == 11);
        CHECK(flashM->extra().value(QLatin1String("dcode")).toInt() == 15);
        CHECK(flashH->extra().value(QLatin1String("dcode")).toInt() == 20);
        CHECK_FALSE(region->extra().contains(QLatin1String("dcode")));

        // The aperture table on the layer: every defined D-code, mm params,
        // usage counts, friendly descriptions (AMPARAMS for the macro).
        const QJsonObject table =
            layer->camMeta.value(QLatin1String("apertures")).toObject();
        REQUIRE(table.size() == 4);
        const QJsonObject d10 = table.value(QLatin1String("D10")).toObject();
        CHECK(d10.value(QLatin1String("shape")).toString() == QStringLiteral("Circle"));
        CHECK(d10.value(QLatin1String("params")).toArray().at(0).toDouble() ==
              Approx(0.2).margin(1e-9));
        CHECK(d10.value(QLatin1String("usage")).toInt() == 2); // two draws
        CHECK(d10.value(QLatin1String("desc")).toString() ==
              QStringLiteral("Circle d=0.200"));

        const QJsonObject d11 = table.value(QLatin1String("D11")).toObject();
        CHECK(d11.value(QLatin1String("shape")).toString() == QStringLiteral("Rect"));
        CHECK(d11.value(QLatin1String("desc")).toString() ==
              QStringLiteral("Rect 1.500x2.000"));
        CHECK(d11.value(QLatin1String("usage")).toInt() == 1);

        // The mission example, verbatim: AMPARAMS beats raw primitives.
        const QJsonObject d15 = table.value(QLatin1String("D15")).toObject();
        CHECK(d15.value(QLatin1String("shape")).toString() == QStringLiteral("Macro"));
        CHECK(d15.value(QLatin1String("macro")).toString() ==
              QStringLiteral("ROUNDEDRECTD15"));
        CHECK(d15.value(QLatin1String("desc")).toString() ==
              QStringLiteral("RoundedRect 0.600x0.900 r=0.054 rot 270deg"));

        const QJsonObject d20 = table.value(QLatin1String("D20")).toObject();
        CHECK(d20.value(QLatin1String("hole")).toDouble() == Approx(0.3).margin(1e-9));
        CHECK(d20.value(QLatin1String("desc")).toString() ==
              QStringLiteral("Circle d=1.000 hole=0.300"));
    };
    checkDoc(doc);

    // Round-trip .vkd: tags AND the layer table survive.
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("cam.vkd"));
    QString error;
    REQUIRE(NativeStore::save(doc, path, error));
    const auto loaded = NativeStore::load(path, error);
    INFO(error.toStdString());
    REQUIRE(loaded != nullptr);
    checkDoc(*loaded);
}

TEST_CASE("Excellon import: tool tags + persisted tool table", "[excellon][g2cam]")
{
    const ExcellonParseResult r = parseExcellonData(inspectionDrill());
    INFO(r.error.toStdString());
    REQUIRE(r.ok);

    Document doc;
    const ExcellonImportResult res =
        excellonToDocument(doc, r.file, QStringLiteral("Drill"));
    REQUIRE(res.ok);
    REQUIRE(res.entities == 3);

    const auto checkDoc = [](const Document& d) {
        const Entity* h0 = d.entity(d.drawOrder()[0]);
        const Entity* h2 = d.entity(d.drawOrder()[2]);
        CHECK(h0->extra().value(QLatin1String("tool")).toString() ==
              QStringLiteral("T1"));
        CHECK(h0->extra().value(QLatin1String("plated")).toBool());
        CHECK(h2->extra().value(QLatin1String("tool")).toString() ==
              QStringLiteral("T2"));
        CHECK_FALSE(h2->extra().value(QLatin1String("plated")).toBool());

        const Layer* layer = nullptr;
        for (const Layer& l : d.layers())
            if (l.name == QLatin1String("Drill"))
                layer = &l;
        REQUIRE(layer != nullptr);
        const QJsonObject tools =
            layer->camMeta.value(QLatin1String("tools")).toObject();
        REQUIRE(tools.size() == 2);
        const QJsonObject t1 = tools.value(QLatin1String("T1")).toObject();
        CHECK(t1.value(QLatin1String("dia")).toDouble() ==
              Approx(0.03937 * 25.4).margin(1e-6));
        CHECK(t1.value(QLatin1String("plated")).toBool());
        CHECK(t1.value(QLatin1String("usage")).toInt() == 2);
        const QJsonObject t2 = tools.value(QLatin1String("T2")).toObject();
        CHECK_FALSE(t2.value(QLatin1String("plated")).toBool());
        CHECK(t2.value(QLatin1String("usage")).toInt() == 1);
    };
    checkDoc(doc);

    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("drill.vkd"));
    QString error;
    REQUIRE(NativeStore::save(doc, path, error));
    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded != nullptr);
    checkDoc(*loaded);
}

TEST_CASE("APERTURES command: aligned table + JSON trailer, both spellings",
          "[commands][g2cam]")
{
    Document doc;
    SelectionSet sel;
    CommandContext ctx{doc, sel};
    CommandProcessor processor{ctx};
    registerBuiltinCommands(processor);

    const GerberParseResult r = parseGerberData(inspectionGerber());
    REQUIRE(r.ok);
    REQUIRE(gerberToDocument(doc, r.file, QStringLiteral("GTL")).ok);

    // Named layer (case-insensitive, like every layer lookup).
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("APERTURES gtl"), true).ok);
    const QStringList msgs = msgList(ctx.messages());
    REQUIRE(!msgs.isEmpty());
    CHECK(msgs.first().contains(QStringLiteral("apertures on 'GTL'")));
    CHECK(msgs.first().contains(QStringLiteral("4 D-code(s)")));
    CHECK(msgs.filter(QStringLiteral("D15")).size() >= 1);
    CHECK(msgs.filter(QStringLiteral("RoundedRect 0.600x0.900")).size() >= 1);
    const QJsonObject trailer = lastJsonTrailer(msgs);
    const QJsonObject table = trailer.value(QLatin1String("apertures"))
                                  .toObject()
                                  .value(QLatin1String("GTL"))
                                  .toObject();
    REQUIRE(table.size() == 4);
    CHECK(table.value(QLatin1String("D10")).toObject()
              .value(QLatin1String("usage")).toInt() == 2);

    // Bare APERTURES = every layer that has a table (here: just GTL).
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("APERTURES"), true).ok);
    CHECK(msgList(ctx.messages()).filter(QStringLiteral("apertures on 'GTL'")).size() == 1);

    // Unknown layer: a clean message, not an error.
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("APERTURES Nope"), true).ok);
    CHECK(msgList(ctx.messages()).filter(QStringLiteral("no layer named")).size() == 1);

    // Alias.
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("APER GTL"), true).ok);
    CHECK(msgList(ctx.messages()).filter(QStringLiteral("apertures on 'GTL'")).size() == 1);
}

TEST_CASE("SELECT command: headless selection set (feeds pickfirst + panel)",
          "[commands][g2cam]")
{
    Document doc;
    SelectionSet sel;
    CommandContext ctx{doc, sel};
    CommandProcessor processor{ctx};
    registerBuiltinCommands(processor);

    const ExcellonParseResult r = parseExcellonData(inspectionDrill());
    REQUIRE(r.ok);
    REQUIRE(excellonToDocument(doc, r.file, QStringLiteral("Drill")).ok);
    const EntityId a = doc.drawOrder()[0];
    const EntityId b = doc.drawOrder()[1];

    // Replace-the-set semantics, bogus ids ignored with a message.
    REQUIRE(processor.submit(QStringLiteral("SELECT %1 %2").arg(a).arg(b), true).ok);
    CHECK(sel.size() == 2);
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("SEL 99999"), true).ok); // alias
    CHECK(sel.isEmpty());
    CHECK(msgList(ctx.messages()).filter(QStringLiteral("do not exist")).size() == 1);

    // The pre-selected pair drives MINDIST (pickfirst parity, headless).
    REQUIRE(processor.submit(QStringLiteral("SELECT %1 %2").arg(a).arg(b), true).ok);
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("MINDIST"), true).ok);
    const QJsonObject md = lastJsonTrailer(msgList(ctx.messages()))
                               .value(QLatin1String("mindist")).toObject();
    CHECK(md.value(QLatin1String("a")).toInt() == int(a));
    CHECK(md.value(QLatin1String("b")).toInt() == int(b));

    // Bare SELECT clears.
    REQUIRE(processor.submit(QStringLiteral("SELECT"), true).ok);
    CHECK(sel.isEmpty());
}

TEST_CASE("DRILLREPORT command: live counts by diameter, erase updates it",
          "[commands][g2cam]")
{
    Document doc;
    SelectionSet sel;
    CommandContext ctx{doc, sel};
    CommandProcessor processor{ctx};
    registerBuiltinCommands(processor);

    const ExcellonParseResult r = parseExcellonData(inspectionDrill());
    REQUIRE(r.ok);
    REQUIRE(excellonToDocument(doc, r.file, QStringLiteral("Drill")).ok);

    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("DRILLREPORT"), true).ok);
    QJsonObject rep = lastJsonTrailer(msgList(ctx.messages()))
                          .value(QLatin1String("drillreport")).toObject();
    CHECK(rep.value(QLatin1String("total")).toInt() == 3);
    CHECK(rep.value(QLatin1String("plated")).toInt() == 2);
    CHECK(rep.value(QLatin1String("npth")).toInt() == 1);
    REQUIRE(rep.value(QLatin1String("rows")).toArray().size() == 2);
    const QJsonObject row0 =
        rep.value(QLatin1String("rows")).toArray().at(0).toObject();
    CHECK(row0.value(QLatin1String("dia")).toDouble() ==
          Approx(0.03937 * 25.4).margin(1e-6));
    CHECK(row0.value(QLatin1String("count")).toInt() == 2);
    CHECK(row0.value(QLatin1String("tools")).toArray().at(0).toString() ==
          QStringLiteral("T1"));

    // The report reads the LIVE document: erase one T1 hit, count drops.
    const EntityId first = doc.drawOrder().front();
    REQUIRE(processor.submit(QStringLiteral("ERASE %1").arg(first), true).ok);
    ctx.clearMessages();
    REQUIRE(processor.submit(QStringLiteral("DR"), true).ok); // alias
    rep = lastJsonTrailer(msgList(ctx.messages()))
              .value(QLatin1String("drillreport")).toObject();
    CHECK(rep.value(QLatin1String("total")).toInt() == 2);
    CHECK(rep.value(QLatin1String("plated")).toInt() == 1);

    // Empty document: honest message, no crash.
    Document empty;
    SelectionSet sel2;
    CommandContext ctx2{empty, sel2};
    CommandProcessor p2{ctx2};
    registerBuiltinCommands(p2);
    REQUIRE(p2.submit(QStringLiteral("DRILLREPORT"), true).ok);
    CHECK(msgList(ctx2.messages()).filter(QStringLiteral("no drill hits")).size() == 1);
}

TEST_CASE("Kit import drops the residual layer '0' on a VIRGIN document only",
          "[gerberkit][g2cam]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();
    writeFile(dir, QStringLiteral("board.GTL"), inspectionGerber());
    writeFile(dir, QStringLiteral("board.TXT"), inspectionDrill());

    SECTION("virgin document: '0' gone, current layer = first kit layer")
    {
        Document doc;
        REQUIRE(doc.layers().size() == 1); // the constructor default
        const GerberKitResult r = importGerberKit(doc, dir);
        REQUIRE(r.ok);
        CHECK(layerNamed(doc, "0") == nullptr);
        REQUIRE(!doc.layers().empty());
        CHECK(doc.currentLayer() != 0);
        const Layer* current = doc.layer(doc.currentLayer());
        REQUIRE(current != nullptr);
        CHECK(current->name == r.layers.first());

        // UNDO keeps the kit layers (direct edits) and re-adding entities
        // by REDO still lands them on their original layers.
        const size_t n = doc.entityCount();
        CHECK(doc.undo() == QStringLiteral("GERBERKIT"));
        CHECK(doc.entityCount() == 0);
        CHECK(layerNamed(doc, "0") == nullptr);
        CHECK(doc.redo() == QStringLiteral("GERBERKIT"));
        CHECK(doc.entityCount() == n);

        // And the saved file round-trips WITHOUT resurrecting layer "0".
        QTemporaryDir tmp2;
        const QString path = tmp2.filePath(QStringLiteral("kit.vkd"));
        QString error;
        REQUIRE(NativeStore::save(doc, path, error));
        const auto loaded = NativeStore::load(path, error);
        REQUIRE(loaded != nullptr);
        CHECK(layerNamed(*loaded, "0") == nullptr);
        CHECK(loaded->layers().size() == doc.layers().size());
    }

    SECTION("document with content on '0': the layer stays")
    {
        Document doc;
        doc.beginTransaction(QStringLiteral("LINE"));
        auto line = std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{1, 1});
        doc.addEntity(std::move(line));
        doc.commitTransaction();
        const GerberKitResult r = importGerberKit(doc, dir);
        REQUIRE(r.ok);
        CHECK(layerNamed(doc, "0") != nullptr);
        CHECK(doc.currentLayer() == 0);
    }
}

// ---------------------------------------------------------------------------
// Real kits: the .REP and .DRR reports are the independent ground truths.
// ---------------------------------------------------------------------------

TEST_CASE("Real kits: persisted aperture usage matches the .REP reports",
          "[gerberkit][g2cam][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    struct Kit {
        const char* dir;
        const char* prefix;
    };
    for (const Kit& kit : {Kit{"S5M0PCBA", "S5M0PCBA1"}, Kit{"S5M0PCBB", "S5M0PCBB1"}}) {
        Document doc;
        const QString root = QStringLiteral("%1/%2").arg(
            QLatin1String(kKitRoot), QLatin1String(kit.dir));
        const GerberKitResult r = importGerberKit(doc, root);
        REQUIRE(r.ok);
        const auto rep = parseRepDcodes(
            QStringLiteral("%1/%2.REP").arg(root, QLatin1String(kit.prefix)));

        int checkedLayers = 0;
        for (const GerberKitFile& f : r.files) {
            if (f.isDrill)
                continue;
            const QString ext = f.path.section(QLatin1Char('.'), -1).toUpper();
            if (!rep.contains(ext))
                continue;
            const Layer* layer = doc.layerByName(f.layerName);
            REQUIRE(layer != nullptr);
            // The key is always stored; the table itself CAN be empty (a
            // region-only file like PCBB's GKO keepout defines no aperture
            // — and its .REP "Used DCodes" list is empty too).
            REQUIRE(layer->camMeta.contains(QLatin1String("apertures")));
            const QJsonObject table =
                layer->camMeta.value(QLatin1String("apertures")).toObject();
            // The set of D-codes with usage > 0 must equal the .REP's
            // "Used DCodes" list for that file, exactly.
            std::set<int> used;
            for (auto it = table.begin(); it != table.end(); ++it)
                if (it.value().toObject().value(QLatin1String("usage")).toInt() > 0)
                    used.insert(it.key().mid(1).toInt());
            INFO(kit.dir << " " << ext.toStdString() << " (layer "
                         << f.layerName.toStdString() << ")");
            CHECK(used == rep[ext]);
            ++checkedLayers;
        }
        CHECK(checkedLayers >= 10); // the whole gerber stack was compared
    }
}

TEST_CASE("Real kits: DRILLREPORT matches the .DRR reports (golden)",
          "[gerberkit][g2cam][kits]")
{
    if (!kitsPresent())
        SKIP("real Gerber kits not present on this machine");

    struct Kit {
        const char* dir;
        const char* prefix;
    };
    for (const Kit& kit : {Kit{"S5M0PCBA", "S5M0PCBA1"}, Kit{"S5M0PCBB", "S5M0PCBB1"}}) {
        Document doc;
        SelectionSet sel;
        CommandContext ctx{doc, sel};
        CommandProcessor processor{ctx};
        registerBuiltinCommands(processor);

        const QString root = QStringLiteral("%1/%2").arg(
            QLatin1String(kKitRoot), QLatin1String(kit.dir));
        REQUIRE(importGerberKit(doc, root).ok);

        int drrTotal = -1;
        const auto drr = parseDrr(
            QStringLiteral("%1/%2.DRR").arg(root, QLatin1String(kit.prefix)),
            drrTotal);
        REQUIRE(!drr.empty());
        REQUIRE(drrTotal > 0);

        ctx.clearMessages();
        REQUIRE(processor.submit(QStringLiteral("DRILLREPORT"), true).ok);
        const QJsonObject repJson = lastJsonTrailer(msgList(ctx.messages()))
                                        .value(QLatin1String("drillreport"))
                                        .toObject();
        const QJsonArray rows = repJson.value(QLatin1String("rows")).toArray();

        CHECK(repJson.value(QLatin1String("total")).toInt() == drrTotal);
        // Every .DRR tool must land in exactly one row: same plating, the
        // rounded .DRR diameter within 0.01 mm, the exact hole count, and
        // the tool number listed on the row.
        int matched = 0;
        for (const auto& [num, want] : drr) {
            INFO(kit.dir << " T" << num);
            const QString tname = QStringLiteral("T%1").arg(num);
            bool found = false;
            for (const QJsonValue& rv : rows) {
                const QJsonObject row = rv.toObject();
                QStringList tools;
                for (const QJsonValue& t :
                     row.value(QLatin1String("tools")).toArray())
                    tools << t.toString();
                if (!tools.contains(tname))
                    continue;
                found = true;
                CHECK(row.value(QLatin1String("plated")).toBool() == want.plated);
                CHECK(row.value(QLatin1String("dia")).toDouble() ==
                      Approx(want.mm).margin(0.01));
                CHECK(row.value(QLatin1String("count")).toInt() == want.count);
            }
            CHECK(found);
            if (found)
                ++matched;
        }
        CHECK(matched == int(drr.size()));
        // No phantom rows: the .DRR tool set covers every reported row.
        CHECK(rows.size() == int(drr.size()));
    }
}
