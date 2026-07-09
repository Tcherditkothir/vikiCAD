#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QJsonDocument>
#include <QTemporaryDir>

#include "doc/Entities.h"
#include "io/NativeStore.h"
#include "solid/SolidOps.h"

using namespace viki;
using Catch::Approx;

TEST_CASE("native store save/load round-trip", "[store]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("t.vkd"));

    Document doc;
    doc.setDisplayUnits(DisplayUnits::Inches);
    doc.beginTransaction(QStringLiteral("draw"));
    const EntityId lineId =
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{100, 50}));
    const EntityId circleId =
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{10, 20}, 7.5));
    doc.addEntity(std::make_unique<ArcEntity>(Vec2d{0, 0}, 5.0, 0.0, M_PI_2));
    doc.commitTransaction();

    QString error;
    REQUIRE(NativeStore::save(doc, path, error));

    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded);
    REQUIRE(loaded->entityCount() == 3);
    REQUIRE(loaded->displayUnits() == DisplayUnits::Inches);
    REQUIRE(loaded->drawOrder() == doc.drawOrder()); // ids and order preserved

    // Byte-identical JSON per entity.
    for (const EntityId id : doc.drawOrder()) {
        const auto a = QJsonDocument(doc.entity(id)->toJson()).toJson(QJsonDocument::Compact);
        const auto b =
            QJsonDocument(loaded->entity(id)->toJson()).toJson(QJsonDocument::Compact);
        REQUIRE(a == b);
    }

    // New entities after load must not collide with existing ids.
    loaded->beginTransaction(QStringLiteral("more"));
    const EntityId next = loaded->addEntity(std::make_unique<CircleEntity>(Vec2d{}, 1.0));
    loaded->commitTransaction();
    REQUIRE(next > circleId);
    REQUIRE(next > lineId);
}

TEST_CASE("native store persists the work plane", "[store]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("wp.vkd"));

    // A non-default, non-axis-aligned frame so a silent fallback to world XY
    // could not accidentally pass. origin offset, normal +xDir orthonormal.
    const WorkPlane want{gp_Pnt(12.5, -3.0, 100.0),
                         gp_Dir(0.0, 0.0, 1.0),
                         gp_Dir(0.6, 0.8, 0.0)};

    {
        Document doc;
        documentWorkplane(doc) = want;
        QString error;
        REQUIRE(NativeStore::save(doc, path, error));
    }

    QString error;
    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded);
    const WorkPlane& got = documentWorkplane(*loaded);

    REQUIRE(got.origin.X() == Approx(want.origin.X()).margin(1e-9));
    REQUIRE(got.origin.Y() == Approx(want.origin.Y()).margin(1e-9));
    REQUIRE(got.origin.Z() == Approx(want.origin.Z()).margin(1e-9));
    REQUIRE(got.normal.X() == Approx(want.normal.X()).margin(1e-9));
    REQUIRE(got.normal.Y() == Approx(want.normal.Y()).margin(1e-9));
    REQUIRE(got.normal.Z() == Approx(want.normal.Z()).margin(1e-9));
    REQUIRE(got.xDir.X() == Approx(want.xDir.X()).margin(1e-9));
    REQUIRE(got.xDir.Y() == Approx(want.xDir.Y()).margin(1e-9));
    REQUIRE(got.xDir.Z() == Approx(want.xDir.Z()).margin(1e-9));
}

TEST_CASE("load rejects a non-vkd file", "[store]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("garbage.vkd"));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("this is not sqlite");
    f.close();

    QString error;
    REQUIRE(NativeStore::load(path, error) == nullptr);
    REQUIRE_FALSE(error.isEmpty());
}
