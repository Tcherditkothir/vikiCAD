#include <catch2/catch_test_macros.hpp>
#include <set>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QFile>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"
#include "io/StepIo.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Matchers::WithinAbs;

namespace {
struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

// A 10x10x10 box at (x0,y0). EXTRUDE's second argument is the PROFILE ID, so
// the rectangle's id has to be read back rather than assumed (same helper shape
// as test_edgeops_mate_draft.cpp).
SolidEntity* buildBox(Rig& rig, double x0 = 0.0, double y0 = 0.0)
{
    REQUIRE(rig.run(QStringLiteral("RECT %1,%2 %3,%4")
                        .arg(x0)
                        .arg(y0)
                        .arg(x0 + 10.0)
                        .arg(y0 + 10.0)));
    EntityId rectId = kInvalidEntityId;
    for (const EntityId id : rig.doc.drawOrder())
        rectId = std::max(rectId, id);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(rectId)));

    SolidEntity* last = nullptr;
    for (const EntityId id : rig.doc.drawOrder())
        if (auto* s = dynamic_cast<SolidEntity*>(rig.doc.entity(id)))
            last = s;
    return last;
}

std::vector<const SolidEntity*> solidsOf(const Document& doc)
{
    std::vector<const SolidEntity*> out;
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            out.push_back(s);
    return out;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}
} // namespace

TEST_CASE("STEP carries colour, transparency and part name both ways",
          "[step][color]")
{
    Rig rig;
    SolidEntity* box = buildBox(rig);
    REQUIRE(box != nullptr);
    box->setColor(ColorSpec{/*byLayer=*/false, 0xE8AD23}); // amber
    box->transparency = 0.25;
    box->component = QStringLiteral("BOUCLIER");

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("coloured.step"));
    const StepResult w = exportStep(rig.doc, path);
    REQUIRE(w.ok);
    CHECK(w.solids == 1);

    // In the FILE, not merely surviving in memory: a plain STEPControl_Writer
    // emits neither of these entities.
    const QByteArray text = readAll(path);
    CHECK(text.contains("COLOUR_RGB"));
    CHECK(text.contains("STYLED_ITEM"));
    CHECK(text.contains("BOUCLIER"));

    std::unique_ptr<Document> back;
    const StepResult r = importStep(path, back);
    REQUIRE(r.ok);
    REQUIRE(back != nullptr);
    CHECK(r.colored == 1);
    CHECK(r.named == 1);

    const auto solids = solidsOf(*back);
    REQUIRE(solids.size() == 1);
    CHECK_FALSE(solids[0]->color().byLayer);
    // Exact equality: toRgb24 and fromRgb24 both tag sRGB, so the 8-bit value
    // must round-trip untouched. A mismatched colour space shows up here as a
    // washed-out or darkened channel.
    CHECK(solids[0]->color().rgb == 0xE8AD23u);
    CHECK_THAT(solids[0]->transparency, WithinAbs(0.25, 0.01));
    CHECK(solids[0]->component == QStringLiteral("BOUCLIER"));
}

TEST_CASE("a ByLayer solid stays unstyled instead of baking in a layer colour",
          "[step][color]")
{
    Rig rig;
    SolidEntity* box = buildBox(rig);
    REQUIRE(box != nullptr);
    REQUIRE(box->color().byLayer); // the default

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("plain.step"));
    REQUIRE(exportStep(rig.doc, path).ok);

    std::unique_ptr<Document> back;
    const StepResult r = importStep(path, back);
    REQUIRE(r.ok);
    // Nothing invented on either side: the count reports the file is colourless,
    // which is what tells "the STEP has no colour" apart from "we lost it".
    CHECK(r.colored == 0);
    const auto solids = solidsOf(*back);
    REQUIRE(solids.size() == 1);
    CHECK(solids[0]->color().byLayer);
}

TEST_CASE("distinct per-solid colours stay distinct through a STEP round-trip",
          "[step][color]")
{
    // The real-world shape of the problem: SolidWorks styles each body
    // separately (STYLED_ITEM -> MANIFOLD_SOLID_BREP), so a file holds several
    // colours that must not collapse into one.
    Rig rig;
    SolidEntity* first = buildBox(rig, 0.0, 0.0);
    REQUIRE(first != nullptr);
    first->setColor(ColorSpec{false, 0xA0A0A0});
    first->component = QStringLiteral("SHIELD");

    SolidEntity* second = buildBox(rig, 20.0, 0.0);
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    second->setColor(ColorSpec{false, 0x449648});
    second->component = QStringLiteral("LeftLight");

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("two.step"));
    REQUIRE(exportStep(rig.doc, path).ok);

    std::unique_ptr<Document> back;
    const StepResult r = importStep(path, back);
    REQUIRE(r.ok);
    CHECK(r.colored == 2);

    std::set<uint32_t> palette;
    std::set<QString> names;
    for (const SolidEntity* s : solidsOf(*back)) {
        palette.insert(s->color().rgb);
        names.insert(s->component);
    }
    CHECK(palette == std::set<uint32_t>{0xA0A0A0u, 0x449648u});
    CHECK(names == std::set<QString>{QStringLiteral("SHIELD"),
                                    QStringLiteral("LeftLight")});
}
