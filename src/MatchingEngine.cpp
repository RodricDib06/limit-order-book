#include "orderbook/MatchingEngine.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>

namespace ob {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find(delim, start);
        parts.push_back(trim(s.substr(start, pos - start)));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return parts;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

template <class T> std::optional<T> parseUnsigned(std::string_view s) {
    T value{};
    if (s.empty())
        return std::nullopt;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || ptr != s.data() + s.size())
        return std::nullopt;
    return value;
}

std::optional<Side> parseSide(std::string_view s) {
    const std::string w = lower(s);
    if (w == "buy" || w == "b" || w == "bid")
        return Side::Buy;
    if (w == "sell" || w == "s" || w == "ask")
        return Side::Sell;
    return std::nullopt;
}

} // namespace

std::optional<Price> parsePrice(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '$')
        text.remove_prefix(1);
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    if (text.empty())
        return std::nullopt;

    const std::size_t dot = text.find('.');
    std::string_view whole = text.substr(0, dot);
    std::string_view frac =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() && frac.empty())
        return std::nullopt;
    if (frac.size() > 2)
        return std::nullopt; // finer than one tick

    Price units = 0;
    if (!whole.empty()) {
        auto parsed = parseUnsigned<std::uint64_t>(whole);
        constexpr auto kMaxUnits =
            static_cast<std::uint64_t>(std::numeric_limits<Price>::max() / kTicksPerUnit) - 1;
        if (!parsed || *parsed > kMaxUnits)
            return std::nullopt;
        units = static_cast<Price>(*parsed);
    }

    Price cents = 0;
    if (!frac.empty()) {
        auto parsed = parseUnsigned<unsigned>(frac);
        if (!parsed)
            return std::nullopt;
        cents = static_cast<Price>(*parsed) * (frac.size() == 1 ? 10 : 1);
    }

    const Price ticks = units * kTicksPerUnit + cents;
    return negative ? -ticks : ticks;
}

std::string formatPrice(Price ticks) {
    std::string sign = ticks < 0 ? "-" : "";
    // Work in unsigned space so the minimum int64 value is formatted correctly.
    const std::uint64_t mag =
        ticks < 0 ? 0 - static_cast<std::uint64_t>(ticks) : static_cast<std::uint64_t>(ticks);
    const std::uint64_t cents = mag % kTicksPerUnit;
    return sign + std::to_string(mag / kTicksPerUnit) + (cents < 10 ? ".0" : ".") +
           std::to_string(cents);
}

MatchingEngine::MatchingEngine(std::ostream& out) : out_(out) {}

void MatchingEngine::noteId(OrderId id) {
    if (id >= nextId_)
        nextId_ = id + 1;
}

void MatchingEngine::reportTrades() {
    for (const Trade& t : trades_)
        out_ << "TRADE    buy #" << t.buyId << " / sell #" << t.sellId << "  " << t.qty << " @ "
             << formatPrice(t.price) << '\n';
    totalTrades_ += trades_.size();
}

void MatchingEngine::reportReject(OrderId id, Status status) {
    out_ << "REJECTED #" << id << ": " << toString(status) << '\n';
}

void MatchingEngine::submitLimit(OrderId id, Side side, Price price, Qty qty) {
    trades_.clear();
    const Status st = book_.addLimit(Order{id, side, price, qty}, trades_);
    if (st != Status::Ok)
        return reportReject(id, st);
    out_ << "ACCEPTED #" << id << ' ' << toString(side) << ' ' << qty << " @ " << formatPrice(price)
         << '\n';
    reportTrades();
    if (auto resting = book_.find(id))
        out_ << "RESTING  #" << id << ' ' << resting->qty << " @ " << formatPrice(price) << '\n';
}

void MatchingEngine::submitMarket(OrderId id, Side side, Qty qty) {
    trades_.clear();
    const Status st = book_.addMarket(id, side, qty, trades_);
    if (st != Status::Ok)
        return reportReject(id, st);
    out_ << "ACCEPTED #" << id << ' ' << toString(side) << ' ' << qty << " @ MARKET\n";
    reportTrades();
    Qty filled = 0;
    for (const Trade& t : trades_)
        filled += t.qty;
    if (filled < qty)
        out_ << "DROPPED  #" << id << ' ' << (qty - filled) << " unfilled\n";
}

void MatchingEngine::submitCancel(OrderId id) {
    const Status st = book_.cancel(id);
    if (st != Status::Ok)
        return reportReject(id, st);
    out_ << "CANCELED #" << id << '\n';
}

void MatchingEngine::submitModify(OrderId id, Qty qty) {
    const Status st = book_.modify(id, qty);
    if (st != Status::Ok)
        return reportReject(id, st);
    out_ << "MODIFIED #" << id << " qty " << qty << '\n';
}

