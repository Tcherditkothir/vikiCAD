#include "Document.h"

#include <algorithm>
#include <cassert>

#include "EntityFactory.h"

namespace viki {

Document::Document()
{
    Layer layer0;
    layer0.id = 0;
    layer0.name = QStringLiteral("0");
    layer0.rgb = 0xFFFFFF;
    m_layers.push_back(layer0);
}

Entity* Document::entity(EntityId id)
{
    const auto it = m_entities.find(id);
    return it == m_entities.end() ? nullptr : it->second.get();
}

const Entity* Document::entity(EntityId id) const
{
    const auto it = m_entities.find(id);
    return it == m_entities.end() ? nullptr : it->second.get();
}

BBox2d Document::extents() const
{
    BBox2d box;
    for (const auto& [id, e] : m_entities)
        if (!e->isInfinite())
            box.expand(entityBounds(*e));
    return box;
}

EntityId Document::addEntity(std::unique_ptr<Entity> entity)
{
    assert(m_openTransaction && "addEntity requires an open transaction");
    const EntityId id = m_nextId++;
    entity->m_id = id;
    // Commands leave the layer at 0; new entities land on the current layer.
    if (entity->layerId() == 0 && m_currentLayer != 0)
        entity->setLayerId(m_currentLayer);

    Change change;
    change.kind = Change::Kind::Added;
    change.id = id;
    change.after = entity->toJson();
    m_openTransaction->changes.push_back(std::move(change));

    m_entities.emplace(id, std::move(entity));
    m_drawOrder.push_back(id);
    // While a sketch is open, everything drawn belongs to it. This is THE
    // single tagging choke point — commands never tag entities themselves.
    if (m_activeSketch != 0)
        m_sketchMembership[id] = m_activeSketch;
    return id;
}

bool Document::removeEntity(EntityId id)
{
    assert(m_openTransaction && "removeEntity requires an open transaction");
    const auto it = m_entities.find(id);
    if (it == m_entities.end())
        return false;

    Change change;
    change.kind = Change::Kind::Removed;
    change.id = id;
    change.before = it->second->toJson();
    m_openTransaction->changes.push_back(std::move(change));

    m_entities.erase(it);
    m_drawOrder.erase(std::remove(m_drawOrder.begin(), m_drawOrder.end(), id),
                      m_drawOrder.end());
    return true;
}

Entity* Document::beginModify(EntityId id)
{
    assert(m_openTransaction && "beginModify requires an open transaction");
    Entity* e = entity(id);
    if (e)
        m_modifyBefore = e->toJson();
    return e;
}

void Document::endModify(EntityId id)
{
    Entity* e = entity(id);
    if (!e || !m_openTransaction)
        return;
    Change change;
    change.kind = Change::Kind::Modified;
    change.id = id;
    change.before = m_modifyBefore;
    change.after = e->toJson();
    m_openTransaction->changes.push_back(std::move(change));
}

void Document::beginTransaction(const QString& name)
{
    assert(!m_openTransaction && "nested transactions are not supported");
    m_openTransaction = std::make_unique<Transaction>();
    m_openTransaction->name = name;
}

void Document::commitTransaction()
{
    if (!m_openTransaction)
        return;
    if (!m_openTransaction->changes.empty()) {
        m_undoStack.push_back(std::move(*m_openTransaction));
        if (m_undoStack.size() > kMaxUndo)
            m_undoStack.erase(m_undoStack.begin());
        m_redoStack.clear();
    }
    m_openTransaction.reset();
    notifyChanged();
}

void Document::rollbackTransaction()
{
    if (!m_openTransaction)
        return;
    // Walk back the recorded changes in reverse.
    for (auto it = m_openTransaction->changes.rbegin();
         it != m_openTransaction->changes.rend(); ++it)
        applyInverse(*it);
    m_openTransaction.reset();
    notifyChanged();
}

QString Document::undo()
{
    if (m_undoStack.empty() || m_openTransaction)
        return {};
    Transaction tx = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    for (auto it = tx.changes.rbegin(); it != tx.changes.rend(); ++it)
        applyInverse(*it);
    const QString name = tx.name;
    m_redoStack.push_back(std::move(tx));
    notifyChanged();
    return name;
}

QString Document::redo()
{
    if (m_redoStack.empty() || m_openTransaction)
        return {};
    Transaction tx = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    for (const Change& change : tx.changes)
        applyForward(change);
    const QString name = tx.name;
    m_undoStack.push_back(std::move(tx));
    notifyChanged();
    return name;
}

void Document::applyInverse(const Change& change)
{
    switch (change.kind) {
    case Change::Kind::Added: {
        m_entities.erase(change.id);
        m_drawOrder.erase(std::remove(m_drawOrder.begin(), m_drawOrder.end(), change.id),
                          m_drawOrder.end());
        break;
    }
    case Change::Kind::Removed: {
        auto e = entityFromJson(change.before);
        if (e) {
            e->m_id = change.id;
            m_entities.emplace(change.id, std::move(e));
            m_drawOrder.push_back(change.id);
        }
        break;
    }
    case Change::Kind::Modified: {
        Entity* e = entity(change.id);
        if (e)
            e->fromJson(change.before);
        break;
    }
    }
}

void Document::applyForward(const Change& change)
{
    switch (change.kind) {
    case Change::Kind::Added: {
        auto e = entityFromJson(change.after);
        if (e) {
            e->m_id = change.id;
            m_entities.emplace(change.id, std::move(e));
            m_drawOrder.push_back(change.id);
        }
        break;
    }
    case Change::Kind::Removed: {
        m_entities.erase(change.id);
        m_drawOrder.erase(std::remove(m_drawOrder.begin(), m_drawOrder.end(), change.id),
                          m_drawOrder.end());
        break;
    }
    case Change::Kind::Modified: {
        Entity* e = entity(change.id);
        if (e)
            e->fromJson(change.after);
        break;
    }
    }
}

const Layer* Document::layer(LayerId id) const
{
    for (const Layer& l : m_layers)
        if (l.id == id)
            return &l;
    return nullptr;
}

Layer* Document::layerByName(const QString& name)
{
    for (Layer& l : m_layers)
        if (l.name.compare(name, Qt::CaseInsensitive) == 0)
            return &l;
    return nullptr;
}

LayerId Document::ensureLayer(const QString& name, uint32_t rgb, bool visible, bool locked)
{
    if (Layer* existing = layerByName(name))
        return existing->id;
    Layer l;
    l.id = m_nextLayerId++;
    l.name = name;
    l.rgb = rgb;
    l.visible = visible;
    l.locked = locked;
    m_layers.push_back(l);
    notifyChanged();
    return l.id;
}

void Document::setLayerProps(LayerId id, uint32_t rgb, bool visible, bool locked)
{
    for (Layer& l : m_layers) {
        if (l.id == id) {
            l.rgb = rgb;
            l.visible = visible;
            l.locked = locked;
            notifyChanged();
            return;
        }
    }
}

void Document::setLayerPrintable(LayerId id, bool printable)
{
    for (Layer& l : m_layers)
        if (l.id == id)
            l.printable = printable;
}

bool Document::removeLayer(LayerId id)
{
    if (id == 0 || id == m_currentLayer)
        return false;
    for (const auto& [eid, e] : m_entities)
        if (e->layerId() == id)
            return false;
    m_layers.erase(std::remove_if(m_layers.begin(), m_layers.end(),
                                  [id](const Layer& l) { return l.id == id; }),
                   m_layers.end());
    notifyChanged();
    return true;
}

BBox2d Document::entityBounds(const Entity& e) const
{
    const auto* ins = dynamic_cast<const InsertEntity*>(&e);
    if (!ins)
        return e.bounds();
    const BlockDef* def = blockByName(ins->blockName);
    if (!def)
        return e.bounds();
    const Xform2d xf = ins->insertXform(def->basePoint);
    BBox2d box;
    for (const auto& sub : def->entities) {
        auto ghost = sub->clone();
        ghost->transform(xf);
        box.expand(ghost->bounds());
    }
    return box.isValid() ? box : e.bounds();
}

const BlockDef* Document::blockByName(const QString& name) const
{
    for (const auto& b : m_blocks)
        if (b->name.compare(name, Qt::CaseInsensitive) == 0)
            return b.get();
    return nullptr;
}

BlockDef* Document::blockByName(const QString& name)
{
    for (auto& b : m_blocks)
        if (b->name.compare(name, Qt::CaseInsensitive) == 0)
            return b.get();
    return nullptr;
}

const BlockDef* Document::blockById(int64_t id) const
{
    for (const auto& b : m_blocks)
        if (b->id == id)
            return b.get();
    return nullptr;
}

BlockDef* Document::createBlock(const QString& name, const Vec2d& basePoint)
{
    if (BlockDef* existing = blockByName(name))
        return existing;
    auto def = std::make_unique<BlockDef>();
    def->id = m_nextBlockId++;
    def->name = name;
    def->basePoint = basePoint;
    m_blocks.push_back(std::move(def));
    notifyChanged();
    return m_blocks.back().get();
}

Layout* Document::layoutByName(const QString& name)
{
    for (Layout& l : m_layouts)
        if (l.name.compare(name, Qt::CaseInsensitive) == 0)
            return &l;
    return nullptr;
}

Layout* Document::ensureLayout(const QString& name)
{
    if (Layout* existing = layoutByName(name))
        return existing;
    Layout l;
    l.id = m_nextLayoutId++;
    l.name = name;
    m_layouts.push_back(l);
    notifyChanged();
    return &m_layouts.back();
}

const DimStyle& Document::dimStyle(const QString& name) const
{
    for (const DimStyle& s : m_dimStyles)
        if (s.name.compare(name, Qt::CaseInsensitive) == 0)
            return s;
    return m_dimStyles.front();
}

DimStyle& Document::currentDimStyle()
{
    return m_dimStyles.front(); // "Standard" is always index 0 in v1
}

void Document::upsertDimStyle(const DimStyle& style)
{
    for (DimStyle& s : m_dimStyles) {
        if (s.name.compare(style.name, Qt::CaseInsensitive) == 0) {
            s = style;
            notifyChanged();
            return;
        }
    }
    m_dimStyles.push_back(style);
    notifyChanged();
}

uint32_t Document::resolveColor(const Entity& e) const
{
    if (!e.color().byLayer)
        return e.color().rgb;
    const Layer* l = layer(e.layerId());
    return l ? l->rgb : 0xFFFFFF;
}

void Document::restoreLayer(const Layer& l)
{
    for (Layer& existing : m_layers) {
        if (existing.id == l.id) {
            existing = l;
            return;
        }
    }
    m_layers.push_back(l);
    m_nextLayerId = std::max(m_nextLayerId, l.id + 1);
}

void Document::restoreEntity(std::unique_ptr<Entity> entity, EntityId id)
{
    entity->m_id = id;
    m_entities.emplace(id, std::move(entity));
    m_drawOrder.push_back(id);
    m_nextId = std::max(m_nextId, id + 1);
}

const SketchInfo* Document::sketchById(int64_t id) const
{
    for (const SketchInfo& s : m_sketches)
        if (s.id == id)
            return &s;
    return nullptr;
}

const SketchInfo* Document::sketchByName(const QString& name) const
{
    for (const SketchInfo& s : m_sketches)
        if (s.name.compare(name, Qt::CaseInsensitive) == 0)
            return &s;
    return nullptr;
}

int64_t Document::createSketch(const QString& name, const WorkPlane& plane)
{
    if (sketchByName(name))
        return 0; // names are unique (0 = refused, ids start at 1)
    SketchInfo info;
    info.id = m_nextSketchId++;
    info.name = name;
    info.plane = plane;
    m_sketches.push_back(info);
    notifyChanged();
    return info.id;
}

bool Document::renameSketch(int64_t id, const QString& name)
{
    if (name.trimmed().isEmpty())
        return false;
    const SketchInfo* clash = sketchByName(name);
    if (clash && clash->id != id)
        return false;
    for (SketchInfo& s : m_sketches) {
        if (s.id == id) {
            s.name = name.trimmed();
            notifyChanged();
            return true;
        }
    }
    return false;
}

bool Document::removeSketch(int64_t id)
{
    const auto it = std::find_if(m_sketches.begin(), m_sketches.end(),
                                 [id](const SketchInfo& s) { return s.id == id; });
    if (it == m_sketches.end())
        return false;
    m_sketches.erase(it);
    // Keep the entities, drop their tags ("delete = registry entry + untag").
    for (auto mit = m_sketchMembership.begin(); mit != m_sketchMembership.end();)
        mit = (mit->second == id) ? m_sketchMembership.erase(mit) : std::next(mit);
    if (m_activeSketch == id)
        m_activeSketch = 0;
    notifyChanged();
    return true;
}

void Document::setActiveSketch(int64_t id)
{
    if (m_activeSketch == id)
        return;
    m_activeSketch = id;
    notifyChanged();
}

int64_t Document::entitySketch(EntityId id) const
{
    const auto it = m_sketchMembership.find(id);
    return it == m_sketchMembership.end() ? 0 : it->second;
}

void Document::setEntitySketch(EntityId id, int64_t sketchId)
{
    if (sketchId == 0)
        m_sketchMembership.erase(id);
    else
        m_sketchMembership[id] = sketchId;
}

std::vector<EntityId> Document::sketchEntities(int64_t sketchId) const
{
    std::vector<EntityId> ids;
    for (const EntityId id : m_drawOrder)
        if (entitySketch(id) == sketchId)
            ids.push_back(id);
    return ids;
}

void Document::restoreSketch(const SketchInfo& info)
{
    m_sketches.push_back(info);
    m_nextSketchId = std::max(m_nextSketchId, info.id + 1);
}

void Document::notifyChanged()
{
    for (const auto& fn : m_listeners)
        fn();
}

} // namespace viki
