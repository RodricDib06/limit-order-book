#pragma once

#include "orderbook/OrderBook.h"
#include "orderbook/Types.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

constexpr Price kTicksPerUnit = 100; // 1 tick = $0.01

// Parse a decimal price such as "101.5" or "101.50" into ticks without ever
// going through floating point. Returns nullopt for malformed input or more
// than two decimal places. A leading '-' is accepted so the book can reject it.
std::optional<Price> parsePrice(std::string_view text);

// Format ticks as a decimal price, e.g. 10150 -> "101.50".
std::string formatPrice(Price ticks);

// Text front end for the order book: drives it from CLI commands or CSV rows
// and reports every acceptance, rejection, trade, and resting remainder.
class MatchingEngine {
public:
    explicit MatchingEngine(std::ostream& out);

    // Interactive command, e.g. "buy 100 @ 101.50", "sell 20", "cancel 42",
    // "modify 42 60", "book", "help". Returns false when the user asks to quit.
    bool execute(std::string_view line);

    // One CSV row: type,id,side,price,qty  (type = limit | market | cancel | modify).
    // Blank lines, '#' comments, and a header row are ignored.
    // Returns false if the row could not be parsed.
    bool executeCsvLine(std::string_view line);

    void printBook(std::size_t levels = 5) const;
    static void printHelp(std::ostream& out);

    const OrderBook& book() const noexcept { return book_; }
    const std::vector<Trade>& lastTrades() const noexcept { return trades_; }
    std::size_t totalTrades() const noexcept { return totalTrades_; }

private:
    void submitLimit(OrderId id, Side side, Price price, Qty qty);
    void submitMarket(OrderId id, Side side, Qty qty);
    void submitCancel(OrderId id);
    void submitModify(OrderId id, Qty qty);
    void reportTrades();
    void reportReject(OrderId id, Status status);
    void noteId(OrderId id);

    OrderBook book_;
    std::ostream& out_;
    std::vector<Trade> trades_; // reused for every order
    OrderId nextId_ = 1;
    std::size_t totalTrades_ = 0;
};

} // namespace ob
