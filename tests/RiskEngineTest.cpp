#include "orderbook/RiskEngine.h"
#include "orderbook/MatchingEngine.h"

#include <gtest/gtest.h>

#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace ob;

namespace {

class RiskTest : public ::testing::Test {
protected:
    RiskEngine risk;
    std::vector<Trade> trades;
    OrderId nextId = 1;
    OrderId lastId = 0;

    Status buy(AccountId a, Price price, Qty qty) { return limit(a, Side::Buy, price, qty); }
    Status sell(AccountId a, Price price, Qty qty) { return limit(a, Side::Sell, price, qty); }
    Status limit(AccountId a, Side side, Price price, Qty qty) {
        trades.clear();
        lastId = nextId++;
        return risk.submitLimit(a, Order{lastId, side, price, qty}, trades);
    }
    Status market(AccountId a, Side side, Qty qty) {
        trades.clear();
        lastId = nextId++;
        return risk.submitMarket(a, lastId, side, qty, trades);
    }
    // Trade `qty` at `price` between two accounts, leaving nothing resting.
    void cross(AccountId buyer, AccountId seller, Price price, Qty qty) {
        ASSERT_EQ(sell(seller, price, qty), Status::Ok);
        ASSERT_EQ(buy(buyer, price, qty), Status::Ok);
        ASSERT_EQ(trades.size(), 1u);
    }
    AccountSnapshot snap(AccountId a) const { return risk.snapshot(a); }

    void TearDown() override {
        std::string why;
        EXPECT_TRUE(risk.checkInvariants(&why)) << why;
    }
};

RiskLimits withMaxLoss(Money dollars) {
    RiskLimits l;
    l.maxLoss = dollars * 100;
    return l;
}

} // namespace

// ---- Accounts -----------------------------------------------------------------

TEST_F(RiskTest, AccountsAreFoundByName) {
    const AccountId a = risk.addAccount("alice");
    const AccountId b = risk.addAccount("bob");
    EXPECT_EQ(risk.findAccount("alice"), a);
    EXPECT_EQ(risk.findAccount("bob"), b);
    EXPECT_FALSE(risk.findAccount("mallory"));
    EXPECT_EQ(risk.accountCount(), 2u);
}

TEST_F(RiskTest, UnknownAccountRejected) {
    EXPECT_EQ(buy(7, 10000, 10), Status::UnknownAccount);
    EXPECT_EQ(market(7, Side::Buy, 10), Status::UnknownAccount);
    EXPECT_TRUE(risk.book().empty());
}

TEST_F(RiskTest, NoLimitsBehavesLikePlainBook) {
    const AccountId a = risk.addAccount("a");
    const AccountId b = risk.addAccount("b");
    ASSERT_EQ(sell(a, 10100, 50), Status::Ok);
    ASSERT_EQ(buy(b, 10200, 70), Status::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{lastId, 1, 10100, 50}));
    EXPECT_EQ(risk.ownerOf(lastId), b); // the 20 unfilled shares rest for b
}

// ---- Pre-trade checks ---------------------------------------------------------

TEST_F(RiskTest, BasicValidationStillApplies) {
    const AccountId a = risk.addAccount("a");
    EXPECT_EQ(buy(a, 10000, 0), Status::InvalidQuantity);
    EXPECT_EQ(buy(a, -5, 10), Status::InvalidPrice);
    EXPECT_EQ(snap(a).rejects, 2u);
}

TEST_F(RiskTest, MaxOrderQuantity) {
    RiskLimits l;
    l.maxOrderQty = 100;
    const AccountId a = risk.addAccount("a", l);
    EXPECT_EQ(buy(a, 10000, 100), Status::Ok);
    EXPECT_EQ(buy(a, 10000, 101), Status::RiskOrderQty);
    EXPECT_EQ(market(a, Side::Sell, 500), Status::RiskOrderQty);
    EXPECT_EQ(snap(a).rejects, 2u);
    EXPECT_EQ(risk.book().orderCount(), 1u);
}

