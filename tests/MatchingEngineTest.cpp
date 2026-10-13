#include "orderbook/MatchingEngine.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace ob;

// ---- Price parsing and formatting -------------------------------------------

TEST(PriceParsing, ParsesWholeAndFractionalPrices) {
    EXPECT_EQ(parsePrice("101.50"), 10150);
    EXPECT_EQ(parsePrice("101.5"), 10150);
    EXPECT_EQ(parsePrice("101"), 10100);
    EXPECT_EQ(parsePrice("0.07"), 7);
    EXPECT_EQ(parsePrice(".25"), 25);
    EXPECT_EQ(parsePrice("$99.99"), 9999);
    EXPECT_EQ(parsePrice(" 12.30 "), 1230);
}

TEST(PriceParsing, AvoidsFloatingPointRounding) {
    // 0.1 + 0.2 != 0.3 in binary floating point; integer ticks are exact.
    EXPECT_EQ(*parsePrice("0.10") + *parsePrice("0.20"), *parsePrice("0.30"));
    EXPECT_EQ(parsePrice("1.13"), 113); // 1.13 * 100 == 112.99999999999999 as a double
}

TEST(PriceParsing, RejectsMalformedInput) {
    EXPECT_FALSE(parsePrice(""));
    EXPECT_FALSE(parsePrice("abc"));
    EXPECT_FALSE(parsePrice("1.2.3"));
    EXPECT_FALSE(parsePrice("101.505")); // finer than one tick
    EXPECT_FALSE(parsePrice("."));
    EXPECT_FALSE(parsePrice("1e5"));
    EXPECT_FALSE(parsePrice("99999999999999999999"));
}

TEST(PriceParsing, NegativeParsesSoBookCanReject) {
    EXPECT_EQ(parsePrice("-5.25"), -525);
}

TEST(PriceFormatting, FormatsTicksAsDecimal) {
    EXPECT_EQ(formatPrice(10150), "101.50");
    EXPECT_EQ(formatPrice(10105), "101.05");
    EXPECT_EQ(formatPrice(7), "0.07");
    EXPECT_EQ(formatPrice(0), "0.00");
    EXPECT_EQ(formatPrice(-525), "-5.25");
}

TEST(PriceFormatting, RoundTripsThroughParse) {
    for (Price p : {Price{1}, Price{99}, Price{100}, Price{10150}, Price{123456789}})
        EXPECT_EQ(parsePrice(formatPrice(p)), p);
}

// ---- Command-line interface -------------------------------------------------

namespace {
class EngineTest : public ::testing::Test {
protected:
    std::ostringstream out;
    MatchingEngine engine{out};
};
} // namespace

TEST_F(EngineTest, LimitCommandRestsOrder) {
    EXPECT_TRUE(engine.execute("buy 100 @ 101.50"));
    EXPECT_EQ(engine.book().bestBid(), 10150);
    EXPECT_NE(out.str().find("ACCEPTED #1 BUY 100 @ 101.50"), std::string::npos);
    EXPECT_NE(out.str().find("RESTING  #1"), std::string::npos);
}

TEST_F(EngineTest, CompactSyntaxAndCaseInsensitive) {
    engine.execute("SELL 10@102");
    EXPECT_EQ(engine.book().bestAsk(), 10200);
}

TEST_F(EngineTest, CommandsTradeAndReport) {
    engine.execute("sell 50 @ 101");
    engine.execute("buy 20 @ 101");
    ASSERT_EQ(engine.lastTrades().size(), 1u);
    EXPECT_EQ(engine.lastTrades()[0], (Trade{2, 1, 10100, 20}));
    EXPECT_NE(out.str().find("TRADE    buy #2 / sell #1  20 @ 101.00"), std::string::npos);
}

TEST_F(EngineTest, MarketCommandReportsDroppedRemainder) {
    engine.execute("sell 10 @ 100");
    engine.execute("buy 25");
    EXPECT_EQ(engine.lastTrades().size(), 1u);
    EXPECT_NE(out.str().find("DROPPED  #2 15 unfilled"), std::string::npos);
    EXPECT_TRUE(engine.book().empty());
}

