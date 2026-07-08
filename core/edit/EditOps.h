#pragma once

#include <optional>
#include <vector>

#include "doc/Document.h"

namespace viki {

// Editing operations shared by the interactive commands. All functions
// mutate the document THROUGH ITS OPEN TRANSACTION (callers begin/commit).
// v1 scope: TRIM/EXTEND/BREAK/FILLET/CHAMFER operate on Line, Circle and
// Arc targets (EXPLODE a polyline first); any curve type can be a boundary.
namespace editops {

struct OpResult {
    bool ok = false;
    QString message;
};

// Intersection points of `entity` with each entity in `others`.
std::vector<Vec2d> intersectionsWith(const Document& doc, EntityId entity,
                                     const std::vector<EntityId>& others);

// Removes the portion of `target` around `pick`, bounded by the nearest
// intersections with `cutters`. Cutters = empty means every other entity.
OpResult trimEntity(Document& doc, EntityId target, std::vector<EntityId> cutters,
                    const Vec2d& pick);

// Extends the end of `target` nearest to `pick` up to the closest boundary.
OpResult extendEntity(Document& doc, EntityId target, std::vector<EntityId> boundaries,
                      const Vec2d& pick);

// Parallel copy at `distance`, on the side of `sidePick`. Returns new id.
OpResult offsetEntity(Document& doc, EntityId source, double distance,
                      const Vec2d& sidePick);

// The offset geometry alone (no style, not added to the document) — shared by
// offsetEntity and the OFFSET command's live preview. Null on failure; if
// `err` is non-null it receives the reason.
std::unique_ptr<Entity> offsetGeometry(const Document& doc, EntityId source,
                                       double distance, const Vec2d& sidePick,
                                       QString* err = nullptr);

// Fillet (radius >= 0) between two lines; radius 0 = sharp corner.
// pick points choose which portions are kept.
OpResult filletLines(Document& doc, EntityId line1, const Vec2d& pick1, EntityId line2,
                     const Vec2d& pick2, double radius);

// Chamfer between two lines with distances d1 (on line1) and d2.
OpResult chamferLines(Document& doc, EntityId line1, const Vec2d& pick1, EntityId line2,
                      const Vec2d& pick2, double d1, double d2);

// Removes the portion of `target` between p1 and p2 (projected onto it).
// p1 == p2 splits in two without removing material.
OpResult breakEntity(Document& doc, EntityId target, const Vec2d& p1, const Vec2d& p2);

// Joins collinear lines / cocircular arcs / chained lines+arcs into a
// polyline. Consumes the inputs it managed to join.
OpResult joinEntities(Document& doc, const std::vector<EntityId>& ids);

// Explodes polylines into lines and arcs. Returns count in message.
OpResult explodeEntities(Document& doc, const std::vector<EntityId>& ids);

// Stretch: vertices inside `window` move by `delta`; entities fully inside
// move whole.
OpResult stretchEntities(Document& doc, const std::vector<EntityId>& ids,
                         const BBox2d& window, const Vec2d& delta);

} // namespace editops
} // namespace viki