TEST_F(RiskTest, MaxOrderNotionalForLimitOrders) {
    RiskLimits l;
    l.maxOrderNotional = 10'000 * 100; // $10,000
    const AccountId a = risk.addAccount("a", l);
    EXPECT_EQ(buy(a, 10000, 100), Status::Ok);                // exactly $10,000
    EXPECT_EQ(buy(a, 10001, 100), Status::RiskOrderNotional); // $10,001
}

TEST_F(RiskTest, MaxOrderNotionalForMarketOrdersWalksTheBook) {
    RiskLimits l;
    l.maxOrderNotional = 3'000 * 100; // $3,000
    const AccountId a = risk.addAccount("a", l);
    const AccountId mm = risk.addAccount("mm");
    sell(mm, 10000, 10); // $1,000
    sell(mm, 20000, 10); // $2,000
    sell(mm, 30000, 10);
    EXPECT_EQ(market(a, Side::Buy, 20), Status::Ok);                // sweeps $3,000
    sell(mm, 10000, 10);                                            // asks now: 10 @ 100, 10 @ 300
    EXPECT_EQ(market(a, Side::Buy, 17), Status::RiskOrderNotional); // $1,000 + 7 x $300
    EXPECT_EQ(market(a, Side::Buy, 16), Status::Ok);                // $1,000 + 6 x $300
    // Only what the book can fill counts: a huge order against a thin book is fine.
    EXPECT_EQ(market(a, Side::Sell, 1'000), Status::Ok);
}

TEST_F(RiskTest, PriceBandUsesLastTradePrice) {
    RiskLimits l;
    l.priceBandBps = 500; // 5%
    const AccountId a = risk.addAccount("a", l);
    const AccountId b = risk.addAccount("b");
    const AccountId c = risk.addAccount("c");
    EXPECT_EQ(buy(a, 1, 10), Status::Ok); // no reference price yet: no band
    risk.cancel(lastId);
    cross(b, c, 10000, 10);                  // last trade 100.00
    EXPECT_EQ(buy(a, 9500, 10), Status::Ok); // exactly 5% away
    EXPECT_EQ(buy(a, 9499, 10), Status::RiskPriceBand);
    EXPECT_EQ(sell(a, 10500, 10), Status::Ok);
    EXPECT_EQ(sell(a, 10501, 10), Status::RiskPriceBand);
    EXPECT_EQ(market(a, Side::Buy, 1), Status::Ok); // market orders have no price to check
}

TEST_F(RiskTest, PriceBandFallsBackToMidPrice) {
    RiskLimits l;
    l.priceBandBps = 1000; // 10%
    const AccountId a = risk.addAccount("a", l);
    const AccountId mm = risk.addAccount("mm");
    buy(mm, 9900, 10);
    sell(mm, 10100, 10); // mid 100.00
    EXPECT_EQ(risk.referencePrice(), 10000);
    EXPECT_EQ(buy(a, 9000, 1), Status::Ok);
    EXPECT_EQ(buy(a, 8999, 1), Status::RiskPriceBand);
}

TEST_F(RiskTest, PositionLimitCountsOpenOrders) {
    RiskLimits l;
    l.maxPosition = 100;
    const AccountId a = risk.addAccount("a", l);
    EXPECT_EQ(buy(a, 10000, 60), Status::Ok);
    EXPECT_EQ(buy(a, 9900, 40), Status::Ok);
    EXPECT_EQ(buy(a, 9800, 1), Status::RiskPosition); // 100 already open
    risk.cancel(lastId - 1);
    EXPECT_EQ(buy(a, 9800, 1), Status::Ok); // cancel frees room
}

TEST_F(RiskTest, PositionLimitCountsFilledPosition) {
    RiskLimits l;
    l.maxPosition = 100;
    const AccountId a = risk.addAccount("a", l);
    const AccountId b = risk.addAccount("b");
    cross(a, b, 10000, 80);
    EXPECT_EQ(snap(a).position, 80);
    EXPECT_EQ(buy(a, 10000, 21), Status::RiskPosition);
    EXPECT_EQ(buy(a, 10000, 20), Status::Ok);
    // Selling reduces risk: selling 180 lands exactly at the -100 short limit.
    EXPECT_EQ(sell(a, 20000, 180), Status::Ok);
    EXPECT_EQ(sell(a, 20000, 1), Status::RiskPosition);
}

TEST_F(RiskTest, ShortPositionLimit) {
    RiskLimits l;
    l.maxPosition = 50;
    const AccountId a = risk.addAccount("a", l);
    const AccountId b = risk.addAccount("b");
    cross(b, a, 10000, 50);
    EXPECT_EQ(snap(a).position, -50);
    EXPECT_EQ(market(a, Side::Sell, 1), Status::RiskPosition);
    EXPECT_EQ(buy(a, 9000, 100), Status::Ok); // buying back up to +50 is fine
}

TEST_F(RiskTest, CreditLimitOnRestingOrderValue) {
    RiskLimits l;
    l.maxOpenNotional = 5'000 * 100; // $5,000 of resting orders
    const AccountId a = risk.addAccount("a", l);
    EXPECT_EQ(buy(a, 10000, 30), Status::Ok);  // $3,000
    EXPECT_EQ(sell(a, 10100, 19), Status::Ok); // $1,919 more
    const OrderId ask = lastId;
    EXPECT_EQ(snap(a).openNotional, 491'900);
    EXPECT_EQ(buy(a, 9000, 1), Status::RiskOpenNotional); // would be $5,009
    risk.cancel(ask);
    EXPECT_EQ(buy(a, 9000, 1), Status::Ok);
}

TEST_F(RiskTest, SelfTradePreventionRejectsCrossingOwnOrders) {
    RiskLimits l;
    l.selfTradePrevention = true;
    const AccountId a = risk.addAccount("a", l);
    ASSERT_EQ(sell(a, 10100, 10), Status::Ok);
    EXPECT_EQ(buy(a, 10100, 5), Status::RiskSelfTrade);
    EXPECT_EQ(buy(a, 10200, 5), Status::RiskSelfTrade);
    EXPECT_EQ(market(a, Side::Buy, 5), Status::RiskSelfTrade);
    EXPECT_EQ(buy(a, 10099, 5), Status::Ok); // does not reach the own ask
    EXPECT_EQ(sell(a, 10099, 1), Status::RiskSelfTrade);
    EXPECT_TRUE(trades.empty());
}

TEST_F(RiskTest, SelfTradePreventionAllowsOrdersFilledByOthersFirst) {
    RiskLimits l;
    l.selfTradePrevention = true;
    const AccountId a = risk.addAccount("a", l);
    const AccountId x = risk.addAccount("x");
    sell(x, 10000, 10); // someone else's ask at a better price
    sell(a, 10100, 10); // a's own ask behind it
    // Buying 10 @ 101 is filled entirely by x at 100 and never reaches a's ask.
    EXPECT_EQ(buy(a, 10100, 10), Status::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10000);
    sell(x, 10000, 10);
    EXPECT_EQ(buy(a, 10100, 11), Status::RiskSelfTrade); // the 11th share would hit a's ask
    EXPECT_EQ(market(a, Side::Buy, 10), Status::Ok);
}

TEST_F(RiskTest, SelfTradePreventionChecksTimePriorityWithinLevel) {
    RiskLimits l;
    l.selfTradePrevention = true;
    const AccountId a = risk.addAccount("a", l);
    const AccountId x = risk.addAccount("x");
    sell(x, 10000, 5);
    sell(a, 10000, 5); // same price, but behind x in the queue
    EXPECT_EQ(buy(a, 10000, 5), Status::Ok);
    sell(x, 10000, 5); // now behind a's ask
    EXPECT_EQ(buy(a, 10000, 1), Status::RiskSelfTrade);
}

TEST_F(RiskTest, WithoutSelfTradePreventionAccountCanTradeWithItself) {
    const AccountId a = risk.addAccount("a");
    sell(a, 10000, 10);
    ASSERT_EQ(buy(a, 10000, 10), Status::Ok);
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(snap(a).position, 0);
    EXPECT_EQ(snap(a).cash, 0);
}

TEST_F(RiskTest, ModifyIncreaseIsRiskChecked) {
    RiskLimits l;
    l.maxPosition = 100;
    l.maxOrderQty = 80;
    const AccountId a = risk.addAccount("a", l);
    buy(a, 10000, 50);
    const OrderId first = lastId;
    buy(a, 9900, 30);
    EXPECT_EQ(risk.modify(first, 90), Status::RiskOrderQty);
    EXPECT_EQ(risk.modify(first, 71), Status::RiskPosition); // 71 + 30 > 100
    EXPECT_EQ(risk.modify(first, 70), Status::Ok);
    EXPECT_EQ(risk.modify(first, 10), Status::Ok); // decreases always pass
    EXPECT_EQ(snap(a).openBuyQty, 40u);
}

// ---- Positions and P&L ----------------------------------------------------------

TEST_F(RiskTest, FillUpdatesBothCounterparties) {
    const AccountId a = risk.addAccount("a");
    const AccountId b = risk.addAccount("b");
    cross(a, b, 10000, 25);
    EXPECT_EQ(snap(a).position, 25);
    EXPECT_EQ(snap(b).position, -25);
    EXPECT_EQ(snap(a).cash, -250'000);
    EXPECT_EQ(snap(b).cash, 250'000);
    EXPECT_EQ(snap(a).volume, 25u);
    EXPECT_EQ(snap(b).fills, 1u);
}

TEST_F(RiskTest, AverageCostAndRealizedPnl) {
    const AccountId a = risk.addAccount("a");
    const AccountId x = risk.addAccount("x");
    cross(a, x, 10000, 100);
    cross(a, x, 10200, 100);
    EXPECT_EQ(snap(a).avgCost, 10100);
    cross(x, a, 10400, 50); // sell 50 at 104 against an average cost of 101

    const AccountSnapshot s = snap(a);
    EXPECT_EQ(s.position, 150);
    EXPECT_EQ(s.avgCost, 10100);
    EXPECT_EQ(s.realizedPnl, 50 * 300);    // $150
    EXPECT_EQ(s.unrealizedPnl, 150 * 300); // marked at 104: $450
    EXPECT_EQ(s.totalPnl, s.realizedPnl + s.unrealizedPnl);
}

TEST_F(RiskTest, FlippingFromLongToShort) {
    const AccountId a = risk.addAccount("a");
    const AccountId x = risk.addAccount("x");
    cross(a, x, 10000, 100);
    cross(x, a, 10100, 150);
    const AccountSnapshot s = snap(a);
    EXPECT_EQ(s.position, -50);
    EXPECT_EQ(s.avgCost, 10100);
    EXPECT_EQ(s.realizedPnl, 100 * 100); // $100 on the closed 100 shares
    EXPECT_EQ(s.unrealizedPnl, 0);
}

TEST_F(RiskTest, UnrealizedPnlFollowsMarkPrice) {
    const AccountId a = risk.addAccount("a");
    const AccountId x = risk.addAccount("x");
    const AccountId y = risk.addAccount("y");
    cross(a, x, 10000, 10);
    cross(x, y, 9750, 1); // the market moves, a is not involved
    EXPECT_EQ(snap(a).unrealizedPnl, -2'500);
    EXPECT_EQ(snap(a).totalPnl, -2'500);
    EXPECT_EQ(snap(a).realizedPnl, 0);
}

TEST_F(RiskTest, ClosedPositionHasOnlyRealizedPnl) {
    const AccountId a = risk.addAccount("a");
    const AccountId x = risk.addAccount("x");
    cross(a, x, 10000, 30);
    cross(x, a, 9900, 30);
    const AccountSnapshot s = snap(a);
    EXPECT_EQ(s.position, 0);
    EXPECT_EQ(s.realizedPnl, -3'000);
    EXPECT_EQ(s.unrealizedPnl, 0);
    EXPECT_EQ(s.cash, -3'000);
}

// ---- Kill switch ----------------------------------------------------------------

TEST_F(RiskTest, ManualKillCancelsOrdersAndBlocks) {
    const AccountId a = risk.addAccount("a");
    const AccountId b = risk.addAccount("b");
    buy(a, 9900, 10);
    sell(a, 10100, 10);
    buy(b, 9800, 10);
    EXPECT_EQ(risk.kill(a, "test"), 2u);
    EXPECT_TRUE(snap(a).blocked);
    EXPECT_EQ(risk.book().orderCount(), 1u);
    EXPECT_EQ(buy(a, 9900, 1), Status::AccountBlocked);
    EXPECT_EQ(market(a, Side::Sell, 1), Status::AccountBlocked);
    EXPECT_EQ(risk.unblock(a), Status::Ok);
    EXPECT_EQ(buy(a, 9900, 1), Status::Ok);
}

TEST_F(RiskTest, MarketMoveTriggersLossLimitOfUninvolvedAccount) {
    const AccountId alice = risk.addAccount("alice", withMaxLoss(500));
    const AccountId bob = risk.addAccount("bob");
    const AccountId carol = risk.addAccount("carol");
    const AccountId dave = risk.addAccount("dave");
    cross(alice, bob, 10000, 100); // alice long 100 @ 100.00
    buy(alice, 9000, 10);          // and a resting bid
    const OrderId aliceBid = lastId;

    cross(dave, carol, 9500, 10); // mark 95.00: loss exactly $500, still allowed
    EXPECT_FALSE(snap(alice).blocked);
    EXPECT_TRUE(risk.lastKills().empty());

    cross(dave, carol, 9499, 10); // mark 94.99: loss $501
    EXPECT_TRUE(snap(alice).blocked);
    ASSERT_EQ(risk.lastKills().size(), 1u);
    EXPECT_EQ(risk.lastKills()[0].account, alice);
    EXPECT_EQ(risk.lastKills()[0].ordersCancelled, 1u);
    EXPECT_FALSE(risk.book().contains(aliceBid));
}

TEST_F(RiskTest, ShortPositionTriggersOnPriceRise) {
    const AccountId s = risk.addAccount("shorty", withMaxLoss(100));
    const AccountId x = risk.addAccount("x");
    const AccountId y = risk.addAccount("y");
    cross(x, s, 10000, 20); // short 20 @ 100.00
    cross(x, y, 10500, 1);  // +$5 per share: loss $100, at the limit
    EXPECT_FALSE(snap(s).blocked);
    cross(x, y, 10501, 1);
    EXPECT_TRUE(snap(s).blocked);
}

TEST_F(RiskTest, RealizedLossTriggersKillWhenFlat) {
    const AccountId a = risk.addAccount("a", withMaxLoss(50));
    const AccountId x = risk.addAccount("x");
    cross(a, x, 10000, 10);
    buy(a, 5000, 5);       // a resting order that should be cancelled
    cross(x, a, 9400, 10); // closes the position at a $60 loss
    EXPECT_EQ(snap(a).position, 0);
    EXPECT_TRUE(snap(a).blocked);
    EXPECT_EQ(risk.book().orderCount(), 0u);
}

TEST_F(RiskTest, AggressorKilledByItsOwnFillLosesRestingRemainder) {
    const AccountId a = risk.addAccount("a", withMaxLoss(10));
    const AccountId x = risk.addAccount("x");
    cross(a, x, 10000, 10); // a long 10 @ 100.00
    sell(x, 9000, 5);
    // a buys 10 @ 90: 5 fill against x at 90.00, which marks a's position down
    // $100 and breaches the $10 limit. The 5 that were left resting get cancelled.
    buy(a, 9000, 10);
    EXPECT_TRUE(snap(a).blocked);
    EXPECT_FALSE(risk.book().contains(lastId)); // the unfilled 5 were cancelled
    EXPECT_EQ(risk.lastKills().size(), 1u);
}

TEST_F(RiskTest, UnblockRefusedWhileStillPastLossLimit) {
    const AccountId a = risk.addAccount("a", withMaxLoss(10));
    const AccountId x = risk.addAccount("x");
    const AccountId y = risk.addAccount("y");
    cross(a, x, 10000, 10);
    cross(x, y, 9800, 1); // loss $20
    ASSERT_TRUE(snap(a).blocked);
    EXPECT_EQ(risk.unblock(a), Status::AccountBlocked);
    risk.setLimits(a, withMaxLoss(100));
    EXPECT_EQ(risk.unblock(a), Status::Ok);
    EXPECT_FALSE(snap(a).blocked);
}

TEST_F(RiskTest, LoweringLossLimitBelowCurrentLossKillsImmediately) {
    const AccountId a = risk.addAccount("a", withMaxLoss(1000));
    const AccountId x = risk.addAccount("x");
    const AccountId y = risk.addAccount("y");
    cross(a, x, 10000, 10);
    cross(x, y, 9500, 1); // loss $50
    EXPECT_FALSE(snap(a).blocked);
    risk.setLimits(a, withMaxLoss(40));
    EXPECT_TRUE(snap(a).blocked);
}

// ---- Randomized: many accounts, limits always respected -------------------------

TEST(RiskRandomizedTest, LimitsAndConservationHoldThroughout) {
    RiskEngine risk;
    std::mt19937_64 rng(2026);
    std::vector<RiskLimits> limits;
    for (int i = 0; i < 12; ++i) {
        RiskLimits l;
        l.maxPosition = 200 + 50 * i;
        l.maxOrderQty = 60 + 5 * i;
        l.maxOpenNotional = (i % 3 == 0) ? 0 : 400'000 * 100;
        l.priceBandBps = (i % 2) ? 300 : 0;
        l.maxLoss = (i % 4 == 0) ? 0 : (20 + 10 * i) * 100;
        l.selfTradePrevention = i % 2 == 0;
        limits.push_back(l);
        risk.addAccount("acct" + std::to_string(i), l);
    }

    std::vector<Trade> trades;
    std::vector<OrderId> ids;
    OrderId nextId = 1;
    std::size_t accepted = 0, rejected = 0, kills = 0;
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<AccountId> acct(0, 11);
    std::uniform_int_distribution<Price> price(9900, 10100);
    std::uniform_int_distribution<Qty> qty(1, 80);

    for (int i = 0; i < 30'000; ++i) {
        const int p = pct(rng);
        const AccountId a = acct(rng);
        const Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
        trades.clear();
        Status st = Status::Ok;
        if (p < 65) {
            const OrderId id = nextId++;
            st = risk.submitLimit(a, Order{id, side, price(rng), qty(rng)}, trades);
            ids.push_back(id);
        } else if (p < 75) {
            st = risk.submitMarket(a, nextId++, side, qty(rng), trades);
        } else if (p < 92 && !ids.empty()) {
            risk.cancel(ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)]);
        } else if (p < 98 && !ids.empty()) {
            risk.modify(ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)],
                        qty(rng));
        } else {
            const AccountId k = acct(rng);
            if (risk.snapshot(k).blocked)
                risk.unblock(k);
        }
        (st == Status::Ok ? accepted : rejected)++;
        kills += risk.lastKills().size();

        if (i % 50 == 0) {
            std::string why;
            ASSERT_TRUE(risk.checkInvariants(&why)) << "op " << i << ": " << why;
        }
        // Worst-case exposure never exceeds the position limit, on either side.
        for (AccountId k = 0; k < 12; ++k) {
            const AccountSnapshot s = risk.snapshot(k);
            ASSERT_LE(s.position + static_cast<std::int64_t>(s.openBuyQty), limits[k].maxPosition)
                << s.name << " at op " << i;
            ASSERT_GE(s.position - static_cast<std::int64_t>(s.openSellQty), -limits[k].maxPosition)
                << s.name << " at op " << i;
            if (limits[k].maxOpenNotional) {
                ASSERT_LE(s.openNotional, limits[k].maxOpenNotional);
            }
        }
    }
    std::string why;
    ASSERT_TRUE(risk.checkInvariants(&why)) << why;
    // The scenario should exercise both outcomes, not trivially accept or reject everything.
    EXPECT_GT(accepted, 5'000u);
    EXPECT_GT(rejected, 500u);
    EXPECT_GT(kills, 0u);
}

// ---- CLI and CSV ----------------------------------------------------------------

namespace {
class RiskCliTest : public ::testing::Test {
protected:
    std::ostringstream out;
    MatchingEngine engine{out};
    bool has(const std::string& s) const { return out.str().find(s) != std::string::npos; }
};
} // namespace

TEST(ParseLimits, ParsesAllKeys) {
    RiskLimits l;
    ASSERT_TRUE(parseLimits({"maxqty=500", "maxpos=1000", "maxvalue=50000", "credit=250000.50",
                             "band=300", "maxloss=2000", "stp=off"},
                            l));
    EXPECT_EQ(l.maxOrderQty, 500u);
    EXPECT_EQ(l.maxPosition, 1000);
    EXPECT_EQ(l.maxOrderNotional, 5'000'000);
    EXPECT_EQ(l.maxOpenNotional, 25'000'050);
    EXPECT_EQ(l.priceBandBps, 300u);
    EXPECT_EQ(l.maxLoss, 200'000);
    EXPECT_FALSE(l.selfTradePrevention);
}

TEST(ParseLimits, RejectsBadInputWithoutPartialUpdate) {
    RiskLimits l;
    l.maxOrderQty = 7;
    EXPECT_FALSE(parseLimits({"maxqty=10", "bogus=1"}, l));
    EXPECT_FALSE(parseLimits({"maxqty"}, l));
    EXPECT_FALSE(parseLimits({"maxloss=-5"}, l));
    EXPECT_FALSE(parseLimits({"stp=maybe"}, l));
    EXPECT_EQ(l.maxOrderQty, 7u);
}

TEST_F(RiskCliTest, AccountCommandSwitchesAndAppliesLimits) {
    engine.execute("account alice maxqty=100");
    engine.execute("buy 101 @ 100");
    EXPECT_TRUE(has("REJECTED #1 [alice]: risk: order quantity above limit"));
    engine.execute("buy 100 @ 100");
    EXPECT_TRUE(has("ACCEPTED #2 [alice] BUY 100 @ 100.00"));
    engine.execute("sell 1 @ 99");
    EXPECT_TRUE(has("risk: would trade with own order")); // stp is on by default
}

TEST_F(RiskCliTest, RiskReportShowsPositionsAndPnl) {
    engine.execute("account alice");
    engine.execute("buy 10 @ 100");
    engine.execute("account bob");
    engine.execute("sell 10 @ 100");
    engine.execute("risk");
    EXPECT_TRUE(has("Mark price: 100.00"));
    EXPECT_TRUE(has("alice"));
    EXPECT_TRUE(has("bob"));
    EXPECT_TRUE(has("active"));
}

TEST_F(RiskCliTest, KillAndUnblockCommands) {
    engine.execute("account alice");
    engine.execute("buy 10 @ 100");
    engine.execute("kill alice");
    EXPECT_TRUE(has("KILLED   [alice] manual kill switch, 1 order(s) cancelled"));
    engine.execute("buy 10 @ 100");
    EXPECT_TRUE(has("risk: account is blocked"));
    engine.execute("unblock alice");
    EXPECT_TRUE(has("UNBLOCKED [alice]"));
    EXPECT_TRUE(engine.book().empty());
}

TEST_F(RiskCliTest, AutomaticKillIsReported) {
    engine.execute("account alice maxloss=10");
    engine.execute("buy 10 @ 100");
    engine.execute("use house");
    engine.execute("sell 10 @ 100");
    engine.execute("sell 1 @ 98");
    engine.execute("buy 1 @ 98");
    EXPECT_TRUE(has("KILLED   [alice] max loss breached"));
}

TEST_F(RiskCliTest, CsvAccountsAndAccountColumn) {
    EXPECT_TRUE(engine.executeCsvLine("account,alice,maxpos=50"));
    EXPECT_TRUE(engine.executeCsvLine("limit,1,buy,100,40,alice"));
    EXPECT_TRUE(engine.executeCsvLine("limit,2,buy,100,20,alice"));
    EXPECT_TRUE(has("REJECTED #2 [alice]: risk: position limit would be exceeded"));
    EXPECT_TRUE(engine.executeCsvLine("limit,3,sell,100,40")); // house account
    EXPECT_EQ(engine.risk().snapshot(*engine.risk().findAccount("alice")).position, 40);
    EXPECT_FALSE(engine.executeCsvLine("limit,4,buy,100,1,nobody"));
    EXPECT_FALSE(engine.executeCsvLine("account,bob,maxpos=lots"));
}
