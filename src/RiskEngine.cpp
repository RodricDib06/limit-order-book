#include "orderbook/RiskEngine.h"

#include <algorithm>
#include <limits>

namespace ob {

namespace {

__extension__ typedef __int128 Int128; // only for the cost-basis proration product

Money notional(Price price, std::uint64_t qty) {
    return price * static_cast<Money>(qty);
}

// Floor and ceiling division for a positive divisor.
std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

} // namespace

// ---- Accounts -----------------------------------------------------------------

AccountId RiskEngine::addAccount(std::string name, RiskLimits limits) {
    const auto id = static_cast<AccountId>(accounts_.size());
    byName_.emplace(name, id);
    Account a;
    a.name = std::move(name);
    a.limits = limits;
    accounts_.push_back(std::move(a));
    return id;
}

std::optional<AccountId> RiskEngine::findAccount(std::string_view name) const {
    auto it = byName_.find(std::string(name));
    if (it == byName_.end())
        return std::nullopt;
    return it->second;
}

Status RiskEngine::setLimits(AccountId account, const RiskLimits& limits) {
    if (account >= accounts_.size())
        return Status::UnknownAccount;
    accounts_[account].limits = limits;
    refreshTrigger(account);
    runPendingKills();
    return Status::Ok;
}

void RiskEngine::reserve(std::size_t expectedOrders) {
    book_.reserve(expectedOrders);
    orders_.reserve(expectedOrders);
}

// ---- Pre-trade checks ---------------------------------------------------------

std::optional<Price> RiskEngine::referencePrice() const {
    if (lastPrice_)
        return lastPrice_;
    auto bid = book_.bestBid();
    auto ask = book_.bestAsk();
    if (bid && ask)
        return (*bid + *ask) / 2;
    return std::nullopt;
}

// Value of a market order if it swept the opposite side right now.
Money RiskEngine::estimateSweepNotional(Side side, Qty qty) const {
    Money total = 0;
    book_.simulateMatch(side, std::nullopt, qty, [&total](const Order& resting, Qty fill) {
        total += notional(resting.price, fill);
        return true;
    });
    return total;
}

// Would this order actually trade against one of the account's own resting
// orders? Walks exactly the orders the incoming order would fill, in priority
// order. A non-crossing order stops at the first level, so this is O(1) unless
// the order trades, and then it costs about as much as the match itself.
bool RiskEngine::wouldSelfTrade(AccountId account, const Account& a, Side side,
                                std::optional<Price> limit, Qty qty) const {
    if ((side == Side::Buy ? a.openSellQty : a.openBuyQty) == 0)
        return false; // nothing of our own on the other side
    bool hit = false;
    book_.simulateMatch(side, limit, qty, [&](const Order& resting, Qty) {
        auto it = orders_.find(resting.id);
        hit = it != orders_.end() && it->second.account == account;
        return !hit;
    });
    return hit;
}

Status RiskEngine::checkNewOrder(AccountId account, Side side, std::optional<Price> limit,
                                 Qty qty) const {
    const Account& a = accounts_[account];
    const RiskLimits& L = a.limits;

    if (L.maxOrderQty && qty > L.maxOrderQty)
        return Status::RiskOrderQty;

    if (limit && L.priceBandBps) {
        if (auto ref = referencePrice()) {
            const Money diff = *limit > *ref ? *limit - *ref : *ref - *limit;
            if (diff * 10'000 > static_cast<Money>(L.priceBandBps) * *ref)
                return Status::RiskPriceBand;
        }
    }

    if (L.maxOrderNotional) {
        const Money value = limit ? notional(*limit, qty) : estimateSweepNotional(side, qty);
        if (value > L.maxOrderNotional)
            return Status::RiskOrderNotional;
    }

    if (L.maxPosition) {
        // Worst case: every open order on this side fills, plus this one.
        const auto q = static_cast<std::int64_t>(qty);
        if (side == Side::Buy &&
            a.position + static_cast<std::int64_t>(a.openBuyQty) + q > L.maxPosition)
            return Status::RiskPosition;
        if (side == Side::Sell &&
            a.position - static_cast<std::int64_t>(a.openSellQty) - q < -L.maxPosition)
            return Status::RiskPosition;
    }

    if (limit && L.maxOpenNotional && a.openNotional + notional(*limit, qty) > L.maxOpenNotional)
        return Status::RiskOpenNotional;

    if (L.selfTradePrevention && wouldSelfTrade(account, a, side, limit, qty))
        return Status::RiskSelfTrade;
    return Status::Ok;
}

// ---- Order entry --------------------------------------------------------------

Status RiskEngine::submitLimit(AccountId account, const Order& order, std::vector<Trade>& trades) {
    kills_.clear();
    if (account >= accounts_.size())
        return Status::UnknownAccount;
    Account& a = accounts_[account];
    auto reject = [&a](Status s) {
        ++a.rejects;
        return s;
    };

    if (order.qty == 0)
        return reject(Status::InvalidQuantity);
    if (order.price <= 0)
        return reject(Status::InvalidPrice);
    if (book_.contains(order.id))
        return reject(Status::DuplicateId);
    if (a.blocked)
        return reject(Status::AccountBlocked);
    if (Status st = checkNewOrder(account, order.side, order.price, order.qty); st != Status::Ok)
        return reject(st);

    const std::size_t from = trades.size();
    const Status st = book_.addLimit(order, trades);
    if (st != Status::Ok)
        return reject(st);

    processFills(account, order.side, trades, from);
    Qty filled = 0;
    for (std::size_t i = from; i < trades.size(); ++i)
        filled += trades[i].qty;
    if (filled < order.qty)
        trackOrder(account, order.id, order.side, order.price, order.qty - filled);
    runPendingKills();
    return Status::Ok;
}

Status RiskEngine::submitMarket(AccountId account, OrderId id, Side side, Qty qty,
                                std::vector<Trade>& trades) {
    kills_.clear();
    if (account >= accounts_.size())
        return Status::UnknownAccount;
    Account& a = accounts_[account];
    auto reject = [&a](Status s) {
        ++a.rejects;
        return s;
    };

    if (qty == 0)
        return reject(Status::InvalidQuantity);
    if (book_.contains(id))
        return reject(Status::DuplicateId);
    if (a.blocked)
        return reject(Status::AccountBlocked);
    if (Status st = checkNewOrder(account, side, std::nullopt, qty); st != Status::Ok)
        return reject(st);

    const std::size_t from = trades.size();
    const Status st = book_.addMarket(id, side, qty, trades);
    if (st != Status::Ok)
        return reject(st);
    processFills(account, side, trades, from);
    runPendingKills();
    return Status::Ok;
}

Status RiskEngine::cancel(OrderId id) {
    const Status st = book_.cancel(id);
    if (st == Status::Ok)
        untrack(orders_.find(id));
    return st;
}

Status RiskEngine::modify(OrderId id, Qty newQty) {
    if (newQty == 0)
        return Status::InvalidQuantity;
    auto it = orders_.find(id);
    if (it == orders_.end())
        return Status::UnknownId;
    OpenOrder& o = it->second;
    Account& a = accounts_[o.account];

    if (newQty > o.remaining) {
        // An increase adds risk, so it is checked like a new order for the extra quantity.
        const Qty extra = newQty - o.remaining;
        const RiskLimits& L = a.limits;
        Status st = Status::Ok;
        if (a.blocked)
            st = Status::AccountBlocked;
        else if (L.maxOrderQty && newQty > L.maxOrderQty)
            st = Status::RiskOrderQty;
        else if (L.maxOrderNotional && notional(o.price, newQty) > L.maxOrderNotional)
            st = Status::RiskOrderNotional;
        else if (L.maxPosition && o.side == Side::Buy &&
                 a.position + static_cast<std::int64_t>(a.openBuyQty + extra) > L.maxPosition)
            st = Status::RiskPosition;
        else if (L.maxPosition && o.side == Side::Sell &&
                 a.position - static_cast<std::int64_t>(a.openSellQty + extra) < -L.maxPosition)
            st = Status::RiskPosition;
        else if (L.maxOpenNotional && a.openNotional + notional(o.price, extra) > L.maxOpenNotional)
            st = Status::RiskOpenNotional;
        if (st != Status::Ok) {
            ++a.rejects;
            return st;
        }
    }

    const Status st = book_.modify(id, newQty);
    if (st != Status::Ok)
        return st;
    if (newQty < o.remaining) {
        reduceOpen(o, o.remaining - newQty);
    } else {
        const Qty extra = newQty - o.remaining;
        (o.side == Side::Buy ? a.openBuyQty : a.openSellQty) += extra;
        a.openNotional += notional(o.price, extra);
        o.remaining = newQty;
    }
    return Status::Ok;
}

// ---- Open-order tracking ------------------------------------------------------

void RiskEngine::trackOrder(AccountId account, OrderId id, Side side, Price price, Qty qty) {
    Account& a = accounts_[account];
    orders_.emplace(id, OpenOrder{account, side, price, qty});
    (side == Side::Buy ? a.openBuyQty : a.openSellQty) += qty;
    a.openNotional += notional(price, qty);
}

void RiskEngine::reduceOpen(OpenOrder& o, Qty by) {
    Account& a = accounts_[o.account];
    (o.side == Side::Buy ? a.openBuyQty : a.openSellQty) -= by;
    a.openNotional -= notional(o.price, by);
    o.remaining -= by;
}

void RiskEngine::untrack(std::unordered_map<OrderId, OpenOrder>::iterator it) {
    OpenOrder& o = it->second;
    reduceOpen(o, o.remaining);
    orders_.erase(it);
}

std::optional<AccountId> RiskEngine::ownerOf(OrderId id) const {
    auto it = orders_.find(id);
    if (it == orders_.end())
        return std::nullopt;
    return it->second.account;
}

// ---- Post-trade: positions and P&L --------------------------------------------

void RiskEngine::processFills(AccountId aggressor, Side side, const std::vector<Trade>& trades,
                              std::size_t from) {
    if (from == trades.size())
        return;
    for (std::size_t i = from; i < trades.size(); ++i) {
        const Trade& t = trades[i];
        lastPrice_ = t.price;
        auto it = orders_.find(side == Side::Buy ? t.sellId : t.buyId);
        const AccountId restingAccount = it->second.account;
        const Side restingSide = it->second.side;
        reduceOpen(it->second, t.qty);
        if (it->second.remaining == 0)
            orders_.erase(it);
        applyFill(restingAccount, restingSide, t.price, t.qty);
        applyFill(aggressor, side, t.price, t.qty);
    }
    fireTriggers(); // the mark moved: check every other account's loss trigger
}

// Average-cost position keeping. costBasis is |position| x average entry price;
// reducing a position releases cost basis pro rata, flipping starts a new basis.
void RiskEngine::applyFill(AccountId account, Side side, Price price, Qty qty) {
    Account& a = accounts_[account];
    const auto q = static_cast<std::int64_t>(qty);
    const std::int64_t signedQty = side == Side::Buy ? q : -q;
    a.cash += side == Side::Buy ? -notional(price, qty) : notional(price, qty);
    a.volume += qty;
    ++a.fills;

    if (a.position == 0 || (a.position > 0) == (signedQty > 0)) {
        a.costBasis += notional(price, qty);
        a.position += signedQty;
    } else {
        const std::int64_t held = a.position > 0 ? a.position : -a.position;
        const std::int64_t closed = std::min(q, held);
        a.costBasis -= static_cast<Money>(static_cast<Int128>(a.costBasis) * closed / held);
        a.position += signedQty > 0 ? closed : -closed;
        if (const std::int64_t opened = q - closed; opened > 0) {
            a.costBasis = price * opened;
            a.position += signedQty > 0 ? opened : -opened;
        }
        if (a.position == 0)
            a.costBasis = 0;
    }
    if (a.limits.maxLoss)
        refreshTrigger(account);
}

Money RiskEngine::totalPnl(const Account& a) const {
    return a.cash + a.position * lastPrice_.value_or(0);
}

AccountSnapshot RiskEngine::snapshot(AccountId account) const {
    const Account& a = accounts_.at(account);
    AccountSnapshot s;
    s.name = a.name;
    s.limits = a.limits;
    s.position = a.position;
    s.cash = a.cash;
    const std::int64_t held = a.position > 0 ? a.position : -a.position;
    const Price mark = lastPrice_.value_or(0);
    if (held > 0) {
        s.avgCost = a.costBasis / held;
        s.unrealizedPnl = a.position > 0 ? held * mark - a.costBasis : a.costBasis - held * mark;
    }
    s.totalPnl = totalPnl(a);
    s.realizedPnl = s.totalPnl - s.unrealizedPnl;
    s.openBuyQty = a.openBuyQty;
    s.openSellQty = a.openSellQty;
    s.openNotional = a.openNotional;
    s.volume = a.volume;
    s.fills = a.fills;
    s.rejects = a.rejects;
    s.blocked = a.blocked;
    s.blockReason = a.blockReason;
    return s;
}

// ---- Loss limits and the kill switch ------------------------------------------

// Total P&L is cash + position x mark, so for a fixed position the loss limit
// is breached exactly when the mark crosses one price. Precompute that price:
//   breach  <=>  position x mark < T,  where T = -maxLoss - cash
//   long  (position > 0): breach when mark < ceil(T / position)
//   short (position < 0): breach when mark > floor(-T / |position|)
void RiskEngine::refreshTrigger(AccountId account) {
    Account& a = accounts_[account];
    if (a.blocked || a.limits.maxLoss == 0 || totalPnl(a) < -a.limits.maxLoss || a.position == 0) {
        clearTrigger(a);
        if (!a.blocked && a.limits.maxLoss && totalPnl(a) < -a.limits.maxLoss)
            pendingKills_.push_back(account); // already in breach
        return;
    }
    const Money T = -a.limits.maxLoss - a.cash;
    const Trigger kind = a.position > 0 ? Trigger::Long : Trigger::Short;
    const Price key = kind == Trigger::Long ? ceilDiv(T, a.position) : floorDiv(-T, -a.position);
    TriggerMap& triggers = kind == Trigger::Long ? longTriggers_ : shortTriggers_;
    if (a.trigger == kind) {
        if (a.triggerIt->first == key)
            return;
        // Re-key the existing node in place: no allocation on the fill path.
        auto node = triggers.extract(a.triggerIt);
        node.key() = key;
        a.triggerIt = triggers.insert(std::move(node));
    } else {
        clearTrigger(a);
        a.trigger = kind;
        a.triggerIt = triggers.emplace(key, account);
    }
}

void RiskEngine::clearTrigger(Account& a) {
    if (a.trigger == Trigger::Long)
        longTriggers_.erase(a.triggerIt);
    else if (a.trigger == Trigger::Short)
        shortTriggers_.erase(a.triggerIt);
    a.trigger = Trigger::None;
}

void RiskEngine::fireTriggers() {
    const Price mark = *lastPrice_;
    for (auto it = longTriggers_.upper_bound(mark); it != longTriggers_.end(); ++it)
        pendingKills_.push_back(it->second);
    for (auto it = shortTriggers_.begin(); it != shortTriggers_.lower_bound(mark); ++it)
        pendingKills_.push_back(it->second);
}

void RiskEngine::runPendingKills() {
    for (AccountId id : pendingKills_) {
        const Account& a = accounts_[id];
        if (!a.blocked && totalPnl(a) < -a.limits.maxLoss)
            kill(id, "max loss breached");
    }
    pendingKills_.clear();
}

std::size_t RiskEngine::kill(AccountId account, std::string reason) {
    if (account >= accounts_.size())
        return 0;
    std::vector<OrderId> ids;
    for (const auto& [id, o] : orders_)
        if (o.account == account)
            ids.push_back(id);
    for (OrderId id : ids)
        cancel(id);

    Account& a = accounts_[account];
    a.blocked = true;
    a.blockReason = reason;
    clearTrigger(a);
    kills_.push_back(KillEvent{account, std::move(reason), ids.size()});
    return ids.size();
}

Status RiskEngine::unblock(AccountId account) {
    if (account >= accounts_.size())
        return Status::UnknownAccount;
    Account& a = accounts_[account];
    // An account still past its loss limit stays blocked until the limit is raised.
    if (a.limits.maxLoss && totalPnl(a) < -a.limits.maxLoss)
        return Status::AccountBlocked;
    a.blocked = false;
    a.blockReason.clear();
    refreshTrigger(account);
    return Status::Ok;
}

// ---- Invariants ---------------------------------------------------------------

bool RiskEngine::checkInvariants(std::string* why) const {
    auto fail = [why](const std::string& msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (!book_.checkInvariants(why))
        return false;
    if (orders_.size() != book_.orderCount())
        return fail("risk tracks " + std::to_string(orders_.size()) + " orders, book has " +
                    std::to_string(book_.orderCount()));

    struct Open {
        std::uint64_t buy = 0, sell = 0;
        Money value = 0;
    };
    std::vector<Open> open(accounts_.size());
    for (const auto& [id, o] : orders_) {
        auto b = book_.find(id);
        if (!b || b->qty != o.remaining || b->side != o.side || b->price != o.price)
            return fail("order " + std::to_string(id) + " differs from the book");
        Open& x = open[o.account];
        (o.side == Side::Buy ? x.buy : x.sell) += o.remaining;
        x.value += notional(o.price, o.remaining);
    }

    std::int64_t positions = 0;
    Money cash = 0;
    for (std::size_t i = 0; i < accounts_.size(); ++i) {
        const Account& a = accounts_[i];
        const Open& x = open[i];
        if (a.openBuyQty != x.buy || a.openSellQty != x.sell || a.openNotional != x.value)
            return fail("open-order totals wrong for account " + a.name);
        if (a.blocked && (x.buy || x.sell))
            return fail("blocked account " + a.name + " still has orders");
        if (!a.blocked && a.limits.maxLoss && totalPnl(a) < -a.limits.maxLoss)
            return fail("account " + a.name + " is past its loss limit but not blocked");
        if (a.position == 0 && a.costBasis != 0)
            return fail("flat account " + a.name + " has cost basis");
        positions += a.position;
        cash += a.cash;
    }
    // Every trade has a buyer and a seller, so the firm as a whole is flat.
    if (positions != 0)
        return fail("positions do not sum to zero");
    if (cash != 0)
        return fail("cash does not sum to zero");
    return true;
}

} // namespace ob
