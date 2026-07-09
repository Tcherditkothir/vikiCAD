#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/ParamTable.h"
#include "io/NativeStore.h"

using namespace viki;
using Catch::Approx;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
};

} // namespace

TEST_CASE("param table evaluates expressions with references", "[params]")
{
    ParamTable t;
    REQUIRE(t.set(QStringLiteral("d"), QStringLiteral("10")));
    REQUIRE(t.set(QStringLiteral("w"), QStringLiteral("2*d")));

    bool ok = false;
    REQUIRE(t.value(QStringLiteral("d"), &ok) == Approx(10.0));
    REQUIRE(ok);
    REQUIRE(t.value(QStringLiteral("w"), &ok) == Approx(20.0));
    REQUIRE(ok);
}

TEST_CASE("changing an input re-evaluates dependents", "[params]")
{
    ParamTable t;
    t.set(QStringLiteral("d"), QStringLiteral("10"));
    t.set(QStringLiteral("w"), QStringLiteral("2*d"));
    REQUIRE(t.value(QStringLiteral("w")) == Approx(20.0));

    // Change d -> w must follow.
    t.set(QStringLiteral("d"), QStringLiteral("15"));
    REQUIRE(t.value(QStringLiteral("d")) == Approx(15.0));
    REQUIRE(t.value(QStringLiteral("w")) == Approx(30.0));
}

TEST_CASE("evaluator precedence, parentheses and unary", "[params]")
{
    ParamTable t;
    t.set(QStringLiteral("a"), QStringLiteral("2"));
    t.set(QStringLiteral("b"), QStringLiteral("3"));
    t.set(QStringLiteral("c"), QStringLiteral("a + b * 4"));      // 14
    t.set(QStringLiteral("e"), QStringLiteral("(a + b) * 4"));    // 20
    t.set(QStringLiteral("f"), QStringLiteral("-a + 10 / a"));    // 3
    t.set(QStringLiteral("g"), QStringLiteral("2 * (a + b) - 1")); // 9

    REQUIRE(t.value(QStringLiteral("c")) == Approx(14.0));
    REQUIRE(t.value(QStringLiteral("e")) == Approx(20.0));
    REQUIRE(t.value(QStringLiteral("f")) == Approx(3.0));
    REQUIRE(t.value(QStringLiteral("g")) == Approx(9.0));
}

TEST_CASE("unknown reference, cycle and syntax mark param invalid", "[params]")
{
    ParamTable t;
    // Unknown reference.
    REQUIRE_FALSE(t.set(QStringLiteral("x"), QStringLiteral("y + 1")));
    bool ok = true;
    (void)t.value(QStringLiteral("x"), &ok);
    REQUIRE_FALSE(ok);

    // Fix it by supplying y; x resolves after re-evaluation.
    t.set(QStringLiteral("y"), QStringLiteral("5"));
    REQUIRE(t.value(QStringLiteral("x"), &ok) == Approx(6.0));
    REQUIRE(ok);

    // Cycle: p -> q -> p.
    ParamTable c;
    c.set(QStringLiteral("p"), QStringLiteral("q + 1"));
    REQUIRE_FALSE(c.set(QStringLiteral("q"), QStringLiteral("p + 1")));
    c.value(QStringLiteral("p"), &ok);
    REQUIRE_FALSE(ok);
    c.value(QStringLiteral("q"), &ok);
    REQUIRE_FALSE(ok);

    // Syntax errors.
    ParamTable s;
    REQUIRE_FALSE(s.set(QStringLiteral("a"), QStringLiteral("1 +")));
    REQUIRE_FALSE(s.set(QStringLiteral("b"), QStringLiteral("(2 + 3")));
    REQUIRE_FALSE(s.set(QStringLiteral("d"), QStringLiteral("1 / 0")));
}

TEST_CASE("rejects invalid parameter names", "[params]")
{
    ParamTable t;
    REQUIRE_FALSE(ParamTable::isValidName(QStringLiteral("2d")));
    REQUIRE_FALSE(ParamTable::isValidName(QStringLiteral("a b")));
    REQUIRE_FALSE(ParamTable::isValidName(QString()));
    REQUIRE(ParamTable::isValidName(QStringLiteral("_w2")));
    REQUIRE_FALSE(t.set(QStringLiteral("2d"), QStringLiteral("1")));
    REQUIRE(t.empty());
}

TEST_CASE("PARAM command drives the document table", "[params][commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("PARAM d 10"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("PARAM w 2*d"), true).ok);

    bool ok = false;
    REQUIRE(rig.doc.params().value(QStringLiteral("w"), &ok) == Approx(20.0));
    REQUIRE(ok);

    // Redefining d re-evaluates w.
    REQUIRE(rig.processor.submit(QStringLiteral("PARAM d 21"), true).ok);
    REQUIRE(rig.doc.params().value(QStringLiteral("w")) == Approx(42.0));
}

TEST_CASE("params persist through save/load", "[params][store]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("p.vkd"));

    Document doc;
    doc.params().set(QStringLiteral("d"), QStringLiteral("10"));
    doc.params().set(QStringLiteral("w"), QStringLiteral("2*d"));

    QString error;
    REQUIRE(NativeStore::save(doc, path, error));

    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded);
    REQUIRE(loaded->params().size() == 2);
    REQUIRE(loaded->params().value(QStringLiteral("w")) == Approx(20.0));

    // Expression source survives, so changing d after load still recomputes w.
    loaded->params().set(QStringLiteral("d"), QStringLiteral("50"));
    REQUIRE(loaded->params().value(QStringLiteral("w")) == Approx(100.0));
}
