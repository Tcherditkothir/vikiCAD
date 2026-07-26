#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "doc/Annotations.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "io/EagleImporter.h"

using namespace viki;
using Catch::Approx;

namespace {

QString writeTemp(const QTemporaryDir& dir, const char* name, const QByteArray& data)
{
    const QString path = dir.path() + QLatin1Char('/') + QLatin1String(name);
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(data);
    return path;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

const Layer* layerOf(const Document& doc, const Entity* e)
{
    return doc.layer(e->layerId());
}

template <typename T, typename Pred>
const T* findEntity(const Document& doc, Pred pred)
{
    for (const EntityId id : doc.drawOrder()) {
        const auto* e = dynamic_cast<const T*>(doc.entity(id));
        if (e && pred(*e))
            return e;
    }
    return nullptr;
}

const char kBoardXml[] = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE eagle SYSTEM "eagle.dtd">
<eagle version="6.4">
<drawing>
<layers>
<layer number="1" name="Top" color="4" fill="1" visible="yes" active="yes"/>
<layer number="16" name="Bottom" color="1" fill="1" visible="yes" active="yes"/>
<layer number="17" name="Pads" color="2" fill="1" visible="yes" active="yes"/>
<layer number="18" name="Vias" color="2" fill="1" visible="yes" active="yes"/>
<layer number="20" name="Dimension" color="15" fill="1" visible="yes" active="yes"/>
<layer number="21" name="tPlace" color="7" fill="1" visible="yes" active="yes"/>
<layer number="22" name="bPlace" color="7" fill="1" visible="yes" active="yes"/>
<layer number="25" name="tNames" color="7" fill="1" visible="yes" active="yes"/>
<layer number="26" name="bNames" color="7" fill="1" visible="yes" active="yes"/>
<layer number="29" name="tStop" color="7" fill="3" visible="no" active="yes"/>
<layer number="45" name="Holes" color="7" fill="1" visible="no" active="yes"/>
</layers>
<board>
<plain>
<wire x1="0" y1="0" x2="50" y2="0" width="0" layer="20"/>
<wire x1="50" y1="0" x2="50" y2="40" width="0" layer="20" curve="90"/>
<hole x="5" y="35" drill="3.2"/>
<dimension x1="0" y1="0" x2="50" y2="0" x3="25" y3="-8" textsize="1.27" layer="20"/>
</plain>
<libraries>
<library name="lib1">
<packages>
<package name="P1">
<description>test</description>
<wire x1="-1" y1="1" x2="1" y2="1" width="0.2" layer="21"/>
<smd name="1" x="-1" y="0" dx="1.2" dy="1.4" layer="1"/>
<pad name="2" x="1" y="0" drill="0.8" diameter="1.6" shape="octagon"/>
<text x="0" y="2" size="1.27" layer="25">&gt;NAME</text>
<rectangle x1="-0.4" y1="-0.3" x2="0.4" y2="0.3" layer="21"/>
</package>
</packages>
</library>
</libraries>
<elements>
<element name="R1" library="lib1" package="P1" value="10k" x="10" y="10"/>
<element name="C1" library="lib1" package="P1" value="1u" x="30" y="10" rot="MR180"/>
</elements>
<signals>
<signal name="GND">
<contactref element="R1" pad="2"/>
<wire x1="10" y1="10" x2="20" y2="10" width="0.4064" layer="1"/>
<wire x1="20" y1="10" x2="30" y2="20" width="0.4064" layer="16"/>
<via x="20" y="10" extent="1-16" drill="0.6"/>
<polygon width="0.2" layer="1">
<vertex x="2" y="2"/>
<vertex x="12" y="2"/>
<vertex x="12" y="12" curve="-90"/>
<vertex x="2" y="12"/>
</polygon>
</signal>
</signals>
</board>
</drawing>
</eagle>
)";

const char kSchXml[] = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE eagle SYSTEM "eagle.dtd">
<eagle version="7.7.0">
<drawing>
<layers>
<layer number="91" name="Nets" color="2" fill="1" visible="yes" active="yes"/>
<layer number="94" name="Symbols" color="4" fill="1" visible="yes" active="yes"/>
<layer number="95" name="Names" color="7" fill="1" visible="yes" active="yes"/>
<layer number="96" name="Values" color="7" fill="1" visible="yes" active="yes"/>
</layers>
<schematic>
<libraries>
<library name="libA">
<symbols>
<symbol name="RES">
<wire x1="0" y1="0" x2="2.54" y2="0" width="0.254" layer="94"/>
<pin name="A" x="-2.54" y="0" length="short"/>
<text x="0" y="1" size="1.778" layer="95">&gt;NAME</text>
<text x="0" y="-2" size="1.778" layer="96">&gt;VALUE</text>
</symbol>
</symbols>
<devicesets>
<deviceset name="RESISTOR" prefix="R">
<gates>
<gate name="G$1" symbol="RES" x="0" y="0"/>
</gates>
<devices>
<device name="R0805" package="R0805">
</device>
</devices>
</deviceset>
</devicesets>
</library>
</libraries>
<parts>
<part name="R1" library="libA" deviceset="RESISTOR" device="R0805" value="4k7"/>
</parts>
<sheets>
<sheet>
<plain>
<text x="1" y="1" size="2" layer="95">feuille un</text>
</plain>
<instances>
<instance part="R1" gate="G$1" x="10" y="10" rot="R90"/>
</instances>
<nets>
<net name="N$1" class="0">
<segment>
<pinref part="R1" gate="G$1" pin="A"/>
<wire x1="10" y1="7.46" x2="10" y2="2" width="0.1524" layer="91"/>
<junction x="10" y="2"/>
<label x="11" y="2" size="1.778" layer="95"/>
</segment>
</net>
</nets>
</sheet>
<sheet>
<plain>
<text x="1" y="1" size="2" layer="95">feuille deux</text>
</plain>
</sheet>
</sheets>
</schematic>
</drawing>
</eagle>
)";

} // namespace

