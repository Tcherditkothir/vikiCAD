#include "CommandProcessor.h"

#include "solid/SolidEntity.h"
#include "solid/SubShape.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

// INSPECT (alias INS) — the discovery half of the deterministic sub-shape
// addressing scheme. One ctx.info() line per face/edge with its 0-based
// TopExp index, so a headless agent learns exactly which index to feed to
// index-driven solid operations (push/pull, fillet, shell-open, ...):
//   face 3: cylinder r=4.0 area=125.7 centroid=(10.0,5.0,3.0)
//   edge 7: circle r=4.0 len=25.1 mid=(10.0,5.0,3.0)

namespace viki {
namespace {

QString num(double v)
{
    // Fixed one decimal: stable to parse, precise enough to recognise a face.
    return QString::number(v, 'f', 1);
}

QString pnt(const gp_Pnt& p)
{
    return QStringLiteral("(%1,%2,%3)").arg(num(p.X()), num(p.Y()), num(p.Z()));
}

QString faceLine(const TopoDS_Shape& solid, int index)
{
    const TopoDS_Face face = TopoDS::Face(subshape::faceAt(solid, index));
    BRepAdaptor_Surface surf(face);
    QString type;
    switch (surf.GetType()) {
    case GeomAbs_Plane:
        type = QStringLiteral("plane");
        break;
    case GeomAbs_Cylinder:
        type = QStringLiteral("cylinder r=%1").arg(num(surf.Cylinder().Radius()));
        break;
    case GeomAbs_Cone:
        type = QStringLiteral("cone");
        break;
    case GeomAbs_Sphere:
        type = QStringLiteral("sphere r=%1").arg(num(surf.Sphere().Radius()));
        break;
    case GeomAbs_Torus:
        type = QStringLiteral("torus");
        break;
    case GeomAbs_BezierSurface:
    case GeomAbs_BSplineSurface:
        type = QStringLiteral("freeform");
        break;
    default:
        type = QStringLiteral("surface");
        break;
    }
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    return QStringLiteral("face %1: %2 area=%3 centroid=%4")
        .arg(index)
        .arg(type, num(props.Mass()), pnt(props.CentreOfMass()));
}

QString edgeLine(const TopoDS_Shape& solid, int index)
{
    const TopoDS_Edge edge = TopoDS::Edge(subshape::edgeAt(solid, index));
    BRepAdaptor_Curve curve(edge);
    QString type;
    switch (curve.GetType()) {
    case GeomAbs_Line:
        type = QStringLiteral("line");
        break;
    case GeomAbs_Circle:
        type = QStringLiteral("circle r=%1").arg(num(curve.Circle().Radius()));
        break;
    case GeomAbs_Ellipse:
        type = QStringLiteral("ellipse");
        break;
    case GeomAbs_BezierCurve:
    case GeomAbs_BSplineCurve:
        type = QStringLiteral("spline");
        break;
    default:
        type = QStringLiteral("curve");
        break;
    }
    GProp_GProps props;
    BRepGProp::LinearProperties(edge, props);
    const gp_Pnt mid =
        curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2.0);
    return QStringLiteral("edge %1: %2 len=%3 mid=%4")
        .arg(index)
        .arg(type, num(props.Mass()), pnt(mid));
}

class InspectCommand : public Command {
public:
    const char* name() const override { return "INSPECT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Select solid (id):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_id == kInvalidEntityId) {
            EntityId id = kInvalidEntityId;
            if (v.kind == InputValue::Kind::EntitySet && v.entitySet.size() == 1)
                id = v.entitySet[0];
            else if (v.kind == InputValue::Kind::EntityRef)
                id = v.entityRef;
            if (id == kInvalidEntityId)
                return Step::cancelled();
            if (!dynamic_cast<SolidEntity*>(ctx.doc().entity(id))) {
                ctx.info(QStringLiteral("that id is not a solid"));
                return Step::cancelled();
            }
            m_id = id;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("What [Faces/Edges/All] <All>:"));
        }
        // Scope stage. Finish (Enter / lone click) = the All default.
        bool faces = true, edges = true;
        if (v.kind == InputValue::Kind::Keyword) {
            const QString k = v.text; // uppercased by the processor
            if (k == QLatin1String("F") || k == QLatin1String("FACES")) {
                edges = false;
            } else if (k == QLatin1String("E") || k == QLatin1String("EDGES")) {
                faces = false;
            } else if (k != QLatin1String("A") && k != QLatin1String("ALL")) {
                ctx.info(QStringLiteral("expected Faces, Edges or All"));
                return Step::cont(InputKind::Keyword,
                                  QStringLiteral("What [Faces/Edges/All] <All>:"));
            }
        } else if (v.kind != InputValue::Kind::Finish) {
            return Step::cancelled();
        }

        const auto* solid = dynamic_cast<const SolidEntity*>(ctx.doc().entity(m_id));
        if (!solid) // deleted between the two prompts (GUI could)
            return Step::cancelled();
        const TopoDS_Shape& shape = solid->shape();
        const int nf = subshape::faceCount(shape);
        const int ne = subshape::edgeCount(shape);
        ctx.info(QStringLiteral("solid %1: %2 faces, %3 edges")
                     .arg(m_id)
                     .arg(nf)
                     .arg(ne));
        if (faces)
            for (int i = 0; i < nf; ++i)
                ctx.info(faceLine(shape, i));
        if (edges)
            for (int i = 0; i < ne; ++i)
                ctx.info(edgeLine(shape, i));
        return Step::done();
    }

private:
    EntityId m_id = kInvalidEntityId;
};

std::unique_ptr<Command> makeInspect()
{
    return std::make_unique<InspectCommand>();
}

} // namespace

void registerInspectCommands(CommandProcessor& p)
{
    p.registerCommand(&makeInspect, {QStringLiteral("INS")});
}

} // namespace viki
