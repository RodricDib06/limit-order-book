#include "orderbook/OrderBook.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ob;

namespace {

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book;
    std::vector<Trade> trades;

    Status buy(OrderId id, Price price, Qty qty) {
        trades.clear();
        return book.addLimit(Order{id, Side::Buy, price, qty}, trades);
    }
    Status sell(OrderId id, Price price, Qty qty) {
        trades.clear();
        return book.addLimit(Order{id, Side::Sell, price, qty}, trades);
    }
    Status marketBuy(OrderId id, Qty qty) {
        trades.clear();
        return book.addMarket(id, Side::Buy, qty, trades);
    }
    Status marketSell(OrderId id, Qty qty) {
        trades.clear();
        return book.addMarket(id, Side::Sell, qty, trades);
    }
    Qty restingQty(OrderId id) const {
        auto o = book.find(id);
        return o ? o->qty : 0;
    }

    void TearDown() override {
        std::string why;
        EXPECT_TRUE(book.checkInvariants(&why)) << why;
    }
};

} // namespace

// ---- Basic matching ---------------------------------------------------------

TEST_F(OrderBookTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(book.bestBid());
    EXPECT_FALSE(book.bestAsk());
    EXPECT_FALSE(book.spread());
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, BuyMeetsSellAtSamePriceTradesFullQuantity) {
    ASSERT_EQ(sell(1, 10100, 100), Status::Ok);
    ASSERT_EQ(buy(2, 10100, 100), Status::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 10100, 100}));
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, SellMeetsRestingBuy) {
    ASSERT_EQ(buy(1, 10100, 40), Status::Ok);
    ASSERT_EQ(sell(2, 10100, 40), Status::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{1, 2, 10100, 40}));
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, TradeExecutesAtRestingPriceForBuyAggressor) {
    sell(1, 10000, 10);
    buy(2, 10500, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10000);
}

TEST_F(OrderBookTest, TradeExecutesAtRestingPriceForSellAggressor) {
    buy(1, 10500, 10);
    sell(2, 10000, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 10500);
}

// ---- No cross ---------------------------------------------------------------

TEST_F(OrderBookTest, NoCrossBothOrdersRest) {
    buy(1, 9900, 10);
    sell(2, 10100, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.bestBid(), 9900);
    EXPECT_EQ(book.bestAsk(), 10100);
    EXPECT_EQ(book.spread(), 200);
    EXPECT_EQ(book.orderCount(), 2u);
}

TEST_F(OrderBookTest, BuyBelowAskDoesNotTrade) {
    sell(1, 10100, 10);
    buy(2, 10099, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.orderCount(), 2u);
}

// ---- Price priority ---------------------------------------------------------

TEST_F(OrderBookTest, BuyFillsLowestAskFirst) {
    sell(1, 10100, 10);
    sell(2, 10000, 10);
    buy(3, 10100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{3, 2, 10000, 10}));
    EXPECT_TRUE(book.contains(1));
}

TEST_F(OrderBookTest, SellFillsHighestBidFirst) {
    buy(1, 9900, 10);
    buy(2, 10000, 10);
    sell(3, 9900, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 3, 10000, 10}));
    EXPECT_TRUE(book.contains(1));
}

// ---- Time priority ----------------------------------------------------------

TEST_F(OrderBookTest, EarlierAskAtSamePriceFillsFirst) {
    sell(1, 10000, 10);
    sell(2, 10000, 10);
    buy(3, 10000, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_TRUE(book.contains(2));
    EXPECT_FALSE(book.contains(1));
}

TEST_F(OrderBookTest, EarlierBidAtSamePriceFillsFirst) {
    buy(1, 10000, 10);
    buy(2, 10000, 10);
    buy(3, 10000, 10);
    sell(4, 10000, 25);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].buyId, 1u);
    EXPECT_EQ(trades[1].buyId, 2u);
    EXPECT_EQ(trades[2], (Trade{3, 4, 10000, 5}));
    EXPECT_EQ(restingQty(3), 5u);
}

// ---- Partial fills ----------------------------------------------------------

