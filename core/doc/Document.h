#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QJsonObject>
#include <QString>

#include "Block.h"
#include "DimStyle.h"
#include "Layout.h"
#include "Entity.h"
#include "Layer.h"

namespace viki {

enum class DisplayUnits { Millimeters, Inches };

// One recorded mutation inside a transaction. Inverses are replayed on undo.
struct Change {
    enum class Kind { Added, Removed, Modified };
    Kind kind;
    EntityId id;
    QJsonObject before; // Removed / Modified
    QJsonObject after;  // Added / Modified
};

struct Transaction {
    QString name;
    std::vector<Change> changes;
};

// The drawing database. ALL mutations go through addEntity / removeEntity /
// beginModify+endModify so the undo journal and change notifications are
// recorded at a single choke point — commands never write undo logic.
class Document {
public:
    Document();

    // --- entity access
    Entity* entity(EntityId id);
    const Entity* entity(EntityId id) const;
    // Draw order (insertion order). Iterate this for rendering and queries.
    const std::vector<EntityId>& drawOrder() const { return m_drawOrder; }
    size_t entityCount() const { return m_entities.size(); }
    BBox2d extents() const;
    // Insert-aware bounds (block references expand their definition).
    BBox2d entityBounds(const Entity& e) const;

    // --- mutations (require an open transaction)
    EntityId addEntity(std::unique_ptr<Entity> entity);
    bool removeEntity(EntityId id);
    // Capture-before / capture-after pair around any in-place edit.
    Entity* beginModify(EntityId id);
    void endModify(EntityId id);

    // --- transactions / undo
    void beginTransaction(const QString& name);
    void commitTransaction();
    void rollbackTransaction();
    bool inTransaction() const { return m_openTransaction != nullptr; }
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }
    // Return the name of the undone/redone transaction, empty if none.
    QString undo();
    QString redo();

    // --- layers
    const std::vector<Layer>& layers() const { return m_layers; }
    const Layer* layer(LayerId id) const;
    Layer* layerByName(const QString& name);
    // Direct (non-journaled) layer creation/update — import, load, manager.
    LayerId ensureLayer(const QString& name, uint32_t rgb = 0xFFFFFF,
                        bool visible = true, bool locked = false);
    void setLayerProps(LayerId id, uint32_t rgb, bool visible, bool locked);
    void setLayerPrintable(LayerId id, bool printable);
    // Deletes an empty, non-current layer. False if it has entities/is current/is 0.
    bool removeLayer(LayerId id);
    LayerId currentLayer() const { return m_currentLayer; }
    void setCurrentLayer(LayerId id) { m_currentLayer = id; }
    uint32_t resolveColor(const Entity& e) const;

    // --- blocks (definition edits are direct/not journaled — v1 choice)
    const std::vector<std::unique_ptr<BlockDef>>& blocks() const { return m_blocks; }
    const BlockDef* blockByName(const QString& name) const;
    BlockDef* blockByName(const QString& name);
    const BlockDef* blockById(int64_t id) const;
    // Creates (or returns) a definition; entities are moved in directly.
    BlockDef* createBlock(const QString& name, const Vec2d& basePoint);

    // --- layouts (paper space; direct edits — v1 choice)
    const std::vector<Layout>& layouts() const { return m_layouts; }
    Layout* layoutByName(const QString& name);
    Layout* ensureLayout(const QString& name);

    // --- dimension styles (direct edits, not journaled — v1 choice)
    const DimStyle& dimStyle(const QString& name) const;
    DimStyle& currentDimStyle();
    const std::vector<DimStyle>& dimStyles() const { return m_dimStyles; }
    void upsertDimStyle(const DimStyle& style);

    // --- settings
    DisplayUnits displayUnits() const { return m_displayUnits; }
    void setDisplayUnits(DisplayUnits u) { m_displayUnits = u; }
    QString filePath() const { return m_filePath; }
    void setFilePath(const QString& p) { m_filePath = p; }

    // --- extra snap points (transient reference targets, e.g. the sketch-on-
    // face outline). Not journaled, not persisted; SnapEngine also considers
    // them so the profile can snap to real face features. Cleared when the
    // sketch reference is dropped.
    const std::vector<SnapPoint>& extraSnapPoints() const { return m_extraSnapPoints; }
    void setExtraSnapPoints(std::vector<SnapPoint> pts) { m_extraSnapPoints = std::move(pts); }
    void clearExtraSnapPoints() { m_extraSnapPoints.clear(); }

    // Change notification (any entity/undo mutation). GUI repaints on this.
    void addChangeListener(std::function<void()> fn) { m_listeners.push_back(std::move(fn)); }

    // Used by NativeStore on load: insert with a known id, no transaction.
    void restoreEntity(std::unique_ptr<Entity> entity, EntityId id);
    // Load path: restore a layer with its stored id (id 0 updates layer "0").
    void restoreLayer(const Layer& l);
    void setNextId(EntityId next) { m_nextId = next; }
    EntityId nextId() const { return m_nextId; }

private:
    void applyInverse(const Change& change);
    void applyForward(const Change& change);
    void notifyChanged();

    std::unordered_map<EntityId, std::unique_ptr<Entity>> m_entities;
    std::vector<EntityId> m_drawOrder;
    EntityId m_nextId = 1;
    std::vector<SnapPoint> m_extraSnapPoints; // transient reference snap targets

    std::unique_ptr<Transaction> m_openTransaction;
    QJsonObject m_modifyBefore;
    std::vector<Transaction> m_undoStack;
    std::vector<Transaction> m_redoStack;
    static constexpr size_t kMaxUndo = 100;

    std::vector<Layer> m_layers;
    std::vector<std::unique_ptr<BlockDef>> m_blocks;
    int64_t m_nextBlockId = 1;
    std::vector<Layout> m_layouts;
    int64_t m_nextLayoutId = 1;
    std::vector<DimStyle> m_dimStyles{DimStyle{}};
    LayerId m_currentLayer = 0;
    LayerId m_nextLayerId = 1;
    DisplayUnits m_displayUnits = DisplayUnits::Millimeters;
    QString m_filePath;
    std::vector<std::function<void()>> m_listeners;
};

} // namespace viki