TEST_CASE("EAGLE board import: layers, tracks, pads, mirroring", "[eagle]")
{
    QTemporaryDir dir;
    const QString path = writeTemp(dir, "test.brd", kBoardXml);

    auto result = importEagle(path);
    INFO(result.error.toStdString());
    REQUIRE(result.ok);
    CHECK(result.kind == "board");
    CHECK(result.sheets == 0);
    REQUIRE(result.document);
    Document& doc = *result.document;

    SECTION("layers carry EAGLE names, colors and visibility")
    {
        const Layer* top = doc.layerByName("Top");
        REQUIRE(top);
        CHECK(top->rgb == 0xB40000); // EAGLE color index 4 = red
        const Layer* stop = doc.layerByName("tStop");
        // tStop never received an entity, so it must not exist at all.
        CHECK(stop == nullptr);
        const Layer* holes = doc.layerByName("Holes");
        REQUIRE(holes);
        CHECK_FALSE(holes->visible);
        // Bottom paints before Top.
        const Layer* bottom = doc.layerByName("Bottom");
        REQUIRE(bottom);
        CHECK(bottom->rank < top->rank);
    }

    SECTION("board outline: thin wire + arc wire (curve)")
    {
        const auto* straight = findEntity<PolylineEntity>(doc, [](const PolylineEntity& p) {
            return p.vertices().size() == 2 && near(p.vertices()[0].pos.x, 0) &&
                   near(p.vertices()[1].pos.x, 50) && near(p.width(), 0);
        });
        REQUIRE(straight);
        const auto* curved = findEntity<PolylineEntity>(doc, [](const PolylineEntity& p) {
            return p.vertices().size() == 2 && p.vertices()[0].bulge != 0.0;
        });
        REQUIRE(curved);
        // 90 degree included angle -> bulge tan(22.5)
        CHECK(curved->vertices()[0].bulge == Approx(std::tan(M_PI / 8)).margin(1e-9));
    }

    SECTION("tracks keep width, vias are drill-pierced annuli")
    {
        const auto* track = findEntity<PolylineEntity>(doc, [](const PolylineEntity& p) {
            return near(p.width(), 0.4064);
        });
        REQUIRE(track);
        const auto* via = findEntity<HatchEntity>(doc, [](const HatchEntity& h) {
            if (h.rings.size() != 2 || h.rings[0].empty())
                return false;
            BBox2d box;
            for (const auto& p : h.rings[0])
                box.expand(p);
            return near(box.center().x, 20) && near(box.center().y, 10);
        });
        REQUIRE(via);
        CHECK(layerOf(doc, via)->name == "Vias");
    }

    SECTION("pads: octagon ring, smd rect, package silk at element position")
    {
        // Octagon pad of R1 at package-local (1,0) -> board (11,10).
        const auto* pad = findEntity<HatchEntity>(doc, [&](const HatchEntity& h) {
            if (h.rings.size() != 2 || h.rings[0].size() != 8)
                return false;
            double cx = 0, cy = 0;
            for (const auto& p : h.rings[0]) { cx += p.x; cy += p.y; }
            cx /= 8; cy /= 8;
            return near(cx, 11) && near(cy, 10);
        });
        REQUIRE(pad);
        CHECK(layerOf(doc, pad)->name == "Pads");
        // R1's smd: solid 1-ring hatch on Top around (9,10).
        const auto* smd = findEntity<HatchEntity>(doc, [&](const HatchEntity& h) {
            if (h.rings.size() != 1 || h.rings[0].size() != 4)
                return false;
            double cx = 0;
            for (const auto& p : h.rings[0]) cx += p.x;
            return near(cx / 4, 9.0);
        });
        REQUIRE(smd);
        CHECK(layerOf(doc, smd)->name == "Top");
    }

    SECTION("mirrored element: smd flips to Bottom, silk to bPlace")
    {
        // C1 at (30,10) MR180: smd local (-1,0) -> mirror (1,0) -> R180 -> (29,10).
        const auto* smd = findEntity<HatchEntity>(doc, [&](const HatchEntity& h) {
            if (h.rings.size() != 1 || h.rings[0].size() != 4)
                return false;
            double cx = 0;
            for (const auto& p : h.rings[0]) cx += p.x;
            return near(cx / 4, 29.0);
        });
        REQUIRE(smd);
        CHECK(layerOf(doc, smd)->name == "Bottom");
        const auto* silk = findEntity<PolylineEntity>(doc, [&](const PolylineEntity& p) {
            return near(p.width(), 0.2) && !p.isClosed() &&
                   layerOf(doc, &p) && layerOf(doc, &p)->name == "bPlace";
        });
        CHECK(silk);
    }

    SECTION(">NAME substitutes to the element name")
    {
        const auto* name = findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "R1";
        });
        REQUIRE(name);
        CHECK(layerOf(doc, name)->name == "tNames");
        CHECK(findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text().contains(">");
        }) == nullptr);
    }

    SECTION("copper polygon: closed outline with its wire width")
    {
        const auto* pour = findEntity<PolylineEntity>(doc, [](const PolylineEntity& p) {
            return p.isClosed() && p.vertices().size() == 4;
        });
        REQUIRE(pour);
        CHECK(near(pour->width(), 0.2));
        CHECK(pour->vertices()[2].bulge != 0.0);
    }

    SECTION("EAGLE dimension becomes a live DimensionEntity")
    {
        const auto* dim = findEntity<DimensionEntity>(doc, [](const DimensionEntity& d) {
            return d.kind == DimensionEntity::Kind::Aligned && near(d.a.x, 0) &&
                   near(d.b.x, 50) && near(d.pos.y, -8);
        });
        REQUIRE(dim);
        CHECK(dim->styleScale == Approx(1.27 / 3.5));
        CHECK(dim->measurement() == Approx(50.0));
    }

    SECTION("hole lands on Holes as a drill circle")
    {
        const auto* hole = findEntity<CircleEntity>(doc, [](const CircleEntity& c) {
            return near(c.radius(), 1.6);
        });
        REQUIRE(hole);
        CHECK(layerOf(doc, hole)->name == "Holes");
    }
}