TEST_F(OrderBookTest, WorkedExampleFromSpec) {
    // Asks 50 @ 101 and 30 @ 102; limit buy 70 @ 102.
    sell(1, 10100, 50);
    sell(2, 10200, 30);
    buy(3, 10200, 70);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{3, 1, 10100, 50}));
    EXPECT_EQ(trades[1], (Trade{3, 2, 10200, 20}));
    EXPECT_EQ(restingQty(2), 10u);
    EXPECT_EQ(book.bestAsk(), 10200);
    EXPECT_FALSE(book.bestBid()); // the buy fully filled, nothing rests on the bid side
}

TEST_F(OrderBookTest, IncomingPartiallyFilledRemainderRests) {
    sell(1, 10000, 40);
    buy(2, 10000, 100);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 40u);
    EXPECT_EQ(restingQty(2), 60u);
    EXPECT_EQ(book.bestBid(), 10000);
    EXPECT_FALSE(book.bestAsk());
}

TEST_F(OrderBookTest, RestingPartiallyFilledKeepsPriority) {
    sell(1, 10000, 100);
    sell(2, 10000, 100);
    buy(3, 10000, 30);
    buy(4, 10000, 30);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(restingQty(1), 40u);
    EXPECT_EQ(restingQty(2), 100u);
}

TEST_F(OrderBookTest, LimitStopsAtLimitPriceAcrossLevels) {
    sell(1, 10000, 10);
    sell(2, 10100, 10);
    sell(3, 10200, 10);
    buy(4, 10100, 50);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(restingQty(4), 30u);
    EXPECT_EQ(book.bestBid(), 10100);
    EXPECT_EQ(book.bestAsk(), 10200);
}

TEST_F(OrderBookTest, SellSweepsMultipleBidLevels) {
    buy(1, 10200, 10);
    buy(2, 10100, 10);
    buy(3, 10000, 10);
    sell(4, 10000, 25);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0], (Trade{1, 4, 10200, 10}));
    EXPECT_EQ(trades[1], (Trade{2, 4, 10100, 10}));
    EXPECT_EQ(trades[2], (Trade{3, 4, 10000, 5}));
    EXPECT_EQ(restingQty(3), 5u);
}

// ---- Market orders ----------------------------------------------------------

TEST_F(OrderBookTest, MarketBuySweepsSeveralLevels) {
    sell(1, 10000, 10);
    sell(2, 10100, 10);
    sell(3, 10500, 10);
    ASSERT_EQ(marketBuy(4, 25), Status::Ok);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[2], (Trade{4, 3, 10500, 5}));
    EXPECT_EQ(restingQty(3), 5u);
}

TEST_F(OrderBookTest, MarketBuyLeftoverDroppedWhenBookEmpties) {
    sell(1, 10000, 10);
    ASSERT_EQ(marketBuy(2, 50), Status::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 10u);
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.contains(2));
}

TEST_F(OrderBookTest, MarketSellSweepsBids) {
    buy(1, 10000, 10);
    buy(2, 9900, 10);
    ASSERT_EQ(marketSell(3, 15), Status::Ok);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{1, 3, 10000, 10}));
    EXPECT_EQ(trades[1], (Trade{2, 3, 9900, 5}));
}

TEST_F(OrderBookTest, MarketOrderOnEmptyBookDoesNothing) {
    ASSERT_EQ(marketBuy(1, 10), Status::Ok);
    EXPECT_TRUE(trades.empty());
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, MarketOrderNeverRests) {
    buy(1, 9000, 10);
    ASSERT_EQ(marketBuy(2, 10), Status::Ok); // no asks: nothing to trade against
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.orderCount(), 1u);
    EXPECT_EQ(book.bestBid(), 9000);
}

TEST_F(OrderBookTest, MarketOrderZeroQuantityRejected) {
    sell(1, 10000, 10);
    EXPECT_EQ(marketBuy(2, 0), Status::InvalidQuantity);
    EXPECT_EQ(restingQty(1), 10u);
}

// ---- Cancel -----------------------------------------------------------------

TEST_F(OrderBookTest, CancelRemovesOrder) {
    buy(1, 10000, 10);
    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.bestBid());
}

TEST_F(OrderBookTest, CancelledOrderNeverFills) {
    sell(1, 10000, 10);
    sell(2, 10100, 10);
    book.cancel(1);
    buy(3, 10100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 2u);
}

TEST_F(OrderBookTest, CancelUnknownIdFails) {
    EXPECT_EQ(book.cancel(42), Status::UnknownId);
}

