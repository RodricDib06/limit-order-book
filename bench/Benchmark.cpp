// Throughput benchmark: 1M pre-generated orders (70% limit, 10% market,
// 20% cancel) in a narrow band around a mid price so orders actually match.
// Only the processing loop is timed. Runs 5 times and reports the median.
//
//   orderbook_bench [orders] [runs]

#include "orderbook/OrderBook.h"

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
};

std::vector<Op> generate(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    constexpr Price kMid = 10'000; // $100.00
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<Price> offset(-5, 40);
    std::uniform_int_distribution<Qty> qty(1, 100);

    std::vector<Op> ops;
    ops.reserve(count);
    std::vector<OrderId> limitIds;
    limitIds.reserve(count);
    OrderId nextId = 1;

    for (std::size_t i = 0; i < count; ++i) {
        const int p = pct(rng);
        const Side side = coin(rng) ? Side::Buy : Side::Sell;
        if (p < 70 || limitIds.empty()) {
            // Buys sit mostly below mid, sells mostly above, with a small overlap
            // so a steady fraction of orders cross the spread.
            const Price price = side == Side::Buy ? kMid - offset(rng) : kMid + offset(rng);
            ops.push_back({OpType::Limit, side, nextId, price, qty(rng)});
            limitIds.push_back(nextId++);
        } else if (p < 80) {
            ops.push_back({OpType::Market, side, nextId++, 0, qty(rng)});
        } else {
            // Cancel one of the recent orders; many will still be resting.
            const std::size_t window = std::min<std::size_t>(limitIds.size(), 10'000);
            std::uniform_int_distribution<std::size_t> pick(limitIds.size() - window,
                                                            limitIds.size() - 1);
            ops.push_back({OpType::Cancel, side, limitIds[pick(rng)], 0, 0});
        }
    }
    return ops;
}

struct RunResult {
    double seconds;
    std::size_t trades;
};

RunResult runOnce(const std::vector<Op>& ops) {
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

    runOnce(ops); // warm-up

    std::vector<double> times;
    std::size_t trades = 0;
    for (int r = 0; r < runs; ++r) {
        const RunResult res = runOnce(ops);
        times.push_back(res.seconds);
        trades = res.trades;
        std::printf("  run %d: %8.2f ms  %6.2f M orders/s\n", r + 1, res.seconds * 1e3,
                    static_cast<double>(ops.size()) / res.seconds / 1e6);
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];

    std::printf("\nTrades per run:   %zu\n", trades);
    std::printf("Median time:      %.2f ms\n", median * 1e3);
    std::printf("Throughput:       %.2f M orders/sec\n",
                static_cast<double>(ops.size()) / median / 1e6);
    std::printf("Avg latency:      %.1f ns/order\n",
                median * 1e9 / static_cast<double>(ops.size()));
    return 0;
}