TEST_CASE("EAGLE schematic import: symbols, nets, sheets", "[eagle]")
{
    QTemporaryDir dir;
    const QString path = writeTemp(dir, "test.sch", kSchXml);

    auto result = importEagle(path);
    INFO(result.error.toStdString());
    REQUIRE(result.ok);
    CHECK(result.kind == "schematic");
    CHECK(result.sheets == 2);
    REQUIRE(result.document);
    Document& doc = *result.document;

    SECTION("instance placement rotates the symbol body")
    {
        // Symbol wire (0,0)-(2.54,0) under R90 at (10,10) -> (10,10)-(10,12.54).
        const auto* wire = findEntity<PolylineEntity>(doc, [](const PolylineEntity& p) {
            return p.vertices().size() == 2 && near(p.vertices()[0].pos.x, 10) &&
                   near(p.vertices()[0].pos.y, 10) && near(p.vertices()[1].pos.y, 12.54);
        });
        REQUIRE(wire);
        CHECK(layerOf(doc, wire)->name == "Symbols");
    }

    SECTION("pin draws its line and name")
    {
        // Pin A at local (-2.54, 0), short (2.54), R0 -> line toward +x, under
        // instance R90 -> from (10, 7.46) to (10, 10).
        const auto* pin = findEntity<PolylineEntity>(doc, [](const PolylineEntity& p) {
            return p.vertices().size() == 2 && near(p.vertices()[0].pos.y, 7.46) &&
                   near(p.vertices()[1].pos.y, 10.0);
        });
        REQUIRE(pin);
        const auto* pinName = findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "A";
        });
        CHECK(pinName);
    }

    SECTION(">NAME/>VALUE substitute from the part")
    {
        CHECK(findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "R1";
        }));
        CHECK(findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "4k7";
        }));
    }

    SECTION("net wire, junction dot and net-name label")
    {
        const auto* net = findEntity<PolylineEntity>(doc, [&](const PolylineEntity& p) {
            return near(p.width(), 0.1524) && layerOf(doc, &p)->name == "Nets";
        });
        REQUIRE(net);
        const auto* junction = findEntity<HatchEntity>(doc, [&](const HatchEntity& h) {
            return h.rings.size() == 1 && layerOf(doc, &h)->name == "Nets";
        });
        REQUIRE(junction);
        const auto* label = findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "N$1";
        });
        REQUIRE(label);
    }

    SECTION("second sheet is laid out to the right of the first")
    {
        const auto* un = findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "feuille un";
        });
        const auto* deux = findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == "feuille deux";
        });
        REQUIRE(un);
        REQUIRE(deux);
        CHECK(deux->position().x > un->position().x + 10.0);
        CHECK(near(deux->position().y, un->position().y));
    }
}