TEST_F(OrderBookTest, CancelFilledOrderFails) {
    sell(1, 10000, 10);
    buy(2, 10000, 10);
    EXPECT_EQ(book.cancel(1), Status::UnknownId);
    EXPECT_EQ(book.cancel(2), Status::UnknownId);
}

TEST_F(OrderBookTest, CancelTwiceFails) {
    buy(1, 10000, 10);
    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_EQ(book.cancel(1), Status::UnknownId);
}

TEST_F(OrderBookTest, CancelMiddleOfQueuePreservesOrder) {
    sell(1, 10000, 10);
    sell(2, 10000, 10);
    sell(3, 10000, 10);
    book.cancel(2);
    buy(4, 10000, 20);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(trades[1].sellId, 3u);
}

TEST_F(OrderBookTest, CancelPartiallyFilledOrder) {
    sell(1, 10000, 100);
    buy(2, 10000, 30);
    EXPECT_EQ(book.cancel(1), Status::Ok);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, CancelledIdCanBeReused) {
    buy(1, 10000, 10);
    book.cancel(1);
    EXPECT_EQ(buy(1, 9900, 5), Status::Ok);
    EXPECT_EQ(restingQty(1), 5u);
}

// ---- Validation -------------------------------------------------------------

TEST_F(OrderBookTest, ZeroQuantityRejected) {
    EXPECT_EQ(buy(1, 10000, 0), Status::InvalidQuantity);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, NegativePriceRejected) {
    EXPECT_EQ(buy(1, -100, 10), Status::InvalidPrice);
    EXPECT_EQ(sell(2, -1, 10), Status::InvalidPrice);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, ZeroPriceRejected) {
    EXPECT_EQ(sell(1, 0, 10), Status::InvalidPrice);
    EXPECT_TRUE(book.empty());
}

TEST_F(OrderBookTest, DuplicateIdRejected) {
    ASSERT_EQ(buy(1, 10000, 10), Status::Ok);
    EXPECT_EQ(sell(1, 11000, 10), Status::DuplicateId);
    EXPECT_EQ(marketSell(1, 10), Status::DuplicateId);
    EXPECT_EQ(book.orderCount(), 1u);
    EXPECT_EQ(restingQty(1), 10u);
}

TEST_F(OrderBookTest, RejectedOrderProducesNoTrades) {
    sell(1, 10000, 10);
    EXPECT_EQ(buy(1, 10000, 10), Status::DuplicateId);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(restingQty(1), 10u);
}

// ---- Modify -----------------------------------------------------------------

TEST_F(OrderBookTest, ModifyDecreaseKeepsPriority) {
    sell(1, 10000, 50);
    sell(2, 10000, 50);
    EXPECT_EQ(book.modify(1, 20), Status::Ok);
    EXPECT_EQ(restingQty(1), 20u);
    buy(3, 10000, 20);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 1u);
}

TEST_F(OrderBookTest, ModifyIncreaseLosesPriority) {
    sell(1, 10000, 50);
    sell(2, 10000, 50);
    EXPECT_EQ(book.modify(1, 80), Status::Ok);
    buy(3, 10000, 50);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 2u);
    EXPECT_EQ(restingQty(1), 80u);
}

TEST_F(OrderBookTest, ModifyUpdatesLevelTotal) {
    buy(1, 10000, 50);
    buy(2, 10000, 30);
    book.modify(1, 10);
    auto d = book.depth(Side::Buy);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].totalQty, 40u);
}

TEST_F(OrderBookTest, ModifyToZeroRejected) {
    buy(1, 10000, 50);
    EXPECT_EQ(book.modify(1, 0), Status::InvalidQuantity);
    EXPECT_EQ(restingQty(1), 50u);
}

TEST_F(OrderBookTest, ModifyUnknownIdRejected) {
    EXPECT_EQ(book.modify(7, 10), Status::UnknownId);
}

TEST_F(OrderBookTest, ReplacePriceLosesPriorityAndCanTrade) {
    sell(1, 10200, 10);
    buy(2, 10000, 10);
    buy(3, 10000, 10);
    trades.clear();
    ASSERT_EQ(book.replace(2, 10200, 10, trades), Status::Ok);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 10200, 10}));
    EXPECT_FALSE(book.contains(2));
    EXPECT_TRUE(book.contains(3));
}

