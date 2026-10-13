# Limit Order Book, Matching Engine & Real-Time Risk

[![CI](https://github.com/RodricDib06/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/RodricDib06/limit-order-book/actions/workflows/ci.yml)

A C++17 limit order book that matches buy and sell orders by **price-time priority**, with a **built-in real-time risk engine** in front of it. Every order passes seven pre-trade checks, every fill updates both traders' positions and P&L, and a **kill switch** stops any trader whose losses pass their limit, even when the loss comes from other people's trades moving the price.

On one core of an Apple M2, it handles **~20 million orders/sec** for the bare book and **~8.2 million orders/sec** with every risk check enabled for 100 trading accounts.

An order book is the core of every exchange: it keeps all outstanding buy orders (bids) and sell orders (asks) sorted by price. When the highest bid meets or beats the lowest ask, a trade happens. Everything else waits in the book. Trading firms and exchanges never let orders reach the book unchecked; the risk layer here models that gate.

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
- **Risk engine** with trader accounts, pre-trade limits, live P&L, and a kill switch ([details below](#real-time-risk-engine))

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
./build/orderbook data/risk_demo.csv         # risk demo: every check plus the kill switch
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

## Real-time risk engine

`RiskEngine` wraps the order book. Orders are submitted for a trading **account**, checked against that account's limits, and only then reach the book. The core book is untouched, so it can still be benchmarked on its own.

### Pre-trade checks

Each check is optional per account. If any check fails, the order is rejected before it touches the book.

| Check | Rejects an order that... |
|---|---|
| Max order quantity | is bigger than the account's per-order share limit (fat finger) |
| Max order value | is worth more than the per-order dollar limit. For a market order, the value is estimated by walking the book |
| Price band | is priced more than N basis points away from the last trade (or the mid price) |
| Position limit | could take the position past ±N shares **in the worst case**, counting every open order as filled |
| Credit limit | would push the total value of the account's resting orders over its limit |
| Self-trade prevention | **would actually** trade with the account's own resting order |
| Kill switch | comes from a blocked account |

Self-trade prevention is exact. A read-only simulation of the match walks the resting orders the incoming order would fill, in price-time order, and rejects only if one of them is the trader's own. An order that would be filled entirely by other traders first is allowed, and an order that doesn't cross the spread costs O(1). A simpler price-only check rejected 18% of orders in the benchmark. The exact check rejects 0.3%.

### Positions and P&L

Every fill updates **both** counterparties: signed position, cash, and average cost. Reducing a position realizes P&L against the average cost, and flipping from long to short starts a new cost basis. Open positions are marked to the last trade price:

```
total P&L = cash + position × mark      realized = total − unrealized
```

All amounts are exact integers in cents. Every trade has a buyer and a seller, so **positions and cash across all accounts always sum to zero**. The tests check this after every operation.

### Loss limits and the kill switch

When an account's total P&L drops below `-maxLoss`, the kill switch cancels all of its resting orders and blocks it. Most losses come from *other* traders moving the price, so the check can't wait for the account's own next order.

Checking every account after every trade would cost O(accounts) per trade. Instead, each account's breach point is turned into a single **trigger price**, like a stop order. For a position `p` and cash `c`:

```
breach  ⇔  c + p × mark < −maxLoss
long  (p > 0):  breach when mark < ⌈(−maxLoss − c) / p⌉
short (p < 0):  breach when mark > ⌊(maxLoss + c) / |p|⌋
```

Trigger prices live in two ordered maps, one for long positions and one for short. When the price moves, only the triggers that crossed are visited, so it costs **O(log A)** instead of scanning all A accounts. When a fill changes an account's position, its trigger node is re-keyed in place with C++17 `std::map::extract`, which avoids a heap allocation on the fill path. A blocked account can only be unblocked once it's back inside its limit, for example after the limit is raised.

### Risk demo

```
$ ./build/orderbook data/risk_demo.csv
...
ACCEPTED #3 [alice] BUY 200 @ 101.00
TRADE    buy #3 / sell #2  200 @ 101.00
ACCEPTED #4 [alice] SELL 100 @ 104.00
RESTING  #4 100 @ 104.00
REJECTED #5 [alice]: risk: order quantity above limit
REJECTED #6 [alice]: risk: position limit would be exceeded
REJECTED #7 [alice]: risk: price outside allowed band
ACCEPTED #8 [alice] BUY 50 @ 100.00
RESTING  #8 50 @ 100.00
REJECTED #9 [alice]: risk: would trade with own order
ACCEPTED #10 [carol] BUY 150 @ 98.00
RESTING  #10 150 @ 98.00
REJECTED #11 [carol]: risk: credit limit would be exceeded
ACCEPTED #12 [bob] SELL 400 @ MARKET
TRADE    buy #8 / sell #12  50 @ 100.00
TRADE    buy #1 / sell #12  350 @ 99.00
KILLED   [alice] max loss breached, 1 order(s) cancelled, account blocked
REJECTED #13 [alice]: risk: account is blocked (kill switch)
...
Mark price: 99.00   P&L in dollars
ACCOUNT        POS  AVG COST   REALIZED    UNREAL.      TOTAL    OPEN B/S  STATUS
house            0         -       0.00       0.00       0.00         0/0  active
alice          250    100.80       0.00    -450.00    -450.00         0/0  BLOCKED (max loss breached)
bob           -400     99.12       0.00      50.00      50.00         0/0  active
carol            0         -       0.00       0.00       0.00       150/0  active
mm             150     99.00     400.00       0.00     400.00     150/300  active
```

In the interactive CLI, `account alice maxqty=500 maxpos=300 maxloss=400 band=500` creates an account and switches to it. `risk` prints the table above, and `kill alice` / `unblock alice` work the kill switch by hand. In CSV files, add an `account,<name>,key=value,...` row, then put the account name in the sixth column of an order row. The keys are `maxqty`, `maxpos` (shares), `maxvalue`, `credit`, `maxloss` (dollars), `band` (basis points), and `stp=on|off`. New accounts start with self-trade prevention on. The default `house` account has no limits.

## Design

The code has five parts. `Order`/`Trade` hold the data, `PriceLevel` is a FIFO queue at one price, `OrderBook` stores both sides and runs the matching loop, `RiskEngine` checks orders and keeps positions in front of it, and `MatchingEngine` is the text front end (CLI and CSV parsing, price formatting).

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

There are **116 tests** (114 GoogleTest cases plus 2 CLI replay runs), run on every push with GCC and Clang, plus a separate AddressSanitizer + UBSan job:

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
| Risk checks | Each pre-trade check at its exact boundary; exact self-trade prevention (price *and* queue position); risk-checked modify |
| P&L | Average cost, realized P&L on partial closes, flipping long → short, mark-to-market from other traders' trades |
| Kill switch | Loss limits breached by market moves (long and short), by a realized loss, by the account's own fill, and by lowering the limit; unblock is refused while still in breach |
| **Risk randomized** | 30,000 operations across 12 accounts with mixed limits: no account's worst-case position ever passes its limit, positions and cash sum to zero, and risk tracking always matches the book |

`OrderBook::checkInvariants()` and `RiskEngine::checkInvariants()` check the whole structure and run after every test.

## Benchmark

`bench/Benchmark.cpp` processes **1,000,000 pre-generated orders** with a fixed seed: 70% limit, 10% market, and 20% cancel, with prices in a narrow band around $100.00 so that about 29% of orders trade. Only the processing loop is timed, using `std::chrono::steady_clock`. The benchmark builds in Release mode (`-O2`/`-O3`), runs 5 times, and reports the median.

**Result on an Apple M2: 20.0 M orders/sec, 50 ns average per order.**

### Risk engine throughput

The same 1M orders, sent through `RiskEngine` from 100 accounts with **every check enabled**. The limits are wide enough that only 0.3% of orders are rejected, so this measures the cost of the checks rather than the speed of rejecting. It produces 286,623 trades, against 291,684 for the bare book.

| Configuration | Throughput | Avg latency |
|---|---:|---:|
| Order book only | 20.0 M/s | 50 ns |
| Order book + risk engine | **8.2 M/s** | **122 ns** (+73 ns for 7 checks, P&L, and loss triggers) |

The risk layer was profiled and optimized. The first version ran at 5.7 M/s. Profiling showed about 30% of the time in `malloc`/`free`, from re-inserting loss-trigger nodes on every fill and from per-account price maps. Re-keying trigger nodes in place with `extract()` and dropping the price maps, which exact self-trade prevention made unnecessary, brought it to 8.2 M/s (+44%).

### Order book optimizations (before / after)

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
include/orderbook/   Types.h, OrderPool.h, PriceLevel.h, OrderBook.h, RiskEngine.h, MatchingEngine.h
src/                 OrderBook.cpp, RiskEngine.cpp, MatchingEngine.cpp, main.cpp (CLI + CSV demo)
tests/               OrderBookTest.cpp, MatchingEngineTest.cpp, StressTest.cpp, RiskEngineTest.cpp
bench/               Benchmark.cpp (book only, and book + risk engine)
data/                sample_orders.csv, risk_demo.csv
.github/workflows/   ci.yml (GCC + Clang matrix, ASan/UBSan, clang-format)
```

## Possible extensions

- Multiple instruments behind a symbol router
- IOC / FOK / stop orders
- Top-of-book market data feed (observer pattern)
- A flat price-indexed array instead of `std::map` for a bounded tick range
- Python bindings with pybind11
