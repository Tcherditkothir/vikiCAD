// G2 layer stack: per-layer alpha (compositing opacity), paint rank
// (render order), reassignable Gerber role — model semantics, .vkd
// round-trip, and the LAYER / BOARDVIEW commands (headless parity).

#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "doc/GerberRole.h"
#include "io/NativeStore.h"

using namespace viki;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
};

const Layer* layerNamed(const Document& doc, const QString& name)
{
    for (const Layer& l : doc.layers())
        if (l.name == name)
            return &l;
    return nullptr;
}

} // namespace

TEST_CASE("layer defaults: opaque, rank 0, no role", "[layers][g2]")
{
    Document doc;
    const Layer* l0 = doc.layer(0);
    REQUIRE(l0);
    REQUIRE(l0->alpha == 100);
    REQUIRE(l0->rank == 0);
    REQUIRE(l0->gerberRole.isEmpty());
}

TEST_CASE("setLayerAlpha clamps to 0..100", "[layers][g2]")
{
    Document doc;
    const LayerId id = doc.ensureLayer(QStringLiteral("L1"));
    doc.setLayerAlpha(id, 240);
    REQUIRE(doc.layer(id)->alpha == 100);
    doc.setLayerAlpha(id, -3);
    REQUIRE(doc.layer(id)->alpha == 0);
    doc.setLayerAlpha(id, 55);
    REQUIRE(doc.layer(id)->alpha == 55);
}

TEST_CASE("layersByPaintOrder: stable sort by rank", "[layers][g2]")
{
    Document doc;
    const LayerId a = doc.ensureLayer(QStringLiteral("A"));
    const LayerId b = doc.ensureLayer(QStringLiteral("B"));
    const LayerId c = doc.ensureLayer(QStringLiteral("C"));

    // All rank 0: table order (0, A, B, C) — the legacy behavior.
    auto order = doc.layersByPaintOrder();
    REQUIRE(order.size() == 4);
    REQUIRE(order[1]->id == a);
    REQUIRE(order[2]->id == b);
    REQUIRE(order[3]->id == c);

    // Explicit ranks reorder; ties keep table order (stable).
    doc.setLayerRank(a, 10);
    doc.setLayerRank(b, 5);
    order = doc.layersByPaintOrder();
    REQUIRE(order[0]->name == QStringLiteral("0")); // rank 0
    REQUIRE(order[1]->id == c);                     // rank 0, after "0"
    REQUIRE(order[2]->id == b);                     // rank 5
    REQUIRE(order[3]->id == a);                     // rank 10
}

TEST_CASE("moveLayerPaintOrder materializes ranks and swaps", "[layers][g2]")
{
    Document doc;
    const LayerId a = doc.ensureLayer(QStringLiteral("A"));
    const LayerId b = doc.ensureLayer(QStringLiteral("B"));

    // Up = painted later (on top). Initial paint order: 0, A, B.
    REQUIRE(doc.moveLayerPaintOrder(a, +1)); // 0, B, A
    auto order = doc.layersByPaintOrder();
    REQUIRE(order[1]->id == b);
    REQUIRE(order[2]->id == a);
    // Ranks materialized 0..n-1.
    REQUIRE(order[0]->rank == 0);
    REQUIRE(order[1]->rank == 1);
    REQUIRE(order[2]->rank == 2);

    // Already on top: refused.
    REQUIRE_FALSE(doc.moveLayerPaintOrder(a, +1));
    // Bottom edge: layer 0 cannot go below the bottom.
    REQUIRE_FALSE(doc.moveLayerPaintOrder(0, -1));
    // Down moves back.
    REQUIRE(doc.moveLayerPaintOrder(a, -1)); // 0, A, B
    order = doc.layersByPaintOrder();
    REQUIRE(order[1]->id == a);
}

TEST_CASE("applyGerberRole recolors and reranks; None only clears",
          "[layers][g2]")
{
    Document doc;
    const LayerId id = doc.ensureLayer(QStringLiteral("Mech-7"), 0x8FA3B0);

    // The outline-election escape hatch: reassign Mech-7 as the real contour.
    REQUIRE(applyGerberRole(doc, id, QStringLiteral("outline"))); // ci match
    const Layer* l = doc.layer(id);
    REQUIRE(l->gerberRole == QStringLiteral("Outline"));
    REQUIRE(l->rgb == 0xFF00FF); // outline palette (magenta)
    REQUIRE(l->rank == 90);      // paints on top

    // None clears the role but keeps color and rank untouched.
    REQUIRE(applyGerberRole(doc, id, QStringLiteral("None")));
    l = doc.layer(id);
    REQUIRE(l->gerberRole.isEmpty());
    REQUIRE(l->rgb == 0xFF00FF);
    REQUIRE(l->rank == 90);

    REQUIRE_FALSE(applyGerberRole(doc, id, QStringLiteral("NotARole")));
    REQUIRE_FALSE(applyGerberRole(doc, 12345, QStringLiteral("Outline")));
}