TEST_F(EngineTest, CancelAndModifyCommands) {
    engine.execute("buy 10 @ 99");
    engine.execute("modify 1 4");
    EXPECT_EQ(engine.book().find(1)->qty, 4u);
    engine.execute("cancel 1");
    EXPECT_TRUE(engine.book().empty());
    engine.execute("cancel 1");
    EXPECT_NE(out.str().find("REJECTED #1: unknown order id"), std::string::npos);
}

TEST_F(EngineTest, BookCommandPrintsLadder) {
    engine.execute("buy 10 @ 99");
    engine.execute("sell 5 @ 101");
    engine.execute("book");
    const std::string s = out.str();
    EXPECT_NE(s.find("ASK       101.00  x        5  (1 order)"), std::string::npos);
    EXPECT_NE(s.find("BID        99.00  x       10  (1 order)"), std::string::npos);
    EXPECT_NE(s.find("spread 2.00"), std::string::npos);
}

TEST_F(EngineTest, BadCommandsReportErrors) {
    EXPECT_TRUE(engine.execute("buy"));
    EXPECT_TRUE(engine.execute("buy ten @ 100"));
    EXPECT_TRUE(engine.execute("sell 10 @ 1.234"));
    EXPECT_TRUE(engine.execute("explode"));
    EXPECT_TRUE(engine.book().empty());
    EXPECT_NE(out.str().find("ERROR"), std::string::npos);
}

TEST_F(EngineTest, InvalidOrderIsRejectedNotCrashed) {
    engine.execute("buy 0 @ 100");
    engine.execute("buy 5 @ -1");
    EXPECT_TRUE(engine.book().empty());
    EXPECT_NE(out.str().find("invalid quantity"), std::string::npos);
    EXPECT_NE(out.str().find("invalid price"), std::string::npos);
}

TEST_F(EngineTest, QuitStopsLoop) {
    EXPECT_FALSE(engine.execute("quit"));
    EXPECT_FALSE(engine.execute("exit"));
    EXPECT_TRUE(engine.execute(""));
}

// ---- CSV input --------------------------------------------------------------

TEST_F(EngineTest, CsvWorkedExample) {
    EXPECT_TRUE(engine.executeCsvLine("type,id,side,price,qty"));
    EXPECT_TRUE(engine.executeCsvLine("# comment"));
    EXPECT_TRUE(engine.executeCsvLine("limit,1,sell,101.00,50"));
    EXPECT_TRUE(engine.executeCsvLine("limit,2,sell,102.00,30"));
    EXPECT_TRUE(engine.executeCsvLine("limit,3,buy,102.00,70"));
    ASSERT_EQ(engine.lastTrades().size(), 2u);
    EXPECT_EQ(engine.lastTrades()[0], (Trade{3, 1, 10100, 50}));
    EXPECT_EQ(engine.lastTrades()[1], (Trade{3, 2, 10200, 20}));
    EXPECT_EQ(engine.book().find(2)->qty, 10u);
}

TEST_F(EngineTest, CsvMarketCancelModify) {
    engine.executeCsvLine("limit,1,sell,100,10");
    engine.executeCsvLine("limit,2,sell,101,10");
    EXPECT_TRUE(engine.executeCsvLine("modify,2,,,4"));
    EXPECT_TRUE(engine.executeCsvLine("cancel,1,,,"));
    EXPECT_TRUE(engine.executeCsvLine("market,3,buy,,10"));
    ASSERT_EQ(engine.lastTrades().size(), 1u);
    EXPECT_EQ(engine.lastTrades()[0], (Trade{3, 2, 10100, 4}));
    EXPECT_EQ(engine.totalTrades(), 1u);
}

TEST_F(EngineTest, CsvMalformedRowsReturnFalse) {
    EXPECT_FALSE(engine.executeCsvLine("limit,x,buy,100,10"));
    EXPECT_FALSE(engine.executeCsvLine("limit,1,up,100,10"));
    EXPECT_FALSE(engine.executeCsvLine("limit,1,buy,abc,10"));
    EXPECT_FALSE(engine.executeCsvLine("teleport,1,buy,100,10"));
    EXPECT_FALSE(engine.executeCsvLine("modify,1"));
    EXPECT_TRUE(engine.book().empty());
}

TEST_F(EngineTest, CliIdsContinueAfterCsvIds) {
    engine.executeCsvLine("limit,41,buy,100,10");
    engine.execute("buy 5 @ 99");
    EXPECT_TRUE(engine.book().contains(42));
}
