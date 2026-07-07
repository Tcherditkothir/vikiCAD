#pragma once

#include <algorithm>
#include <vector>

#include "Entity.h"

namespace viki {

// Ordered set of selected entity ids.
class SelectionSet {
public:
    const std::vector<EntityId>& ids() const { return m_ids; }
    bool isEmpty() const { return m_ids.empty(); }
    size_t size() const { return m_ids.size(); }
    bool contains(EntityId id) const
    {
        return std::find(m_ids.begin(), m_ids.end(), id) != m_ids.end();
    }
    void add(EntityId id)
    {
        if (!contains(id))
            m_ids.push_back(id);
    }
    void remove(EntityId id)
    {
        m_ids.erase(std::remove(m_ids.begin(), m_ids.end(), id), m_ids.end());
    }
    void toggle(EntityId id)
    {
        if (contains(id))
            remove(id);
        else
            m_ids.push_back(id);
    }
    void clear() { m_ids.clear(); }

private:
    std::vector<EntityId> m_ids;
};

} // namespace viki
