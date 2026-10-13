#pragma once

#include "orderbook/Types.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace ob {

using NodeIndex = std::uint32_t;
constexpr NodeIndex kNullNode = std::numeric_limits<NodeIndex>::max();

// An order plus the intrusive links that thread it into its price level's queue.
struct OrderNode {
    Order order;
    NodeIndex prev = kNullNode;
    NodeIndex next = kNullNode;
};

// Slab of order nodes addressed by index. Released slots go on a free list and
// are reused, so after warm-up adding an order performs no heap allocation.
// Indices (not pointers) are handed out because the slab may grow.
class OrderPool {
public:
    void reserve(std::size_t n) { nodes_.reserve(n); }

    NodeIndex allocate(const Order& order) {
        NodeIndex i;
        if (freeHead_ != kNullNode) {
            i = freeHead_;
            freeHead_ = nodes_[i].next;
            nodes_[i] = OrderNode{order, kNullNode, kNullNode};
        } else {
            i = static_cast<NodeIndex>(nodes_.size());
            nodes_.push_back(OrderNode{order, kNullNode, kNullNode});
        }
        ++live_;
        return i;
    }

    void release(NodeIndex i) {
        nodes_[i].next = freeHead_;
        freeHead_ = i;
        --live_;
    }

    OrderNode& operator[](NodeIndex i) { return nodes_[i]; }
    const OrderNode& operator[](NodeIndex i) const { return nodes_[i]; }

    std::size_t live() const noexcept { return live_; }
    std::size_t capacity() const noexcept { return nodes_.size(); }

private:
    std::vector<OrderNode> nodes_;
    NodeIndex freeHead_ = kNullNode;
    std::size_t live_ = 0;
};

} // namespace ob
