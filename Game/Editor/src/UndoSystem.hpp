#pragma once

#include "LevelManager.hpp"
#include <vector>
#include <string>
#include <functional>

class UndoSystem
{
public:
    void clear()
    {
        m_history.clear();
        m_current = -1;
    }

    // Push a snapshot before a mutation. Truncates any redo history.
    void pushState(LevelData snapshot, const std::string& description = "")
    {
        // Truncate redo history
        if (m_current + 1 < static_cast<int>(m_history.size()))
            m_history.erase(m_history.begin() + m_current + 1, m_history.end());

        m_history.push_back({std::move(snapshot), description});
        m_current = static_cast<int>(m_history.size()) - 1;

        // Enforce max history size
        if (static_cast<int>(m_history.size()) > MAX_HISTORY)
        {
            m_history.erase(m_history.begin());
            m_current--;
        }
    }

    // Convenience: only snapshot if no snapshot was taken this frame (debounce for drags).
    // builder is called lazily to avoid building LevelData when not needed.
    void snapshotIfNeeded(const std::function<LevelData()>& builder)
    {
        if (!m_snapshot_taken_this_frame)
        {
            pushState(builder(), "edit");
            m_snapshot_taken_this_frame = true;
        }
    }

    // Call once per frame to reset the debounce flag.
    void beginFrame() { m_snapshot_taken_this_frame = false; }

    bool canUndo() const { return m_current > 0; }
    bool canRedo() const { return m_current + 1 < static_cast<int>(m_history.size()); }

    const LevelData& undo()
    {
        m_current--;
        return m_history[m_current].data;
    }

    const LevelData& redo()
    {
        m_current++;
        return m_history[m_current].data;
    }

private:
    struct Entry
    {
        LevelData data;
        std::string description;
    };

    std::vector<Entry> m_history;
    int m_current = -1;
    bool m_snapshot_taken_this_frame = false;

    static constexpr int MAX_HISTORY = 50;
};
