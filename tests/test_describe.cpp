#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "cmd/CommandProcessor.h"
#include "io/QueryJson.h"
#include "solid/SolidEntity.h"

// describe-model, both halves:
//  - DESCRIBE (text): computed document/solid/feature/sketch/layer lines;
//  - queryjson::describeJson (machine): same data, numbers as numbers,
//    and NO brep base64 anywhere.

using namespace viki;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

EntityId firstSolidId(const Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (dynamic_cast<const SolidEntity*>(doc.entity(id)))
            return id;
    return kInvalidEntityId;
}

bool anyContains(const std::vector<QString>& msgs, const QString& needle)
{
    for (const QString& m : msgs)
        if (m.contains(needle))
            return true;
    return false;
}

// 20x20x10 box with a d=4 through-hole at (10,10):
// volume = 4000 - pi * 2^2 * 10 = 4000 - pi*4*10.
void buildBoxWithHole(Rig& rig)
{
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const EntityId boxId = firstSolidId(rig.doc);
    REQUIRE(boxId != kInvalidEntityId);
    REQUIRE(rig.run(QStringLiteral("HOLE 4 T 10,10 %1").arg(boxId)));
}

const double kExpectedVolume = 4000.0 - M_PI * 4.0 * 10.0;

} // namespace

TEST_CASE("DESCRIBE prints the computed document/solid/feature lines",
          "[describe]")
{
    Rig rig;
    buildBoxWithHole(rig);
    const EntityId id = firstSolidId(rig.doc);

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("DESCRIBE")));
    const auto& msgs = rig.ctx.messages();

    // Document line: units, entity count, layer count.
    CHECK(anyContains(msgs, QStringLiteral("document: units=mm entities=1 layers=1")));

    // Solid line carries the boolean-accurate volume at one decimal.
    const QString volumeNeedle = QStringLiteral("solid %1: volume=%2 mm3")
                                     .arg(id)
                                     .arg(QString::number(kExpectedVolume, 'f', 1));
    CHECK(anyContains(msgs, volumeNeedle));
    CHECK(anyContains(msgs, QStringLiteral("bbox=(0.0,0.0,0.0)-(20.0,20.0,10.0)")));
    CHECK(anyContains(msgs, QStringLiteral("centroid=(10.0,10.0,5.0)")));

    // Feature history, featureparams-style labels (indented).
    CHECK(anyContains(msgs, QStringLiteral("  base 0")));
    CHECK(anyContains(msgs, QStringLiteral("  hole 1 d=4 through @(10,10)")));

    // Layer inventory (the box's profile was consumed by EXTRUDE).
    CHECK(anyContains(msgs, QStringLiteral("layer '0': 2d=0")));
}

TEST_CASE("DESCRIBE with a solid id scopes to that solid; DESC is an alias",
          "[describe]")
{
    Rig rig;
    buildBoxWithHole(rig);
    const EntityId id = firstSolidId(rig.doc);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 50,50 5"))); // 2D noise

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("DESC %1").arg(id)));
    const auto& msgs = rig.ctx.messages();
    CHECK(anyContains(msgs, QStringLiteral("solid %1:").arg(id)));
    CHECK(anyContains(msgs, QStringLiteral("  hole 1 d=4 through @(10,10)")));
    // Scoped output: no layer inventory lines.
    CHECK_FALSE(anyContains(msgs, QStringLiteral("layer '0'")));

    // A non-solid id reports instead of describing.
    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("DESCRIBE 1")));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("not a solid")));
}

TEST_CASE("DESCRIBE lists sketches with plane and entity count", "[describe]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW lid")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("DESCRIBE")));
    const auto& msgs = rig.ctx.messages();
    CHECK(anyContains(msgs, QStringLiteral(
        "sketch 1 'lid': origin=(0.0,0.0,0.0) normal=(0.0,0.0,1.0) entities=1")));
    CHECK(anyContains(msgs, QStringLiteral("layer '0': 2d=1 polyline=1")));
}

