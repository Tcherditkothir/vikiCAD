#pragma once

#include <memory>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

#include "geom/Vec2d.h"
#include "solid/SolidOps.h"

namespace viki {

// ---------------------------------------------------------------------------
// Parametric feature history — the FOUNDATION of a Fusion-style timeline.
//
// This is purely additive groundwork: it does NOT replace SolidEntity or the
// document commands. A FeatureTree owns an ordered list of FeatureNodes and can
// regenerate() a TopoDS_Shape by replaying them from scratch. Editing a node's
// parameter (e.g. an extrude height) and calling regenerate() again yields the
// updated shape — that is the whole point of a parametric history.
//
// A later milestone will wire this into SolidEntity/.vkd; for now it is a real,
// tested data model + replay engine that lives on its own.
// ---------------------------------------------------------------------------

// The kinds of feature a node can be. Kept small on purpose; the model is
// meant to grow node kinds without changing the replay contract.
enum class FeatureKind {
    Sketch,    // a 2D profile on a work plane -> closed wires
    Extrude,   // turns the referenced sketch's wires into a prism
    BaseShape, // a non-parametric starting body (imported STEP, boolean result…)
    Hole,      // parametric bore -> replays solidops::makeHole on the current body
    Shell,     // hollow to a wall thickness -> replays solidops::shellSolid
};

// A 2D profile inside a Sketch feature. Only the shapes the replay engine can
// turn into a closed wire are modelled here. This is deliberately a small,
// explicit description (not a reference into the Document) so the tree is
// self-contained and replayable in isolation.
enum class ProfileKind {
    Rectangle, // axis-aligned rect on the work plane, given by two corners
    Circle,    // circle given by centre + radius
    Polygon,   // closed polygon given by ordered vertices (>= 3)
};

struct SketchProfile {
    ProfileKind kind = ProfileKind::Rectangle;

    // Rectangle: min/max corners (u,v on the work plane).
    Vec2d rectMin;
    Vec2d rectMax;

    // Circle: centre + radius.
    Vec2d circleCenter;
    double circleRadius = 0.0;

    // Polygon: ordered vertices, implicitly closed (last -> first).
    std::vector<Vec2d> polygon;

    static SketchProfile rectangle(const Vec2d& a, const Vec2d& b)
    {
        SketchProfile p;
        p.kind = ProfileKind::Rectangle;
        p.rectMin = {std::min(a.x, b.x), std::min(a.y, b.y)};
        p.rectMax = {std::max(a.x, b.x), std::max(a.y, b.y)};
        return p;
    }
    static SketchProfile circle(const Vec2d& c, double r)
    {
        SketchProfile p;
        p.kind = ProfileKind::Circle;
        p.circleCenter = c;
        p.circleRadius = r;
        return p;
    }
    static SketchProfile polygonFrom(std::vector<Vec2d> verts)
    {
        SketchProfile p;
        p.kind = ProfileKind::Polygon;
        p.polygon = std::move(verts);
        return p;
    }
};

// One feature node. A single struct with a `kind` tag plays the role of the
// FeatureNode variant: only the fields relevant to `kind` are meaningful. This
// keeps the type plainly copyable/serialisable for the .vkd wiring.
struct FeatureNode {
    FeatureKind kind = FeatureKind::Sketch;

    // --- Sketch fields (Hole reuses `plane` as its bore plane) ---
    WorkPlane plane;                      // work plane the profiles live on
    std::vector<SketchProfile> profiles;  // the closed profiles

    // --- Extrude fields ---
    // Index (into the tree's node list) of the Sketch this extrude consumes.
    // A negative value means "the most recent Sketch before this node".
    int sketchIndex = -1;
    double height = 10.0;                  // extrusion height (mm)
    solidops::ExtrudeMode mode = solidops::ExtrudeMode::NewBody;
    // For Join/Cut: index of the node whose regenerated shape is the target.
    // Negative means "the most recent solid-producing node before this one".
    int targetIndex = -1;

