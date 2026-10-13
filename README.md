# Limit Order Book & Matching Engine

[![CI](https://github.com/RodricDib06/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/RodricDib06/limit-order-book/actions/workflows/ci.yml)

A C++17 limit order book that accepts buy and sell orders, matches them by **price-time priority**, and reports every trade. It handles **~20 million orders per second** on one core (Apple M2).

An order book is the core of every exchange: it keeps all outstanding buy orders (bids) and sell orders (asks) sorted by price. When the highest bid meets or beats the lowest ask, a trade happens. Everything else waits in the book.

## Features

- **Limit orders**: buy or sell at a set price or better; any leftover rests in the book
- **Market orders**: sweep the book at the best available prices; any leftover is dropped
- **Cancel** by order ID in O(1) average time
- **Modify** quantity: a decrease keeps the order's place in line, an increase sends it to the back
- **Replace** price: treated as cancel + new order (it loses priority and may trade immediately)
- **Partial fills** across multiple price levels, with a trade record for every fill (buy ID, sell ID, price, quantity)
- **Validation**: rejects zero quantity, zero or negative price, duplicate live IDs, and unknown IDs
- **Market data**: best bid, best ask, spread, and the top N levels per side
- **CLI and CSV replay** so anyone can run a demo

Two rules are settled up front:

- **Prices are integer ticks** (`int64_t` cents), never `double`. `1.13 * 100` is `112.99999999999999` in binary floating point, so floating-point prices can make equal prices compare unequal. Text prices are parsed straight to ticks without going through floating point.
- **A trade executes at the resting order's price**, not at the incoming order's price.

## Build and run

Requires CMake 3.16+ and a C++17 compiler (GCC or Clang). GoogleTest is downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # run all tests
./build/orderbook data/sample_orders.csv     # replay the sample CSV
./build/orderbook                            # interactive mode
./build/orderbook_bench                      # throughput benchmark
```

### Interactive mode

```
$ ./build/orderbook
Order book ready. Type 'help' for commands.
> sell 50 @ 101
ACCEPTED #1 SELL 50 @ 101.00
RESTING  #1 50 @ 101.00
> sell 30 @ 102
ACCEPTED #2 SELL 30 @ 102.00
RESTING  #2 30 @ 102.00
> buy 70 @ 102
ACCEPTED #3 BUY 70 @ 102.00
TRADE    buy #3 / sell #1  50 @ 101.00
TRADE    buy #3 / sell #2  20 @ 102.00
> buy 40 @ 99.50
ACCEPTED #4 BUY 40 @ 99.50
RESTING  #4 40 @ 99.50
> book
+--------------------------------------------+
| ASK       102.00  x       10  (1 order)    |
| ------- spread 2.50 -------                |
| BID        99.50  x       40  (1 order)    |
+--------------------------------------------+
> cancel 4
CANCELED #4
```

Commands: `buy|sell <qty> @ <price>` (limit), `buy|sell <qty>` (market), `cancel <id>`, `modify <id> <qty>`, `book`, `help`, `quit`.

### CSV replay

The columns are `type,id,side,price,qty`, where `type` is `limit`, `market`, `cancel`, or `modify` (see [data/sample_orders.csv](data/sample_orders.csv)).

```
$ ./build/orderbook data/sample_orders.csv
...
ACCEPTED #8 BUY 70 @ 102.00
TRADE    buy #8 / sell #1  50 @ 101.00
TRADE    buy #8 / sell #2  20 @ 102.00
ACCEPTED #9 SELL 70 @ 100.00
TRADE    buy #5 / sell #9  60 @ 100.00
TRADE    buy #6 / sell #9  10 @ 100.00
CANCELED #3
MODIFIED #4 qty 10
ACCEPTED #10 BUY 50 @ MARKET
TRADE    buy #10 / sell #2  10 @ 102.00
TRADE    buy #10 / sell #7  20 @ 104.00
DROPPED  #10 20 unfilled
REJECTED #11: invalid quantity
REJECTED #6: duplicate order id
REJECTED #999: unknown order id
...
+--------------------------------------------+
| ASK       103.25  x       60  (1 order)    |
| ASK       101.50  x       65  (2 orders)   |
| ------- spread 1.50 -------                |
| BID       100.00  x       40  (2 orders)   |
| BID        99.00  x       10  (1 order)    |
+--------------------------------------------+
6 trades, 6 orders resting
```

## Design

The code has four parts. `Order`/`Trade` hold the data, `PriceLevel` is a FIFO queue at one price, `OrderBook` stores both sides and runs the matching loop, and `MatchingEngine` is the text front end (CLI and CSV parsing, price formatting).

| Part | Container | Why |
|---|---|---|
| Bid side | `std::map<Price, PriceLevel, std::greater<>>` | Sorted high to low, so `begin()` is the best bid |
| Ask side | `std::map<Price, PriceLevel>` | Sorted low to high, so `begin()` is the best ask |
| Orders at one price | Intrusive doubly linked list over a pooled slab (`OrderPool`) | FIFO order, O(1) unlink, no allocation per order |
| Order lookup | `std::unordered_map<OrderId, Locator>` | O(1) cancel: the locator holds the side, price, level pointer, and node handle |

**Matching loop.** An incoming buy looks at the best ask. While the ask's price is at or below the buy's limit (or the order is a market order), it fills `min(incoming, resting)` at the resting price. It removes filled resting orders and empty levels, and moves to the next level. A limit order's leftover rests in the book, and a market order's leftover is dropped. Sells run the same loop against the bids. One templated function handles both sides, using the map's own comparator to decide whether a level still crosses.

| Operation | Complexity |
|---|---|
| Add at a new price level | O(log P), where P = number of price levels |
| Add at an existing level, cancel, each fill | O(1) average |
| Best bid / best ask | O(1) |
| Top-N depth | O(N) |

## Testing

There are **77 tests**, run on every push with GCC and Clang, plus a separate AddressSanitizer + UBSan job:

| Area | Examples |
|---|---|
| Basic matching | Buy at 101 meets sell at 101 → one trade for the full quantity; trades execute at the resting price |
| No cross | Bid 99, ask 101 → no trade, both rest |
| Price / time priority | The lowest ask fills first; at the same price, the earlier order fills first |
| Partial fills | Buy 70 against asks of 50 + 30 → 2 trades, 10 left resting |
| Market orders | Sweeps several levels; leftover is dropped; never rests |
| Cancel / modify | A cancelled order never fills; a filled or unknown ID returns `UnknownId`; a decrease keeps priority |
| Validation | Zero quantity, zero or negative price, and duplicate IDs are rejected and leave the book untouched |
| Book state | Empty levels are removed; best bid/ask update; top-5 depth |
| Parsing | Decimal prices → ticks without floating point; malformed CLI and CSV input |
| **Invariants** | After *every* operation: the book is never crossed, level totals match their orders, the index matches the book, and quantity is conserved |
| **Randomized** | 100,000 random orders with a fixed seed, plus multiple seeds and a determinism check |

`OrderBook::checkInvariants()` checks the whole structure and runs after every test.

## Benchmark

`bench/Benchmark.cpp` processes **1,000,000 pre-generated orders** with a fixed seed: 70% limit, 10% market, and 20% cancel, with prices in a narrow band around $100.00 so that about 29% of orders trade. Only the processing loop is timed, using `std::chrono::steady_clock`. The benchmark builds in Release mode (`-O2`/`-O3`), runs 5 times, and reports the median.

**Result on an Apple M2: 20.0 M orders/sec, 50 ns average per order.**

### Optimizations (before / after)

Each step is a separate commit. All four builds were benchmarked back to back in 5 interleaved rounds on the same machine, and the medians are shown:

| Step | Throughput | Avg latency | vs. baseline |
|---|---:|---:|---:|
| Baseline: `std::list` per level, new `vector<Trade>` per order | 9.4 M/s | 106 ns | — |
| + `reserve()` the order index up front (no rehashing) | 13.8 M/s | 73 ns | +46% |
| + reuse one trade buffer instead of a vector per order | 15.4 M/s | 65 ns | +64% |
| + pooled intrusive list instead of `std::list` (no allocation per order) | **20.0 M/s** | **50 ns** | **+112%** |

Throughput depends on the machine; run `./build/orderbook_bench` to measure yours.

## Project layout

```
include/orderbook/   Types.h, OrderPool.h, PriceLevel.h, OrderBook.h, MatchingEngine.h
src/                 OrderBook.cpp, MatchingEngine.cpp, main.cpp (CLI + CSV demo)
tests/               OrderBookTest.cpp, MatchingEngineTest.cpp, StressTest.cpp (GoogleTest)
bench/               Benchmark.cpp
data/                sample_orders.csv
.github/workflows/   ci.yml (GCC + Clang matrix, ASan/UBSan, clang-format)
```

## Possible extensions

- Multiple instruments behind a symbol router
- IOC / FOK / stop orders
- Top-of-book market data feed (observer pattern)
- A flat price-indexed array instead of `std::map` for a bounded tick range
- Python bindings with pybind11
