#pragma once

#include <cstddef>
#include <cstdint>

namespace ob {

using Price = std::int64_t;    // price in integer ticks (1 tick = 1 cent)
using Qty = std::uint32_t;     // quantity in shares
using OrderId = std::uint64_t; // client-supplied order identifier

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

struct Order {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Qty qty{}; // remaining quantity
};

struct Trade {
    OrderId buyId{};
    OrderId sellId{};
    Price price{}; // always the resting order's price
    Qty qty{};

    friend bool operator==(const Trade& a, const Trade& b) noexcept {
        return a.buyId == b.buyId && a.sellId == b.sellId && a.price == b.price && a.qty == b.qty;
    }
    friend bool operator!=(const Trade& a, const Trade& b) noexcept { return !(a == b); }
};

// Aggregated view of one price level, used for market-depth queries.
struct LevelInfo {
    Price price{};
    std::uint64_t totalQty{};
    std::size_t orderCount{};
};

enum class Status : std::uint8_t {
    Ok,
    InvalidQuantity, // zero quantity
    InvalidPrice,    // zero or negative price
    DuplicateId,     // an order with this ID is already live in the book
    UnknownId,       // no live order with this ID (never existed, filled, or cancelled)
};

constexpr const char* toString(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

constexpr const char* toString(Status s) noexcept {
    switch (s) {
    case Status::Ok:
        return "ok";
    case Status::InvalidQuantity:
        return "invalid quantity";
    case Status::InvalidPrice:
        return "invalid price";
    case Status::DuplicateId:
        return "duplicate order id";
    case Status::UnknownId:
        return "unknown order id";
    }
    return "unknown status";
}

} // namespace ob
