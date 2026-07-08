#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Annotations.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "io/DxfExporter.h"
#include "io/DxfImporter.h"

using namespace viki;
using Catch::Approx;

TEST_CASE("DXF export -> reimport preserves geometry semantically", "[dxf][golden]")
{
    Document doc;
    SelectionSet sel;
    CommandContext ctx(doc, sel);
    CommandProcessor proc(ctx);
    registerBuiltinCommands(proc);

    const LayerId walls = doc.ensureLayer(QStringLiteral("walls"), 0xFF0000);
    doc.setCurrentLayer(walls);
    REQUIRE(proc.submit(QStringLiteral("LINE 0,0 100,50"), true).ok);
    doc.setCurrentLayer(0);
    REQUIRE(proc.submit(QStringLiteral("CIRCLE 10,20 5"), true).ok);
    REQUIRE(proc.submit(QStringLiteral("ARC 30,0 35,5 40,0"), true).ok);
    REQUIRE(proc.submit(QStringLiteral("RECT 50,50 80,90"), true).ok);
    REQUIRE(proc.submit(QStringLiteral("ELLIPSE 0,100 30,100 0.5"), true).ok);
    REQUIRE(proc.submit(QStringLiteral("SPLINE 0,0 10,15 25,-5 40,10"), true).ok);
    REQUIRE(proc.submit(QStringLiteral("POINT 5,5"), true).ok);
    REQUIRE(proc.submit(QStringLiteral("XLINE 0,0 10,10"), true).ok);

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("rt.dxf"));

    for (const char* version : {"2013", "R12", "2018"}) {
        DYNAMIC_SECTION("version " << version)
        {
            const bool r12 = QLatin1String(version) == QLatin1String("R12");
            const DxfExportResult ex = exportDxf(doc, path, QLatin1String(version));
            REQUIRE(ex.ok);
            REQUIRE(ex.exported == 8);
            REQUIRE(ex.skipped == 0);

            const DxfImportResult im = importDxf(path);
            REQUIRE(im.ok);
            // R12: the spline is dropped (no SPLINE record before R13) and
            // the ellipse comes back as a flattened polyline.
            REQUIRE(im.imported == (r12 ? 7 : 8));

            Document& d2 = *im.document;
            // Layer round-trip (true color preserved via color24).
            Layer* w2 = d2.layerByName(QStringLiteral("walls"));
            REQUIRE(w2);

            // Match entities by type and compare key geometry.
            const auto findFirst = [&](const char* type) -> const Entity* {
                for (const EntityId id : d2.drawOrder())
                    if (QLatin1String(d2.entity(id)->typeName()) == QLatin1String(type))
                        return d2.entity(id);
                return nullptr;
            };
            const auto* line = dynamic_cast<const LineEntity*>(findFirst("line"));
            REQUIRE(line);
            REQUIRE(line->p2().x == Approx(100.0));
            REQUIRE(line->layerId() == w2->id);

            const auto* circle = dynamic_cast<const CircleEntity*>(findFirst("circle"));
            REQUIRE(circle);
            REQUIRE(circle->radius() == Approx(5.0));

            const auto* arc = dynamic_cast<const ArcEntity*>(findFirst("arc"));
            REQUIRE(arc);
            REQUIRE(arc->radius() == Approx(5.0).margin(1e-6));

            const auto* pl = dynamic_cast<const PolylineEntity*>(findFirst("polyline"));
            REQUIRE(pl);
            REQUIRE(pl->isClosed());
            REQUIRE(pl->bounds().width() == Approx(30.0));

            if (!r12) {
                const auto* el = dynamic_cast<const EllipseEntity*>(findFirst("ellipse"));
                REQUIRE(el);
                REQUIRE(el->majorAxis().length() == Approx(30.0));
                REQUIRE(el->ratio() == Approx(0.5));

                const auto* sp = dynamic_cast<const SplineEntity*>(findFirst("spline"));
                REQUIRE(sp);
                REQUIRE(sp->fitPoints.size() == 4);
            }

            REQUIRE(findFirst("point"));
            REQUIRE(findFirst("xline"));
        }
    }
}

TEST_CASE("DXF text alignment round trip", "[dxf][golden][mtext]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("t"));
    {
        // Multiline, middle-center: exports as MTEXT attachment 5.
        auto t = std::make_unique<TextEntity>(
            Vec2d{100, 50}, 2.5, 0.3, QStringLiteral("first\nsecond"));
        t->hAlign = TextHAlign::Center;
        t->vAlign = TextVAlign::Middle;
        t->lineSpacing = 2.0;
        doc.addEntity(std::move(t));
    }
    {
        // Multiline, baseline-left: MTEXT has no baseline attachment, so the
        // exporter lifts the anchor one cap height and writes a Top row.
        doc.addEntity(std::make_unique<TextEntity>(
            Vec2d{0, 0}, 5.0, 0.0, QStringLiteral("base\nline")));
    }
    {
        // Single line, right-aligned: exports as TEXT with codes 72/11.
        auto t = std::make_unique<TextEntity>(
            Vec2d{20, -30}, 3.5, 0.0, QStringLiteral("right"));
        t->hAlign = TextHAlign::Right;
        doc.addEntity(std::move(t));
    }
    doc.commitTransaction();

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("text-rt.dxf"));
    REQUIRE(exportDxf(doc, path).ok);
    const DxfImportResult im = importDxf(path);
    REQUIRE(im.ok);
    REQUIRE(im.imported == 3);
    Document& d2 = *im.document;

    std::vector<const TextEntity*> texts;
    for (const EntityId id : d2.drawOrder())
        if (const auto* t = dynamic_cast<const TextEntity*>(d2.entity(id)))
            texts.push_back(t);
    REQUIRE(texts.size() == 3);

    const TextEntity* mid = texts[0];
    CHECK(mid->text() == QStringLiteral("first\nsecond"));
    CHECK(mid->position().x == Approx(100.0));
    CHECK(mid->position().y == Approx(50.0));
    CHECK(mid->hAlign == TextHAlign::Center);
    CHECK(mid->vAlign == TextVAlign::Middle);
    CHECK(mid->lineSpacing == Approx(2.0));
    CHECK(mid->rotation() == Approx(0.3));

    // Baseline case: semantics become Top but the RENDERED first baseline
    // must land where the original one was (y = 0).
    const TextEntity* base = texts[1];
    CHECK(base->text() == QStringLiteral("base\nline"));
    CHECK(base->vAlign == TextVAlign::Top);
    const double y0 = TextEntity::firstBaselineY(
        base->vAlign, base->height(), base->lineSpacing, 2);
    CHECK(base->position().y + y0 == Approx(0.0).margin(1e-9));

    const TextEntity* right = texts[2];
    CHECK(right->hAlign == TextHAlign::Right);
    CHECK(right->position().x == Approx(20.0));
    CHECK(right->position().y == Approx(-30.0));
}