TEST_CASE("query describe JSON: numbers as numbers, no brep anywhere",
          "[describe]")
{
    Rig rig;
    buildBoxWithHole(rig);
    const EntityId id = firstSolidId(rig.doc);

    const QJsonObject d = queryjson::describeJson(rig.doc);

    CHECK(d[QStringLiteral("units")].toString() == QStringLiteral("mm"));
    CHECK(d[QStringLiteral("entityCount")].toInt() == 1);
    CHECK(d[QStringLiteral("layerCount")].toInt() == 1);

    const QJsonArray solids = d[QStringLiteral("solids")].toArray();
    REQUIRE(solids.size() == 1);
    const QJsonObject s = solids[0].toObject();
    CHECK(s[QStringLiteral("id")].toInteger() == qint64(id));
    CHECK(s[QStringLiteral("volume")].toDouble() ==
          Catch::Approx(kExpectedVolume).margin(1e-3));
    CHECK(s[QStringLiteral("area")].toDouble() > 0.0);
    const QJsonObject bbox = s[QStringLiteral("bbox")].toObject();
    const QJsonArray bmin = bbox[QStringLiteral("min")].toArray();
    const QJsonArray bmax = bbox[QStringLiteral("max")].toArray();
    REQUIRE(bmin.size() == 3);
    REQUIRE(bmax.size() == 3);
    CHECK(bmax[0].toDouble() == Catch::Approx(20.0).margin(1e-6));
    CHECK(bmax[2].toDouble() == Catch::Approx(10.0).margin(1e-6));
    const QJsonArray centroid = s[QStringLiteral("centroid")].toArray();
    REQUIRE(centroid.size() == 3);
    CHECK(centroid[2].toDouble() == Catch::Approx(5.0).margin(1e-6));

    // features[0] IS the hole (param-less base/sketch nodes are skipped),
    // params flattened by name.
    const QJsonArray features = s[QStringLiteral("features")].toArray();
    REQUIRE(features.size() == 1);
    const QJsonObject hole = features[0].toObject();
    CHECK(hole[QStringLiteral("kind")].toString() == QStringLiteral("hole"));
    CHECK(hole[QStringLiteral("diameter")].toDouble() == Catch::Approx(4.0));
    CHECK(hole[QStringLiteral("through")].toBool());
    const QJsonArray center = hole[QStringLiteral("center")].toArray();
    REQUIRE(center.size() == 2);
    CHECK(center[0].toDouble() == Catch::Approx(10.0));

    // NO brep key anywhere in the whole payload.
    const QByteArray raw =
        QJsonDocument(d).toJson(QJsonDocument::Compact);
    CHECK_FALSE(raw.contains("\"brep\""));
    CHECK_FALSE(raw.contains("brep"));
}

TEST_CASE("query describe JSON covers sketches and per-layer counts",
          "[describe]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW lid")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.run(QStringLiteral("CIRCLE 30,30 5")));

    const QJsonObject d = queryjson::describeJson(rig.doc);

    const QJsonArray sketches = d[QStringLiteral("sketches")].toArray();
    REQUIRE(sketches.size() == 1);
    const QJsonObject sk = sketches[0].toObject();
    CHECK(sk[QStringLiteral("name")].toString() == QStringLiteral("lid"));
    CHECK(sk[QStringLiteral("entityCount")].toInt() == 1);
    REQUIRE(sk[QStringLiteral("normal")].toArray().size() == 3);
    CHECK(sk[QStringLiteral("normal")].toArray()[2].toDouble() ==
          Catch::Approx(1.0));

    const QJsonArray layers = d[QStringLiteral("layers")].toArray();
    REQUIRE(layers.size() == 1);
    const QJsonObject layer = layers[0].toObject();
    CHECK(layer[QStringLiteral("name")].toString() == QStringLiteral("0"));
    CHECK(layer[QStringLiteral("count")].toInt() == 2);
    const QJsonObject counts = layer[QStringLiteral("counts")].toObject();
    CHECK(counts[QStringLiteral("circle")].toInt() == 1);
    CHECK(counts[QStringLiteral("polyline")].toInt() == 1);
}
