#pragma once

#include "orderbook/OrderPool.h"
#include "orderbook/Types.h"

#include <cstdint>

namespace ob {

// All resting orders at a single price, in arrival (FIFO) order, stored as an
// intrusive doubly linked list of nodes that live in a shared OrderPool.
// Keeps a running total so depth queries don't have to walk the queue.
//
// The level does not own its nodes, so every operation takes the pool.
class PriceLevel {
public:
    using Handle = NodeIndex; // stable until the order is erased

    Handle push(OrderPool& pool, const Order& o) {
        const NodeIndex i = pool.allocate(o);
        pool[i].prev = tail_;
        if (tail_ != kNullNode)
            pool[tail_].next = i;
        else
            head_ = i;
        tail_ = i;
        totalQty_ += o.qty;
        ++count_;
        return i;
    }

    void erase(OrderPool& pool, Handle h) {
        OrderNode& n = pool[h];
        if (n.prev != kNullNode)
            pool[n.prev].next = n.next;
        else
            head_ = n.next;
        if (n.next != kNullNode)
            pool[n.next].prev = n.prev;
        else
            tail_ = n.prev;
        totalQty_ -= n.order.qty;
        --count_;
        pool.release(h);
    }

    // Reduce an order's remaining quantity in place (keeps its queue position).
    void reduce(OrderPool& pool, Handle h, Qty by) {
        pool[h].order.qty -= by;
        totalQty_ -= by;
    }

    Handle frontHandle() const noexcept { return head_; }
    Handle next(const OrderPool& pool, Handle h) const { return pool[h].next; }

    bool empty() const noexcept { return head_ == kNullNode; }
    std::size_t orderCount() const noexcept { return count_; }
    std::uint64_t totalQty() const noexcept { return totalQty_; }

private:
    NodeIndex head_ = kNullNode;
    NodeIndex tail_ = kNullNode;
    std::uint64_t totalQty_ = 0;
    std::size_t count_ = 0;
};

} // namespace ob
