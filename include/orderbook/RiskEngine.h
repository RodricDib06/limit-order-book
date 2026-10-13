#pragma once

#include "orderbook/OrderBook.h"
#include "orderbook/Types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ob {

using AccountId = std::uint32_t;
using Money = std::int64_t; // ticks x shares, i.e. cents

// Per-account risk limits. A value of 0 (or false) disables that check.
struct RiskLimits {
    Qty maxOrderQty = 0;              // fat-finger: largest single order
    Money maxOrderNotional = 0;       // fat-finger: largest single order value
    std::int64_t maxPosition = 0;     // largest long or short position, counting open orders
    Money maxOpenNotional = 0;        // credit limit on the value of resting orders
    std::uint32_t priceBandBps = 0;   // max distance from the reference price, in basis points
    Money maxLoss = 0;                // kill switch fires when total P&L drops below -maxLoss
    bool selfTradePrevention = false; // reject orders that would trade with the account's own
};

// Point-in-time view of one account, for reports and tests.
struct AccountSnapshot {
    std::string name;
    RiskLimits limits;
    std::int64_t position = 0; // signed shares: + long, - short
    Money cash = 0;            // net cash from trading (sells add, buys subtract)
    Price avgCost = 0;         // average entry price of the open position
    Money realizedPnl = 0;
    Money unrealizedPnl = 0; // open position marked to the last trade price
    Money totalPnl = 0;
    std::uint64_t openBuyQty = 0;
    std::uint64_t openSellQty = 0;
    Money openNotional = 0;
    std::uint64_t volume = 0; // shares traded
    std::size_t fills = 0;
    std::size_t rejects = 0;
    bool blocked = false;
    std::string blockReason;
};

// Fired when the kill switch stops an account.
struct KillEvent {
    AccountId account;
    std::string reason;
    std::size_t ordersCancelled;
};

// Pre-trade risk gate and post-trade position keeper in front of an OrderBook.
//
// Every order goes through the account's limits before it reaches the book.
// Every fill updates both counterparties' positions, cash, and P&L. Loss limits
// are enforced continuously: each account with a position has a trigger price
// (like a stop order) at which its loss would breach the limit. When the last
// trade price crosses it, the kill switch cancels the account's orders and
// blocks it. A price move costs O(log A), not a scan of all A accounts.
class RiskEngine {
public:
    AccountId addAccount(std::string name, RiskLimits limits = {});
    std::optional<AccountId> findAccount(std::string_view name) const;
    Status setLimits(AccountId account, const RiskLimits& limits);
    std::size_t accountCount() const noexcept { return accounts_.size(); }

    Status submitLimit(AccountId account, const Order& order, std::vector<Trade>& trades);
    Status submitMarket(AccountId account, OrderId id, Side side, Qty qty,
                        std::vector<Trade>& trades);
    Status cancel(OrderId id);
    Status modify(OrderId id, Qty newQty); // increases are risk-checked; decreases always pass

    // Kill switch: cancel every resting order of the account and block it.
    // Returns the number of orders cancelled.
    std::size_t kill(AccountId account, std::string reason);
    Status unblock(AccountId account);

    // Kill-switch events triggered by the most recent submit call.
    const std::vector<KillEvent>& lastKills() const noexcept { return kills_; }

    AccountSnapshot snapshot(AccountId account) const;
    std::optional<AccountId> ownerOf(OrderId id) const;
    std::optional<Price> lastTradePrice() const noexcept { return lastPrice_; }
    // Last trade price if there is one, else the mid price, else nothing.
    std::optional<Price> referencePrice() const;

    const OrderBook& book() const noexcept { return book_; }
    void reserve(std::size_t expectedOrders);

    // Cross-checks open-order tracking against the book and firm-wide
    // conservation (positions and cash both sum to zero). Used by tests.
    bool checkInvariants(std::string* why = nullptr) const;

private:
    enum class Trigger : std::uint8_t { None, Long, Short };
    using TriggerMap = std::multimap<Price, AccountId>;

    struct Account {
        std::string name;
        RiskLimits limits;
        std::int64_t position = 0;
        Money cash = 0;
        Money costBasis = 0; // |position| x average cost
        std::uint64_t openBuyQty = 0;
        std::uint64_t openSellQty = 0;
        Money openNotional = 0;
        std::uint64_t volume = 0;
        std::size_t fills = 0;
        std::size_t rejects = 0;
        bool blocked = false;
        std::string blockReason;
        Trigger trigger = Trigger::None;
        TriggerMap::iterator triggerIt;
    };

    struct OpenOrder {
        AccountId account;
        Side side;
        Price price;
        Qty remaining;
    };

    Status checkNewOrder(AccountId account, Side side, std::optional<Price> limit, Qty qty) const;
    Money estimateSweepNotional(Side side, Qty qty) const;
    bool wouldSelfTrade(AccountId account, const Account& a, Side side, std::optional<Price> limit,
                        Qty qty) const;

    void trackOrder(AccountId account, OrderId id, Side side, Price price, Qty qty);
    void reduceOpen(OpenOrder& o, Qty by);
    void untrack(std::unordered_map<OrderId, OpenOrder>::iterator it);

    void processFills(AccountId aggressor, Side side, const std::vector<Trade>& trades,
                      std::size_t from);
    void applyFill(AccountId account, Side side, Price price, Qty qty);
    Money totalPnl(const Account& a) const;
    void refreshTrigger(AccountId account);
    void clearTrigger(Account& a);
    void fireTriggers();
    void runPendingKills();

    OrderBook book_;
    std::vector<Account> accounts_;
    std::unordered_map<std::string, AccountId> byName_;
    std::unordered_map<OrderId, OpenOrder> orders_;
    std::optional<Price> lastPrice_;
    TriggerMap longTriggers_;  // breach when the mark falls below the key
    TriggerMap shortTriggers_; // breach when the mark rises above the key
    std::vector<AccountId> pendingKills_;
    std::vector<KillEvent> kills_;
};

} // namespace ob
