// Throughput benchmark: 1M pre-generated orders (70% limit, 10% market,
// 20% cancel) in a narrow band around a mid price so orders actually match.
// Only the processing loop is timed. Runs 5 times and reports the median.
//
// Runs twice: the bare OrderBook, then the same orders through the RiskEngine
// (100 accounts, every pre-trade check enabled, live P&L and loss triggers).
//
//   orderbook_bench [orders] [runs]

#include "orderbook/OrderBook.h"
#include "orderbook/RiskEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

using namespace ob;

namespace {

enum class OpType : std::uint8_t { Limit, Market, Cancel };

struct Op {
    OpType type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
    AccountId account;
};

constexpr AccountId kAccounts = 100;

std::vector<Op> generate(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    constexpr Price kMid = 10'000; // $100.00
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<Price> offset(-5, 40);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::uniform_int_distribution<AccountId> account(0, kAccounts - 1);

    std::vector<Op> ops;
    ops.reserve(count);
    std::vector<OrderId> limitIds;
    limitIds.reserve(count);
    OrderId nextId = 1;

    for (std::size_t i = 0; i < count; ++i) {
        const int p = pct(rng);
        const Side side = coin(rng) ? Side::Buy : Side::Sell;
        const AccountId acct = account(rng);
        if (p < 70 || limitIds.empty()) {
            // Buys sit mostly below mid, sells mostly above, with a small overlap
            // so a steady fraction of orders cross the spread.
            const Price price = side == Side::Buy ? kMid - offset(rng) : kMid + offset(rng);
            ops.push_back({OpType::Limit, side, nextId, price, qty(rng), acct});
            limitIds.push_back(nextId++);
        } else if (p < 80) {
            ops.push_back({OpType::Market, side, nextId++, 0, qty(rng), acct});
        } else {
            // Cancel one of the recent orders; many will still be resting.
            const std::size_t window = std::min<std::size_t>(limitIds.size(), 10'000);
            std::uniform_int_distribution<std::size_t> pick(limitIds.size() - window,
                                                            limitIds.size() - 1);
            ops.push_back({OpType::Cancel, side, limitIds[pick(rng)], 0, 0, acct});
        }
    }
    return ops;
}

struct RunResult {
    double seconds;
    std::size_t trades;
    std::size_t rejected = 0;
    std::size_t kills = 0;
};

RunResult runBook(const std::vector<Op>& ops) {
    OrderBook book;
    book.reserve(ops.size()); // upper bound on live orders: no rehashing in the timed loop
    std::size_t tradeCount = 0;
    std::vector<Trade> trades; // one buffer reused for every order
    trades.reserve(1024);

    const auto start = std::chrono::steady_clock::now();
    for (const Op& op : ops) {
        trades.clear();
        switch (op.type) {
        case OpType::Limit:
            book.addLimit(Order{op.id, op.side, op.price, op.qty}, trades);
            break;
        case OpType::Market:
            book.addMarket(op.id, op.side, op.qty, trades);
            break;
        case OpType::Cancel:
            book.cancel(op.id);
            break;
        }
        tradeCount += trades.size();
    }
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration<double>(end - start).count(), tradeCount};
}

RunResult runRisk(const std::vector<Op>& ops) {
    RiskEngine risk;
    // Every check is enabled and evaluated on every order, but the limits are wide
    // enough for this workload (the book grows to ~380k resting orders, ~3.8k per
    // account) that rejections stay rare: this measures the cost of the checks,
    // not the speed of rejecting.
    RiskLimits limits;
    limits.maxOrderQty = 100;
    limits.maxOrderNotional = 20'000 * 100; // $20,000
    limits.maxPosition = 1'000'000;
    limits.maxOpenNotional = 100'000'000LL * 100; // $100M
    limits.priceBandBps = 500;                    // 5%
    limits.maxLoss = 1'000'000 * 100;             // $1M
    limits.selfTradePrevention = true;
    for (AccountId a = 0; a < kAccounts; ++a)
        risk.addAccount("acct" + std::to_string(a), limits);
    risk.reserve(ops.size());

    RunResult res{};
    std::vector<Trade> trades;
    trades.reserve(1024);

    const auto start = std::chrono::steady_clock::now();
    for (const Op& op : ops) {
        trades.clear();
        Status st = Status::Ok;
        switch (op.type) {
        case OpType::Limit:
            st = risk.submitLimit(op.account, Order{op.id, op.side, op.price, op.qty}, trades);
            break;
        case OpType::Market:
            st = risk.submitMarket(op.account, op.id, op.side, op.qty, trades);
            break;
        case OpType::Cancel:
            risk.cancel(op.id);
            break;
        }
        res.trades += trades.size();
        res.rejected += st != Status::Ok;
        res.kills += risk.lastKills().size();
    }
    const auto end = std::chrono::steady_clock::now();
    res.seconds = std::chrono::duration<double>(end - start).count();
    return res;
}

template <class Run>
double measure(const char* title, const std::vector<Op>& ops, int runs, Run run) {
    std::printf("%s\n", title);
    run(ops); // warm-up
    std::vector<double> times;
    RunResult last{};
    for (int r = 0; r < runs; ++r) {
        last = run(ops);
        times.push_back(last.seconds);
        std::printf("  run %d: %8.2f ms  %6.2f M orders/s\n", r + 1, last.seconds * 1e3,
                    static_cast<double>(ops.size()) / last.seconds / 1e6);
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    std::printf("  Trades per run:   %zu\n", last.trades);
    if (last.rejected || last.kills)
        std::printf("  Risk rejections:  %zu, kill-switch events: %zu\n", last.rejected,
                    last.kills);
    std::printf("  Median time:      %.2f ms\n", median * 1e3);
    std::printf("  Throughput:       %.2f M orders/sec\n",
                static_cast<double>(ops.size()) / median / 1e6);
    std::printf("  Avg latency:      %.1f ns/order\n\n",
                median * 1e9 / static_cast<double>(ops.size()));
    return median;
}

std::string cpuName() {
#if defined(__APPLE__)
    char buf[256];
    std::size_t len = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0)
        return buf;
#elif defined(__linux__)
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("model name", 0) == 0)
            return line.substr(line.find(':') + 2);
#endif
    return "unknown CPU";
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const int runs = argc > 2 ? std::atoi(argv[2]) : 5;

    const auto ops = generate(count, /*seed=*/12345);
    std::printf("CPU:     %s\n", cpuName().c_str());
    std::printf("Orders:  %zu (70%% limit, 10%% market, 20%% cancel), %d runs\n\n", ops.size(),
                runs);

    const double book = measure("Order book only", ops, runs, runBook);
    const double risk =
        measure("Order book + risk engine (100 accounts, all checks on)", ops, runs, runRisk);
    std::printf("Risk overhead:    %.1f ns/order\n",
                (risk - book) * 1e9 / static_cast<double>(ops.size()));
    return 0;
}