bool MatchingEngine::execute(std::string_view line) {
    // Allow "buy 100@101.50" as well as "buy 100 @ 101.50".
    std::string spaced;
    for (char c : line) {
        if (c == '@')
            spaced += " @ ";
        else
            spaced += c;
    }
    const auto w = words(spaced);
    if (w.empty())
        return true;
    const std::string cmd = lower(w[0]);

    auto usage = [this](const char* msg) { out_ << "ERROR: " << msg << " (type 'help')\n"; };

    if (cmd == "quit" || cmd == "exit" || cmd == "q")
        return false;
    if (cmd == "help" || cmd == "?") {
        printHelp(out_);
    } else if (cmd == "book") {
        printBook();
    } else if (auto side = parseSide(cmd)) {
        // buy <qty> [@ <price>]
        auto qty = w.size() >= 2 ? parseUnsigned<Qty>(w[1]) : std::nullopt;
        if (!qty) {
            usage("expected: buy|sell <qty> [@ <price>]");
        } else if (w.size() == 2 || (w.size() == 3 && lower(w[2]) == "market")) {
            submitMarket(nextId_++, *side, *qty);
        } else if (w.size() == 4 && w[2] == "@") {
            auto price = parsePrice(w[3]);
            if (!price)
                usage("bad price");
            else
                submitLimit(nextId_++, *side, *price, *qty);
        } else {
            usage("expected: buy|sell <qty> [@ <price>]");
        }
    } else if (cmd == "cancel") {
        auto id = w.size() == 2 ? parseUnsigned<OrderId>(w[1]) : std::nullopt;
        if (!id)
            usage("expected: cancel <id>");
        else
            submitCancel(*id);
    } else if (cmd == "modify") {
        auto id = w.size() == 3 ? parseUnsigned<OrderId>(w[1]) : std::nullopt;
        auto qty = w.size() == 3 ? parseUnsigned<Qty>(w[2]) : std::nullopt;
        if (!id || !qty)
            usage("expected: modify <id> <qty>");
        else
            submitModify(*id, *qty);
    } else {
        usage("unknown command");
    }
    return true;
}

bool MatchingEngine::executeCsvLine(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() == '#')
        return true;
    const auto f = split(line, ',');
    const std::string type = lower(f[0]);
    if (type == "type")
        return true; // header row

    auto field = [&f](std::size_t i) { return i < f.size() ? f[i] : std::string_view{}; };
    auto id = parseUnsigned<OrderId>(field(1));
    if (!id)
        return false;
    noteId(*id);

    if (type == "limit") {
        auto side = parseSide(field(2));
        auto price = parsePrice(field(3));
        auto qty = parseUnsigned<Qty>(field(4));
        if (!side || !price || !qty)
            return false;
        submitLimit(*id, *side, *price, *qty);
    } else if (type == "market") {
        auto side = parseSide(field(2));
        auto qty = parseUnsigned<Qty>(field(4));
        if (!side || !qty)
            return false;
        submitMarket(*id, *side, *qty);
    } else if (type == "cancel") {
        submitCancel(*id);
    } else if (type == "modify") {
        auto qty = parseUnsigned<Qty>(field(4));
        if (!qty)
            return false;
        submitModify(*id, *qty);
    } else {
        return false;
    }
    return true;
}

void MatchingEngine::printBook(std::size_t levels) const {
    constexpr std::size_t kWidth = 44; // inner width of the box
    auto row = [this](const std::string& text) {
        out_ << "| " << text << std::string(kWidth - 2 - std::min(text.size(), kWidth - 2), ' ')
             << " |\n";
    };
    auto levelRow = [&row](const char* side, const LevelInfo& lvl) {
        std::ostringstream s;
        s << side << std::setw(12) << formatPrice(lvl.price) << "  x " << std::setw(8)
          << lvl.totalQty << "  (" << lvl.orderCount
          << (lvl.orderCount == 1 ? " order)" : " orders)");
        row(s.str());
    };

    const auto asks = book_.depth(Side::Sell, levels);
    const auto bids = book_.depth(Side::Buy, levels);
    out_ << "+" << std::string(kWidth, '-') << "+\n";
    if (asks.empty())
        row("ASK    (empty)");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        levelRow("ASK ", *it);
    if (auto spread = book_.spread())
        row("------- spread " + formatPrice(*spread) + " -------");
    else
        row("----------------------------");
    for (const auto& lvl : bids)
        levelRow("BID ", lvl);
    if (bids.empty())
        row("BID    (empty)");
    out_ << "+" << std::string(kWidth, '-') << "+\n";
}

void MatchingEngine::printHelp(std::ostream& out) {
    out << "Commands:\n"
           "  buy <qty> @ <price>    limit buy,  e.g. buy 100 @ 101.50\n"
           "  sell <qty> @ <price>   limit sell, e.g. sell 50 @ 102\n"
           "  buy <qty>              market buy (unfilled remainder is dropped)\n"
           "  sell <qty>             market sell\n"
           "  cancel <id>            cancel a resting order\n"
           "  modify <id> <qty>      change a resting order's quantity\n"
           "  book                   show the top 5 levels per side\n"
           "  help                   show this message\n"
           "  quit                   exit\n";
}

} // namespace ob