TEST_CASE("gerberRoleForLayerName maps the importer names", "[layers][g2]")
{
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Top-Copper")) ==
            QStringLiteral("Copper-Top"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Bottom-Copper")) ==
            QStringLiteral("Copper-Bottom"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Top-Mask")) ==
            QStringLiteral("Mask"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Bottom-Silk-2")) ==
            QStringLiteral("Silk"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Drill-NPTH")) ==
            QStringLiteral("Drill"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Mech-15")) ==
            QStringLiteral("Mech"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Outline")) ==
            QStringLiteral("Outline"));
    REQUIRE(gerberRoleForLayerName(QStringLiteral("Keepout")).isEmpty());
    REQUIRE(gerberRoleForLayerName(QStringLiteral("0")).isEmpty());
}

TEST_CASE("alpha/rank/role survive the .vkd round-trip", "[layers][g2][store]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("stack.vkd"));

    Document doc;
    const LayerId cu = doc.ensureLayer(QStringLiteral("Top-Copper"), 0xE53935);
    const LayerId ol = doc.ensureLayer(QStringLiteral("Outline"), 0xFF00FF);
    doc.setLayerAlpha(cu, 25);
    doc.setLayerRank(cu, 20);
    doc.setLayerGerberRole(cu, QStringLiteral("Copper-Top"));
    doc.setLayerRank(ol, 90);
    doc.setLayerGerberRole(ol, QStringLiteral("Outline"));

    QString error;
    REQUIRE(NativeStore::save(doc, path, error));
    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded);

    const Layer* lcu = layerNamed(*loaded, QStringLiteral("Top-Copper"));
    REQUIRE(lcu);
    REQUIRE(lcu->alpha == 25);
    REQUIRE(lcu->rank == 20);
    REQUIRE(lcu->gerberRole == QStringLiteral("Copper-Top"));
    const Layer* lol = layerNamed(*loaded, QStringLiteral("Outline"));
    REQUIRE(lol);
    REQUIRE(lol->alpha == 100); // untouched default
    REQUIRE(lol->rank == 90);
    REQUIRE(lol->gerberRole == QStringLiteral("Outline"));

    // Saving AGAIN over the same file (the ALTER-in-place path) still works.
    loaded->setLayerAlpha(lcu->id, 60);
    REQUIRE(NativeStore::save(*loaded, path, error));
    const auto again = NativeStore::load(path, error);
    REQUIRE(again);
    REQUIRE(layerNamed(*again, QStringLiteral("Top-Copper"))->alpha == 60);
}

TEST_CASE("LAYER command: ALPHA / RANK / ROLE / UP / DOWN", "[commands][g2]")
{
    Rig rig;
    rig.doc.ensureLayer(QStringLiteral("Top-Copper"));
    rig.doc.ensureLayer(QStringLiteral("Drill"));

    // Name matching is case-insensitive (the tokenizer uppercases keywords).
    REQUIRE(rig.processor.submit(QStringLiteral("LAYER top-copper ALPHA 30"), true).ok);
    REQUIRE(layerNamed(rig.doc, QStringLiteral("Top-Copper"))->alpha == 30);

    // Clamped like the Document setter.
    REQUIRE(rig.processor.submit(QStringLiteral("LAYER Top-Copper ALPHA 250"), true).ok);
    REQUIRE(layerNamed(rig.doc, QStringLiteral("Top-Copper"))->alpha == 100);

    REQUIRE(rig.processor.submit(QStringLiteral("LAYER Top-Copper RANK 42"), true).ok);
    REQUIRE(layerNamed(rig.doc, QStringLiteral("Top-Copper"))->rank == 42);

    REQUIRE(rig.processor.submit(QStringLiteral("LAYER Drill ROLE Drill"), true).ok);
    const Layer* drill = layerNamed(rig.doc, QStringLiteral("Drill"));
    REQUIRE(drill->gerberRole == QStringLiteral("Drill"));
    REQUIRE(drill->rank == 95);
    REQUIRE(drill->rgb == 0x000000);

    REQUIRE(rig.processor.submit(QStringLiteral("LAYER Drill ROLE None"), true).ok);
    REQUIRE(layerNamed(rig.doc, QStringLiteral("Drill"))->gerberRole.isEmpty());

    // UP/DOWN move within the materialized stack.
    Rig rig2;
    rig2.doc.ensureLayer(QStringLiteral("A"));
    rig2.doc.ensureLayer(QStringLiteral("B"));
    REQUIRE(rig2.processor.submit(QStringLiteral("LAYER A UP"), true).ok);
    auto order = rig2.doc.layersByPaintOrder();
    REQUIRE(order[1]->name == QStringLiteral("B"));
    REQUIRE(order[2]->name == QStringLiteral("A"));
    REQUIRE(rig2.processor.submit(QStringLiteral("LAYER A DOWN"), true).ok);
    order = rig2.doc.layersByPaintOrder();
    REQUIRE(order[1]->name == QStringLiteral("A"));

    // Unknown layer: friendly message, no crash, command over.
    const auto r = rig.processor.submit(QStringLiteral("LAYER Nope ALPHA 10"), true);
    REQUIRE(r.ok);
    REQUIRE_FALSE(rig.processor.hasActiveCommand());
}

TEST_CASE("BOARDVIEW presets drive layer alpha by side", "[commands][g2]")
{
    Rig rig;
    Document& doc = rig.doc;
    const LayerId top = doc.ensureLayer(QStringLiteral("Top-Copper"));
    const LayerId bot = doc.ensureLayer(QStringLiteral("Bottom-Copper"));
    const LayerId tsk = doc.ensureLayer(QStringLiteral("Top-Silk"));
    const LayerId bsk = doc.ensureLayer(QStringLiteral("Bottom-Silk"));
    const LayerId out = doc.ensureLayer(QStringLiteral("Outline"));
    const LayerId drl = doc.ensureLayer(QStringLiteral("Drill"));
    doc.setLayerGerberRole(top, QStringLiteral("Copper-Top"));
    doc.setLayerGerberRole(bot, QStringLiteral("Copper-Bottom"));
    doc.setLayerGerberRole(out, QStringLiteral("Outline"));
    doc.setLayerGerberRole(drl, QStringLiteral("Drill"));

    REQUIRE(rig.processor.submit(QStringLiteral("BOARDVIEW TOP"), true).ok);
    REQUIRE(doc.layer(top)->alpha == 100);
    REQUIRE(doc.layer(tsk)->alpha == 100); // sideless role, name says Top
    REQUIRE(doc.layer(bot)->alpha == 25);
    REQUIRE(doc.layer(bsk)->alpha == 25);
    REQUIRE(doc.layer(out)->alpha == 100); // contour always visible
    REQUIRE(doc.layer(drl)->alpha == 100); // drills always visible

    REQUIRE(rig.processor.submit(QStringLiteral("BOARDVIEW BOTTOM"), true).ok);
    REQUIRE(doc.layer(top)->alpha == 25);
    REQUIRE(doc.layer(tsk)->alpha == 25);
    REQUIRE(doc.layer(bot)->alpha == 100);
    REQUIRE(doc.layer(bsk)->alpha == 100);
    REQUIRE(doc.layer(out)->alpha == 100);
    REQUIRE(doc.layer(drl)->alpha == 100);

    // ALL restores every alpha (the "identical to initial" contract).
    REQUIRE(rig.processor.submit(QStringLiteral("BOARDVIEW ALL"), true).ok);
    for (const Layer& l : doc.layers())
        REQUIRE(l.alpha == 100);

    // Bare BOARDVIEW = the ALL default (implicit Finish in strict mode).
    REQUIRE(rig.processor.submit(QStringLiteral("BOARDVIEW TOP"), true).ok);
    REQUIRE(doc.layer(bot)->alpha == 25);
    REQUIRE(rig.processor.submit(QStringLiteral("BOARDVIEW"), true).ok);
    REQUIRE(doc.layer(bot)->alpha == 100);
}

TEST_CASE("BOARDVIEW drives the view mirror hook", "[commands][g2]")
{
    struct MirrorHook : ViewHook {
        void zoomExtents() override {}
        void setMirroredX(bool on) override { mirrored = on; }
        bool mirroredX() const override { return mirrored; }
        bool mirrored = false;
    };
    Document doc;
    SelectionSet sel;
    MirrorHook hook;
    CommandContext ctx{doc, sel, &hook};
    CommandProcessor processor{ctx};
    registerBuiltinCommands(processor);

    REQUIRE(processor.submit(QStringLiteral("BOARDVIEW BOTTOM"), true).ok);
    REQUIRE(hook.mirrored);
    REQUIRE(processor.submit(QStringLiteral("BOARDVIEW TOP"), true).ok);
    REQUIRE_FALSE(hook.mirrored);
    REQUIRE(processor.submit(QStringLiteral("BOARDVIEW BOTTOM"), true).ok);
    REQUIRE(processor.submit(QStringLiteral("BOARDVIEW ALL"), true).ok);
    REQUIRE_FALSE(hook.mirrored);
}