TEST_F(OrderBookTest, ReplaceSamePriceActsAsModify) {
    buy(1, 10000, 10);
    buy(2, 10000, 10);
    trades.clear();
    ASSERT_EQ(book.replace(1, 10000, 5, trades), Status::Ok);
    sell(3, 10000, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buyId, 1u);
}

TEST_F(OrderBookTest, ReplaceValidatesInput) {
    buy(1, 10000, 10);
    EXPECT_EQ(book.replace(1, 0, 10, trades), Status::InvalidPrice);
    EXPECT_EQ(book.replace(1, 10000, 0, trades), Status::InvalidQuantity);
    EXPECT_EQ(book.replace(9, 10000, 10, trades), Status::UnknownId);
    EXPECT_EQ(restingQty(1), 10u);
}

// ---- Book state -------------------------------------------------------------

TEST_F(OrderBookTest, EmptyLevelsAreRemoved) {
    sell(1, 10000, 10);
    sell(2, 10100, 10);
    buy(3, 10000, 10);
    EXPECT_EQ(book.levelCount(Side::Sell), 1u);
    book.cancel(2);
    EXPECT_EQ(book.levelCount(Side::Sell), 0u);
}

TEST_F(OrderBookTest, BestPricesUpdateAfterFillsAndCancels) {
    buy(1, 9900, 10);
    buy(2, 9800, 10);
    sell(3, 10100, 10);
    sell(4, 10200, 10);
    EXPECT_EQ(book.bestBid(), 9900);
    EXPECT_EQ(book.bestAsk(), 10100);
    book.cancel(1);
    EXPECT_EQ(book.bestBid(), 9800);
    buy(5, 10100, 10);
    EXPECT_EQ(book.bestAsk(), 10200);
    buy(6, 10000, 10);
    EXPECT_EQ(book.bestBid(), 10000);
}

TEST_F(OrderBookTest, DepthReturnsTopFiveLevelsBestFirst) {
    for (OrderId i = 1; i <= 7; ++i) {
        buy(i, 10000 - static_cast<Price>(i) * 10, 10);
        sell(100 + i, 10100 + static_cast<Price>(i) * 10, 10);
    }
    buy(50, 9990, 5); // second order on the best bid level

    auto bids = book.depth(Side::Buy);
    auto asks = book.depth(Side::Sell);
    ASSERT_EQ(bids.size(), 5u);
    ASSERT_EQ(asks.size(), 5u);
    EXPECT_EQ(bids[0].price, 9990);
    EXPECT_EQ(bids[0].totalQty, 15u);
    EXPECT_EQ(bids[0].orderCount, 2u);
    EXPECT_EQ(bids[4].price, 9950);
    EXPECT_EQ(asks[0].price, 10110);
    EXPECT_EQ(asks[4].price, 10150);
}

TEST_F(OrderBookTest, DepthWithFewerLevelsThanRequested) {
    buy(1, 10000, 10);
    EXPECT_EQ(book.depth(Side::Buy, 5).size(), 1u);
    EXPECT_TRUE(book.depth(Side::Sell, 5).empty());
    EXPECT_EQ(book.depth(Side::Buy, 0).size(), 0u);
}

TEST_F(OrderBookTest, TotalQuantityPerSide) {
    buy(1, 10000, 10);
    buy(2, 9900, 15);
    sell(3, 10100, 7);
    EXPECT_EQ(book.totalQty(Side::Buy), 25u);
    EXPECT_EQ(book.totalQty(Side::Sell), 7u);
}

TEST_F(OrderBookTest, FindReturnsRemainingQuantity) {
    sell(1, 10000, 100);
    buy(2, 10000, 35);
    auto o = book.find(1);
    ASSERT_TRUE(o);
    EXPECT_EQ(o->qty, 65u);
    EXPECT_EQ(o->side, Side::Sell);
    EXPECT_EQ(o->price, 10000);
    EXPECT_FALSE(book.find(2));
}

TEST_F(OrderBookTest, LargeQuantitiesDoNotOverflowLevelTotal) {
    const Qty big = 4'000'000'000u;
    sell(1, 10000, big);
    sell(2, 10000, big);
    auto d = book.depth(Side::Sell);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].totalQty, 8'000'000'000ull);
}
