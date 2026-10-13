#pragma once

#include "orderbook/OrderPool.h"
#include "orderbook/PriceLevel.h"
#include "orderbook/Types.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ob {

// A single-instrument limit order book with price-time priority matching.
//
// Every operation that can trade appends its fills to a caller-supplied buffer,
// so a hot loop can reuse one vector instead of allocating per order.
// Rejected operations leave the book (and the buffer) untouched.
class OrderBook {
public:
    // Pre-size the order index and node pool for the expected number of live orders.
    void reserve(std::size_t expectedOrders);

    // Match a limit order, then rest any unfilled remainder in the book.
    Status addLimit(const Order& order, std::vector<Trade>& trades);

    // Match a market order against the best prices; any unfilled remainder is dropped.
    Status addMarket(OrderId id, Side side, Qty qty, std::vector<Trade>& trades);

    // Remove a resting order. O(1) average.
    Status cancel(OrderId id);

    // Change a resting order's quantity. A decrease keeps its place in the queue;
    // an increase sends it to the back of its price level.
    Status modify(OrderId id, Qty newQty);

    // Change a resting order's price and quantity. A new price is treated as
    // cancel + new order (it loses priority and may trade immediately).
    Status replace(OrderId id, Price newPrice, Qty newQty, std::vector<Trade>& trades);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::optional<Price> spread() const;

    // Top `levels` price levels on one side, best first.
    std::vector<LevelInfo> depth(Side side, std::size_t levels = 5) const;

    std::optional<Order> find(OrderId id) const;
    bool contains(OrderId id) const { return index_.count(id) != 0; }
    std::size_t orderCount() const noexcept { return index_.size(); }
    std::size_t levelCount(Side side) const noexcept;
    std::uint64_t totalQty(Side side) const;
    bool empty() const noexcept { return index_.empty(); }

    // Full structural self-check, used by tests. Returns false and fills `why` on failure.
    bool checkInvariants(std::string* why = nullptr) const;

private:
    using Bids = std::map<Price, PriceLevel, std::greater<Price>>; // begin() = highest bid
    using Asks = std::map<Price, PriceLevel, std::less<Price>>;    // begin() = lowest ask

    // Everything needed to reach a resting order without searching.
    struct Locator {
        Side side;
        Price price;
        PriceLevel* level; // map nodes are stable, so this stays valid while the level exists
        PriceLevel::Handle handle;
    };

    template <class Book>
    Qty match(Book& book, Side aggressor, OrderId id, Qty qty, std::optional<Price> limit,
              std::vector<Trade>& trades);

    void rest(const Order& order);
    void eraseLevelIfEmpty(Side side, Price price, const PriceLevel& level);

    template <class Book>
    bool checkSide(const Book& book, Side side, std::size_t& orders, std::string* why) const;

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
    OrderPool pool_; // storage for every resting order
};

} // namespace ob
