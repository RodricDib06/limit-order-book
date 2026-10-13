#include "orderbook/OrderBook.h"

#include <algorithm>
#include <sstream>

namespace ob {

void OrderBook::reserve(std::size_t expectedOrders) {
    index_.reserve(expectedOrders);
}

Status OrderBook::addLimit(const Order& order, std::vector<Trade>& trades) {
    if (order.qty == 0)
        return Status::InvalidQuantity;
    if (order.price <= 0)
        return Status::InvalidPrice;
    if (contains(order.id))
        return Status::DuplicateId;

    Qty remaining = order.side == Side::Buy
                        ? match(asks_, Side::Buy, order.id, order.qty, order.price, trades)
                        : match(bids_, Side::Sell, order.id, order.qty, order.price, trades);

    if (remaining > 0) {
        Order leftover = order;
        leftover.qty = remaining;
        rest(leftover);
    }
    return Status::Ok;
}

Status OrderBook::addMarket(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) {
    if (qty == 0)
        return Status::InvalidQuantity;
    if (contains(id))
        return Status::DuplicateId;

    if (side == Side::Buy)
        match(asks_, Side::Buy, id, qty, std::nullopt, trades);
    else
        match(bids_, Side::Sell, id, qty, std::nullopt, trades);
    return Status::Ok; // any unfilled remainder is dropped
}

Status OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end())
        return Status::UnknownId;

    const Locator loc = it->second;
    loc.level->erase(loc.handle);
    index_.erase(it);
    eraseLevelIfEmpty(loc.side, loc.price, *loc.level);
    return Status::Ok;
}

Status OrderBook::modify(OrderId id, Qty newQty) {
    if (newQty == 0)
        return Status::InvalidQuantity;
    auto it = index_.find(id);
    if (it == index_.end())
        return Status::UnknownId;

    Locator& loc = it->second;
    const Qty current = loc.handle->qty;
    if (newQty <= current) {
        loc.level->reduce(loc.handle, current - newQty); // keeps time priority
    } else {
        Order updated = *loc.handle; // an increase loses time priority
        updated.qty = newQty;
        loc.level->erase(loc.handle);
        loc.handle = loc.level->push(updated);
    }
    return Status::Ok;
}

Status OrderBook::replace(OrderId id, Price newPrice, Qty newQty, std::vector<Trade>& trades) {
    if (newQty == 0)
        return Status::InvalidQuantity;
    if (newPrice <= 0)
        return Status::InvalidPrice;
    auto it = index_.find(id);
    if (it == index_.end())
        return Status::UnknownId;

    if (it->second.price == newPrice)
        return modify(id, newQty);

    const Side side = it->second.side;
    cancel(id);
    return addLimit(Order{id, side, newPrice, newQty}, trades);
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty())
        return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty())
        return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    auto bid = bestBid();
    auto ask = bestAsk();
    if (!bid || !ask)
        return std::nullopt;
    return *ask - *bid;
}

namespace {
template <class Book> std::vector<LevelInfo> collectDepth(const Book& book, std::size_t levels) {
    std::vector<LevelInfo> out;
    out.reserve(std::min(levels, book.size()));
    for (auto it = book.begin(); it != book.end() && out.size() < levels; ++it)
        out.push_back(LevelInfo{it->first, it->second.totalQty(), it->second.orderCount()});
    return out;
}

template <class Book> std::uint64_t sumQty(const Book& book) {
    std::uint64_t total = 0;
    for (const auto& [price, level] : book)
        total += level.totalQty();
    return total;
}
} // namespace

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t levels) const {
    return side == Side::Buy ? collectDepth(bids_, levels) : collectDepth(asks_, levels);
}

std::optional<Order> OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end())
        return std::nullopt;
    return *it->second.handle;
}

std::size_t OrderBook::levelCount(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

std::uint64_t OrderBook::totalQty(Side side) const {
    return side == Side::Buy ? sumQty(bids_) : sumQty(asks_);
}

// The core matching loop. Walks the opposite side from its best price, filling
// FIFO within each level, until the incoming quantity is exhausted or the next
// level's price no longer crosses `limit` (no limit = market order).
template <class Book>
Qty OrderBook::match(Book& book, Side aggressor, OrderId id, Qty qty, std::optional<Price> limit,
                     std::vector<Trade>& trades) {
    while (qty > 0 && !book.empty()) {
        auto levelIt = book.begin();
        const Price levelPrice = levelIt->first;
        // key_comp orders levels best-first, so "limit sorts before level" means
        // the level is worse than the limit: stop.
        if (limit && book.key_comp()(*limit, levelPrice))
            break;

        PriceLevel& level = levelIt->second;
        while (qty > 0 && !level.empty()) {
            Order& resting = level.front();
            const Qty fill = std::min(qty, resting.qty);
            if (aggressor == Side::Buy)
                trades.push_back(Trade{id, resting.id, levelPrice, fill});
            else
                trades.push_back(Trade{resting.id, id, levelPrice, fill});
            qty -= fill;

            if (fill == resting.qty) {
                index_.erase(resting.id);
                level.erase(level.frontHandle());
            } else {
                level.reduce(level.frontHandle(), fill);
            }
        }
        if (level.empty())
            book.erase(levelIt);
    }
    return qty;
}

void OrderBook::rest(const Order& order) {
    PriceLevel* level = order.side == Side::Buy ? &bids_[order.price] : &asks_[order.price];
    auto handle = level->push(order);
    index_.emplace(order.id, Locator{order.side, order.price, level, handle});
}

void OrderBook::eraseLevelIfEmpty(Side side, Price price, const PriceLevel& level) {
    if (!level.empty())
        return;
    if (side == Side::Buy)
        bids_.erase(price);
    else
        asks_.erase(price);
}

template <class Book>
bool OrderBook::checkSide(const Book& book, Side side, std::size_t& orders,
                          std::string* why) const {
    auto fail = [why](const std::string& msg) {
        if (why)
            *why = msg;
        return false;
    };
    for (const auto& [price, level] : book) {
        if (level.empty())
            return fail("empty price level at " + std::to_string(price));
        std::uint64_t sum = 0;
        for (const Order& o : level) {
            if (o.qty == 0)
                return fail("zero-quantity order " + std::to_string(o.id));
            if (o.price != price || o.side != side)
                return fail("order " + std::to_string(o.id) + " is on the wrong level");
            auto it = index_.find(o.id);
            if (it == index_.end())
                return fail("order " + std::to_string(o.id) + " missing from index");
            if (&*it->second.handle != &o || it->second.level != &level)
                return fail("stale locator for order " + std::to_string(o.id));
            sum += o.qty;
            ++orders;
        }
        if (sum != level.totalQty())
            return fail("level total mismatch at " + std::to_string(price));
    }
    return true;
}

bool OrderBook::checkInvariants(std::string* why) const {
    std::size_t orders = 0;
    if (!checkSide(bids_, Side::Buy, orders, why) || !checkSide(asks_, Side::Sell, orders, why))
        return false;
    if (orders != index_.size()) {
        if (why)
            *why = "index size does not match resting orders";
        return false;
    }
    auto bid = bestBid();
    auto ask = bestAsk();
    if (bid && ask && *bid >= *ask) {
        if (why)
            *why = "book is crossed";
        return false;
    }
    return true;
}

} // namespace ob