TEST_CASE("EAGLE import refusals are explicit", "[eagle]")
{
    QTemporaryDir dir;

    SECTION("binary EAGLE (v5 and older) names the cure")
    {
        QByteArray bin;
        bin.append(char(0x10));
        bin.append(char(0x00));
        bin.append("legacy");
        const QString path = writeTemp(dir, "old.brd", bin);
        CHECK(isEagleBinary(path));
        CHECK_FALSE(isEagleXml(path));
        auto result = importEagle(path);
        CHECK_FALSE(result.ok);
        CHECK(result.error.contains("EAGLE 6+"));
    }

    SECTION("foreign binary (not 0x10) is refused as not-XML")
    {
        QByteArray bin;
        bin.append(char(0x78));
        bin.append(char(0x56));
        bin.append("1\x01garbage");
        const QString path = writeTemp(dir, "tecnove.sch", bin);
        auto result = importEagle(path);
        CHECK_FALSE(result.ok);
        CHECK(result.error.contains("not an EAGLE XML file"));
    }

    SECTION("foreign XML is refused")
    {
        const QString path = writeTemp(dir, "foo.sch", "<?xml version=\"1.0\"?><foo/>");
        CHECK_FALSE(isEagleXml(path));
        auto result = importEagle(path);
        CHECK_FALSE(result.ok);
        CHECK(result.error.contains("not an EAGLE XML file"));
    }

}