    // --- BaseShape fields ---
    // The starting body of a tree whose origin is not (yet) parametric.
    // Serialized as a BinTools base64 blob, exactly like SolidEntity's brep.
    TopoDS_Shape baseShape;

    // --- Hole fields (the bore plane is `plane` above) ---
    Vec2d holeCenter;      // 2D centre on the bore plane
    double diameter = 5.0; // bore diameter (mm)
    double depth = 10.0;   // bore depth (mm); ignored when `through`
    bool through = false;  // true = pierce the whole body

    // --- Shell fields ---
    double thickness = 1.0; // wall thickness (mm), hollowed all-around

    static FeatureNode makeSketch(std::vector<SketchProfile> profiles,
                                  const WorkPlane& plane = {})
    {
        FeatureNode n;
        n.kind = FeatureKind::Sketch;
        n.plane = plane;
        n.profiles = std::move(profiles);
        return n;
    }
    static FeatureNode makeExtrude(double height, int sketchIndex = -1,
                                   solidops::ExtrudeMode mode = solidops::ExtrudeMode::NewBody,
                                   int targetIndex = -1)
    {
        FeatureNode n;
        n.kind = FeatureKind::Extrude;
        n.height = height;
        n.sketchIndex = sketchIndex;
        n.mode = mode;
        n.targetIndex = targetIndex;
        return n;
    }
    static FeatureNode makeBaseShape(const TopoDS_Shape& shape)
    {
        FeatureNode n;
        n.kind = FeatureKind::BaseShape;
        n.baseShape = shape;
        return n;
    }
    static FeatureNode makeHole(const WorkPlane& plane, const Vec2d& center,
                                double diameter, double depth, bool through)
    {
        FeatureNode n;
        n.kind = FeatureKind::Hole;
        n.plane = plane;
        n.holeCenter = center;
        n.diameter = diameter;
        n.depth = depth;
        n.through = through;
        return n;
    }
    static FeatureNode makeShell(double thickness)
    {
        FeatureNode n;
        n.kind = FeatureKind::Shell;
        n.thickness = thickness;
        return n;
    }
};

struct RegenResult {
    bool ok = false;
    QString message;
    TopoDS_Shape shape; // the shape of the LAST solid-producing node
};

// An ordered list of feature nodes with a replay engine.
class FeatureTree {
public:
    // Node management (append/edit/remove). Editing is deliberately direct:
    // callers mutate nodeAt(i) and re-run regenerate().
    int addNode(FeatureNode node);            // returns the node's index
    int count() const { return static_cast<int>(m_nodes.size()); }
    int nodeCount() const { return count(); } // wiring-facing alias
    FeatureNode& nodeAt(int i) { return m_nodes.at(static_cast<size_t>(i)); }
    const FeatureNode& nodeAt(int i) const { return m_nodes.at(static_cast<size_t>(i)); }
    void clear() { m_nodes.clear(); }

    // Convenience parameter setters: each checks the node kind and returns
    // false on a mismatch (callers then re-run regenerate()).
    bool setExtrudeHeight(int i, double height);
    bool setHoleDiameter(int i, double diameter);
    bool setHoleDepth(int i, double depth);
    bool setShellThickness(int i, double thickness);

    // Whole-tree JSON round-trip (every node kind, BaseShape brep as base64).
    // fromJson replaces the tree's contents; false = malformed input (the tree
    // is left empty in that case).
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& obj);

    // Replay every node from scratch and return the shape of the final
    // solid-producing node. Idempotent: calling it repeatedly (or after a
    // parameter edit) always rebuilds from the node list, never from cache.
    RegenResult regenerate() const;

    // Turn a single Sketch node's profiles into closed wires (used internally
    // by regenerate, exposed for tests/debugging).
    static bool wiresForSketch(const FeatureNode& sketch,
                               std::vector<TopoDS_Wire>& out, QString& err);

private:
    std::vector<FeatureNode> m_nodes;
};

} // namespace viki
