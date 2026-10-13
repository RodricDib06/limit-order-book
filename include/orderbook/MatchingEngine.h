#pragma once

#include "orderbook/OrderBook.h"
#include "orderbook/RiskEngine.h"
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

// Parse "key=value" risk-limit settings (maxqty, maxvalue, maxpos, credit, band,
// maxloss, stp) on top of `limits`. Dollar amounts use parsePrice. Returns false
// and leaves `limits` untouched if any setting is malformed.
bool parseLimits(const std::vector<std::string_view>& settings, RiskLimits& limits);

// Text front end for the order book: drives it from CLI commands or CSV rows
// and reports every acceptance, rejection, trade, and resting remainder.
// Orders go through the RiskEngine under the current account; the built-in
// "house" account has no limits.
class MatchingEngine {
public:
    explicit MatchingEngine(std::ostream& out);

    // Interactive command, e.g. "buy 100 @ 101.50", "sell 20", "cancel 42",
    // "modify 42 60", "book", "account alice maxpos=500", "risk", "help".
    // Returns false when the user asks to quit.
    bool execute(std::string_view line);

    // One CSV row: type,id,side,price,qty[,account]  (type = limit | market | cancel | modify),
    // or an account row: account,<name>[,key=value...]. The account column defaults to "house".
    // Blank lines, '#' comments, and a header row are ignored.
    // Returns false if the row could not be parsed.
    bool executeCsvLine(std::string_view line);

    void printBook(std::size_t levels = 5) const;
    void printRisk() const;
    static void printHelp(std::ostream& out);

    const OrderBook& book() const noexcept { return risk_.book(); }
    const RiskEngine& risk() const noexcept { return risk_; }
    const std::vector<Trade>& lastTrades() const noexcept { return trades_; }
    std::size_t totalTrades() const noexcept { return totalTrades_; }

private:
    void submitLimit(AccountId account, OrderId id, Side side, Price price, Qty qty);
    void submitMarket(AccountId account, OrderId id, Side side, Qty qty);
    void submitCancel(OrderId id);
    void submitModify(OrderId id, Qty qty);
    void reportTrades();
    void reportReject(AccountId account, OrderId id, Status status);
    void reportKills();
    void noteId(OrderId id);
    std::string tag(AccountId account) const;
    bool configureAccount(std::string_view name, const std::vector<std::string_view>& settings);

    RiskEngine risk_;
    AccountId house_;
    AccountId current_;
    std::ostream& out_;
    std::vector<Trade> trades_; // reused for every order
    OrderId nextId_ = 1;
    std::size_t totalTrades_ = 0;
};

} // namespace ob