TEST_CASE("EAGLE library import: grid of packages and symbols", "[eagle]")
{
    QTemporaryDir dir;
    const char kLbrXml[] = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE eagle SYSTEM "eagle.dtd">
<eagle version="6.5.0">
<drawing>
<layers>
<layer number="1" name="Top" color="4" fill="1" visible="yes" active="yes"/>
<layer number="21" name="tPlace" color="7" fill="1" visible="yes" active="yes"/>
<layer number="25" name="tNames" color="7" fill="1" visible="yes" active="yes"/>
<layer number="94" name="Symbols" color="4" fill="1" visible="yes" active="yes"/>
</layers>
<library>
<packages>
<package name="P1">
<smd name="1" x="0" y="0" dx="2" dy="1" layer="1"/>
<text x="0" y="1.5" size="1.27" layer="25">&gt;NAME</text>
</package>
<package name="P2">
<wire x1="-2" y1="0" x2="2" y2="0" width="0.2" layer="21"/>
</package>
</packages>
<symbols>
<symbol name="RES">
<wire x1="0" y1="0" x2="2.54" y2="0" width="0.254" layer="94"/>
<pin name="A" x="-2.54" y="0" length="short"/>
</symbol>
</symbols>
</library>
</drawing>
</eagle>
)";
    const QString path = writeTemp(dir, "lib.lbr", kLbrXml);

    auto result = importEagle(path);
    INFO(result.error.toStdString());
    REQUIRE(result.ok);
    CHECK(result.kind == "library");
    REQUIRE(result.document);
    Document& doc = *result.document;

    SECTION("every item gets a title on the Labels layer")
    {
        const Layer* labels = doc.layerByName("Labels");
        REQUIRE(labels);
        for (const char* name : {"P1", "P2", "RES"}) {
            const auto* t = findEntity<TextEntity>(doc, [&](const TextEntity& e) {
                return e.text() == QLatin1String(name);
            });
            INFO(name);
            REQUIRE(t);
            CHECK(t->layerId() == labels->id);
        }
    }

    SECTION(">NAME stays literal, like EAGLE's library editor")
    {
        CHECK(findEntity<TextEntity>(doc, [](const TextEntity& t) {
            return t.text() == ">NAME";
        }));
    }

    SECTION("cells do not overlap")
    {
        std::vector<Vec2d> titlePos;
        for (const EntityId id : doc.drawOrder()) {
            const auto* t = dynamic_cast<const TextEntity*>(doc.entity(id));
            if (t && (t->text() == "P1" || t->text() == "P2" || t->text() == "RES"))
                titlePos.push_back(t->position());
        }
        REQUIRE(titlePos.size() == 3);
        for (size_t i = 0; i < titlePos.size(); ++i)
            for (size_t j = i + 1; j < titlePos.size(); ++j)
                CHECK((titlePos[i] - titlePos[j]).length() > 3.0);
    }
}

TEST_CASE("EAGLE library import: real vault library", "[eagle]")
{
    const QString lbr = QStringLiteral(
        "/home/lex/LSB_LexSecondBrain/_4_Archives/50-OldJobs/GUILLAUME_SIMARD/"
        "Aki3/aki-pcb/lib/ALRMP_RLC.lbr");
    if (!QFileInfo::exists(lbr)) {
        SKIP("vault library not present on this machine");
    }
    auto result = importEagle(lbr);
    INFO(result.error.toStdString());
    REQUIRE(result.ok);
    CHECK(result.kind == "library");
    CHECK(result.imported > 50);
    INFO(result.skippedTypes.join(", ").toStdString());
    CHECK(result.document->layerByName("Labels"));
}

TEST_CASE("EAGLE import: real vault files", "[eagle]")
{
    const QString brd = QStringLiteral(
        "/home/lex/LSB_LexSecondBrain/_4_Archives/50-OldJobs/MILKOMAX/"
        "MKM_2HR_encodeur/REV3.brd");
    const QString sch = QStringLiteral(
        "/home/lex/LSB_LexSecondBrain/_4_Archives/50-OldJobs/MILKOMAX/"
        "MKM_2HR_encodeur/REV3.sch");
    if (!QFileInfo::exists(brd) || !QFileInfo::exists(sch)) {
        SKIP("vault sample files not present on this machine");
    }

    SECTION("real board")
    {
        auto result = importEagle(brd);
        INFO(result.error.toStdString());
        REQUIRE(result.ok);
        CHECK(result.kind == "board");
        CHECK(result.imported > 500);
        REQUIRE(result.document);
        CHECK(result.document->layerByName("Top"));
        CHECK(result.document->layerByName("Bottom"));
        INFO(result.skippedTypes.join(", ").toStdString());
        CHECK(result.skipped < 60);
    }

    SECTION("real schematic")
    {
        auto result = importEagle(sch);
        INFO(result.error.toStdString());
        REQUIRE(result.ok);
        CHECK(result.kind == "schematic");
        CHECK(result.sheets == 1);
        CHECK(result.imported > 800);
        INFO(result.skippedTypes.join(", ").toStdString());
        CHECK(result.skipped < 60);
    }
}
