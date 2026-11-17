#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Z-DON: a map from a small non-negative int (a note's index) to T, stored in a vector: setting an entry
// allocates nothing once reserve() covered the index (an unordered_map allocated a node per judged note),
// and a lookup is an array read. A negative index is ignored by set and absent for get.
template<class T>
class IndexMap {
public:
    void reserve(int n) {
        if (n > static_cast<int>(slots.size())) {
            slots.resize(static_cast<size_t>(n));
            present.resize(static_cast<size_t>(n), false);
            order.reserve(static_cast<size_t>(n));
        }
    }
    void clear() {
        // keeps the room: a retry plays the same chart
        for (int i : order) {
            present[static_cast<size_t>(i)] = false;
        }
        order.clear();
    }
    void set(int i, const T& v) {
        if (i < 0) {
            return;
        }
        if (i >= static_cast<int>(slots.size())) {
            reserve(std::max(i + 1, static_cast<int>(slots.size()) * 2));
        }
        slots[static_cast<size_t>(i)] = v;
        if (!present[static_cast<size_t>(i)]) {
            present[static_cast<size_t>(i)] = true;
            order.push_back(i);
        }
    }
    const T* get(int i) const {
        return i >= 0 && i < static_cast<int>(slots.size()) && present[static_cast<size_t>(i)]
                   ? &slots[static_cast<size_t>(i)]
                   : nullptr;
    }
    size_t size() const { return order.size(); }
    // f(index, value) for every entry, in the order they were first set
    template<class F>
    void for_each(F&& f) const {
        for (int i : order) {
            f(i, slots[static_cast<size_t>(i)]);
        }
    }

private:
    std::vector<T> slots;
    std::vector<bool> present;
    std::vector<int> order;
};
