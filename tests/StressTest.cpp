// Invariant and randomized tests. After every operation the book must be
// uncrossed, structurally consistent, and conserve quantity: resting quantity
// changes only by what was added, what traded, and what was cancelled.

#include "orderbook/OrderBook.h"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace ob;

namespace {

std::uint64_t restingTotal(const OrderBook& book) {
    return book.totalQty(Side::Buy) + book.totalQty(Side::Sell);
}

std::uint64_t tradedQty(const std::vector<Trade>& trades) {
    std::uint64_t sum = 0;
    for (const Trade& t : trades)
        sum += t.qty;
    return sum;
}

void expectValidTrades(const std::vector<Trade>& trades, OrderId aggressor, Side side,
                       std::optional<Price> limit) {
    Price prev = side == Side::Buy ? 0 : std::numeric_limits<Price>::max();
    for (const Trade& t : trades) {
        ASSERT_GT(t.qty, 0u);
        ASSERT_EQ(side == Side::Buy ? t.buyId : t.sellId, aggressor);
        // Fills walk the book from best to worse price, never past the limit.
        if (side == Side::Buy) {
            ASSERT_GE(t.price, prev);
            if (limit) {
                ASSERT_LE(t.price, *limit);
            }
        } else {
            ASSERT_LE(t.price, prev);
            if (limit) {
                ASSERT_GE(t.price, *limit);
            }
        }
        prev = t.price;
    }
}

struct RandomRun {
    std::uint64_t seed;
    int operations;
    bool checkStructureEveryOp;
};

void runRandom(const RandomRun& cfg) {
    std::mt19937_64 rng(cfg.seed);
    OrderBook book;
    std::vector<Trade> trades;
    std::vector<OrderId> ids;
    OrderId nextId = 1;
    std::uniform_int_distribution<int> opDist(0, 99);
    std::uniform_int_distribution<Price> priceDist(9950, 10050);
    std::uniform_int_distribution<Qty> qtyDist(1, 200);
    std::uniform_int_distribution<int> coin(0, 1);

    // A limit-vs-resting fill consumes quantity on both sides; a market fill
    // consumes only the resting side (the market order itself never rests).
    std::uint64_t added = 0, cancelled = 0, removedByTrades = 0;

    for (int i = 0; i < cfg.operations; ++i) {
        const int op = opDist(rng);
        const std::uint64_t before = restingTotal(book);
        trades.clear();

        if (op < 60) { // limit
            const Side side = coin(rng) ? Side::Buy : Side::Sell;
            const Price price = priceDist(rng);
            const Qty qty = qtyDist(rng);
            const OrderId id = nextId++;
            ASSERT_EQ(book.addLimit(Order{id, side, price, qty}, trades), Status::Ok);
            expectValidTrades(trades, id, side, price);
            const std::uint64_t t = tradedQty(trades);
            ASSERT_LE(t, qty);
            // Incoming adds qty, each fill removes it from both sides.
            ASSERT_EQ(restingTotal(book), before + qty - 2 * t);
            ASSERT_EQ(book.contains(id), t < qty);
            added += qty;
            removedByTrades += 2 * t;
            ids.push_back(id);
        } else if (op < 70) { // market
            const Side side = coin(rng) ? Side::Buy : Side::Sell;
            const Qty qty = qtyDist(rng);
            const OrderId id = nextId++;
            ASSERT_EQ(book.addMarket(id, side, qty, trades), Status::Ok);
            expectValidTrades(trades, id, side, std::nullopt);
            const std::uint64_t t = tradedQty(trades);
            ASSERT_LE(t, qty);
            ASSERT_EQ(restingTotal(book), before - t);
            ASSERT_FALSE(book.contains(id));
            removedByTrades += t;
        } else if (op < 90 && !ids.empty()) { // cancel
            const OrderId id =
                ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)];
            auto o = book.find(id);
            const Status st = book.cancel(id);
            ASSERT_EQ(st == Status::Ok, o.has_value());
            if (o) {
                ASSERT_EQ(restingTotal(book), before - o->qty);
                cancelled += o->qty;
            }
            ASSERT_FALSE(book.contains(id));
        } else if (!ids.empty()) { // modify
            const OrderId id =
                ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)];
            auto o = book.find(id);
            const Qty newQty = qtyDist(rng);
            const Status st = book.modify(id, newQty);
            ASSERT_EQ(st == Status::Ok, o.has_value());
            if (o) {
                ASSERT_EQ(book.find(id)->qty, newQty);
                ASSERT_EQ(restingTotal(book), before - o->qty + newQty);
                added += newQty;
                cancelled += o->qty;
            }
        }

        // The book must never be crossed.
        auto bid = book.bestBid();
        auto ask = book.bestAsk();
        if (bid && ask) {
            ASSERT_LT(*bid, *ask) << "crossed after op " << i;
        }

        if (cfg.checkStructureEveryOp || i % 1000 == 0) {
            std::string why;
            ASSERT_TRUE(book.checkInvariants(&why)) << "op " << i << ": " << why;
        }
    }

    std::string why;
    ASSERT_TRUE(book.checkInvariants(&why)) << why;
    // Global conservation: everything ever added is still resting, was
    // cancelled, or was consumed by a trade.
    ASSERT_EQ(added, restingTotal(book) + cancelled + removedByTrades);
}

} // namespace

TEST(InvariantTest, StructureHoldsAfterEveryOperation) {
    runRandom({/*seed=*/7, /*operations=*/5'000, /*checkStructureEveryOp=*/true});
}

TEST(RandomizedTest, HundredThousandOrdersFixedSeed) {
    runRandom({/*seed=*/42, /*operations=*/100'000, /*checkStructureEveryOp=*/false});
}

TEST(RandomizedTest, DifferentSeedsAgree) {
    for (std::uint64_t seed : {1u, 2u, 3u})
        runRandom({seed, 20'000, false});
}

TEST(RandomizedTest, SameSeedIsDeterministic) {
    auto run = [] {
        std::mt19937_64 rng(99);
        OrderBook book;
        std::vector<Trade> all, trades;
        for (OrderId id = 1; id <= 5'000; ++id) {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            trades.clear();
            book.addLimit(Order{id, side, static_cast<Price>(9990 + rng() % 21),
                                static_cast<Qty>(1 + rng() % 50)},
                          trades);
            all.insert(all.end(), trades.begin(), trades.end());
        }
        return all;
    };
    EXPECT_EQ(run(), run());
}
