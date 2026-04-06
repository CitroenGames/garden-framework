#pragma once

#include <vector>
#include <functional>
#include <cstdint>

class VkDeletionQueue {
public:
    void push(std::function<void()> deleter, uint32_t frameDelay = 3) {
        entries_.push_back({ std::move(deleter), frameDelay });
    }

    void flush() {
        size_t i = 0;
        while (i < entries_.size()) {
            if (entries_[i].frames_remaining == 0) {
                entries_[i].deleter();
                entries_[i] = std::move(entries_.back());
                entries_.pop_back();
            } else {
                entries_[i].frames_remaining--;
                i++;
            }
        }
    }

    void flushAll() {
        for (auto& entry : entries_) {
            entry.deleter();
        }
        entries_.clear();
    }

private:
    struct DeletionEntry {
        std::function<void()> deleter;
        uint32_t frames_remaining;
    };

    std::vector<DeletionEntry> entries_;
};
