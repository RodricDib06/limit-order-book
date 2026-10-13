#pragma once

#include "orderbook/Types.h"

#include <cstdint>
#include <list>

namespace ob {

// All resting orders at a single price, in arrival (FIFO) order.
// Keeps a running total so depth queries don't have to walk the queue.
class PriceLevel {
public:
    using Handle = std::list<Order>::iterator; // stable until the order is erased

    Handle push(const Order& o) {
        totalQty_ += o.qty;
        return orders_.insert(orders_.end(), o);
    }

    void erase(Handle h) {
        totalQty_ -= h->qty;
        orders_.erase(h);
    }

    // Reduce an order's remaining quantity in place (keeps its queue position).
    void reduce(Handle h, Qty by) {
        h->qty -= by;
        totalQty_ -= by;
    }

    Order& front() { return orders_.front(); }
    const Order& front() const { return orders_.front(); }
    Handle frontHandle() { return orders_.begin(); }

    bool empty() const noexcept { return orders_.empty(); }
    std::size_t orderCount() const noexcept { return orders_.size(); }
    std::uint64_t totalQty() const noexcept { return totalQty_; }

    // Read-only iteration in FIFO order.
    std::list<Order>::const_iterator begin() const { return orders_.begin(); }
    std::list<Order>::const_iterator end() const { return orders_.end(); }

private:
    std::list<Order> orders_;
    std::uint64_t totalQty_ = 0;
};

} // namespace ob
