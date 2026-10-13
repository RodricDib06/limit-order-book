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

bool parseLimits(const std::vector<std::string_view>& settings, RiskLimits& limits) {
    RiskLimits next = limits;
    for (std::string_view kv : settings) {
        const std::size_t eq = kv.find('=');
        if (eq == std::string_view::npos)
            return false;
        const std::string key = lower(trim(kv.substr(0, eq)));
        const std::string_view value = trim(kv.substr(eq + 1));
        if (key == "maxqty") {
            auto v = parseUnsigned<Qty>(value);
            if (!v)
                return false;
            next.maxOrderQty = *v;
        } else if (key == "maxpos") {
            auto v = parseUnsigned<std::uint32_t>(value);
            if (!v)
                return false;
            next.maxPosition = *v;
        } else if (key == "band") {
            auto v = parseUnsigned<std::uint32_t>(value);
            if (!v)
                return false;
            next.priceBandBps = *v;
        } else if (key == "maxvalue" || key == "credit" || key == "maxloss") {
            auto v = parsePrice(value); // dollars -> cents
            if (!v || *v < 0)
                return false;
            (key == "maxvalue" ? next.maxOrderNotional
             : key == "credit" ? next.maxOpenNotional
                               : next.maxLoss) = *v;
        } else if (key == "stp") {
            const std::string v = lower(value);
            if (v != "on" && v != "off")
                return false;
            next.selfTradePrevention = v == "on";
        } else {
            return false;
        }
    }
    limits = next;
    return true;
}

MatchingEngine::MatchingEngine(std::ostream& out)
    : house_(risk_.addAccount("house")), current_(house_), out_(out) {}

std::string MatchingEngine::tag(AccountId account) const {
    return account == house_ ? std::string() : " [" + risk_.snapshot(account).name + "]";
}

bool MatchingEngine::configureAccount(std::string_view name,
                                      const std::vector<std::string_view>& settings) {
    auto id = risk_.findAccount(name);
    // New trader accounts start with self-trade prevention on.
    RiskLimits limits = id ? risk_.snapshot(*id).limits : RiskLimits{};
    if (!id)
        limits.selfTradePrevention = true;
    if (!parseLimits(settings, limits))
        return false;
    if (!id)
        id = risk_.addAccount(std::string(name), limits);
    else
        risk_.setLimits(*id, limits);
    current_ = *id;
    out_ << "ACCOUNT  " << name << " ready\n";
    reportKills();
    return true;
}

void MatchingEngine::reportKills() {
    for (const KillEvent& k : risk_.lastKills())
        out_ << "KILLED  " << tag(k.account) << ' ' << k.reason << ", " << k.ordersCancelled
             << " order(s) cancelled, account blocked\n";
}

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

void MatchingEngine::reportReject(AccountId account, OrderId id, Status status) {
    out_ << "REJECTED #" << id << tag(account) << ": " << toString(status) << '\n';
}

void MatchingEngine::submitLimit(AccountId account, OrderId id, Side side, Price price, Qty qty) {
    trades_.clear();
    const Status st = risk_.submitLimit(account, Order{id, side, price, qty}, trades_);
    if (st != Status::Ok)
        return reportReject(account, id, st);
    out_ << "ACCEPTED #" << id << tag(account) << ' ' << toString(side) << ' ' << qty << " @ "
         << formatPrice(price) << '\n';
    reportTrades();
    if (auto resting = book().find(id))
        out_ << "RESTING  #" << id << ' ' << resting->qty << " @ " << formatPrice(price) << '\n';
    reportKills();
}

void MatchingEngine::submitMarket(AccountId account, OrderId id, Side side, Qty qty) {
    trades_.clear();
    const Status st = risk_.submitMarket(account, id, side, qty, trades_);
    if (st != Status::Ok)
        return reportReject(account, id, st);
    out_ << "ACCEPTED #" << id << tag(account) << ' ' << toString(side) << ' ' << qty
         << " @ MARKET\n";
    reportTrades();
    Qty filled = 0;
    for (const Trade& t : trades_)
        filled += t.qty;
    if (filled < qty)
        out_ << "DROPPED  #" << id << ' ' << (qty - filled) << " unfilled\n";
    reportKills();
}

void MatchingEngine::submitCancel(OrderId id) {
    const AccountId owner = risk_.ownerOf(id).value_or(house_);
    const Status st = risk_.cancel(id);
    if (st != Status::Ok)
        return reportReject(owner, id, st);
    out_ << "CANCELED #" << id << '\n';
}

void MatchingEngine::submitModify(OrderId id, Qty qty) {
    const AccountId owner = risk_.ownerOf(id).value_or(house_);
    const Status st = risk_.modify(id, qty);
    if (st != Status::Ok)
        return reportReject(owner, id, st);
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
            submitMarket(current_, nextId_++, *side, *qty);
        } else if (w.size() == 4 && w[2] == "@") {
            auto price = parsePrice(w[3]);
            if (!price)
                usage("bad price");
            else
                submitLimit(current_, nextId_++, *side, *price, *qty);
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
    } else if (cmd == "account") {
        if (w.size() < 2)
            usage("expected: account <name> [key=value ...]");
        else if (!configureAccount(w[1], {w.begin() + 2, w.end()}))
            usage("bad limit; keys: maxqty maxvalue maxpos credit band maxloss stp");
    } else if (cmd == "use") {
        auto id = w.size() == 2 ? risk_.findAccount(w[1]) : std::nullopt;
        if (!id) {
            usage("expected: use <existing account>");
        } else {
            current_ = *id;
            out_ << "USING   " << w[1] << '\n';
        }
    } else if (cmd == "risk") {
        printRisk();
    } else if (cmd == "kill" || cmd == "unblock") {
        auto id = w.size() == 2 ? risk_.findAccount(w[1]) : std::nullopt;
        if (!id) {
            usage("expected: kill|unblock <account>");
        } else if (cmd == "kill") {
            const std::size_t n = risk_.kill(*id, "manual kill switch");
            out_ << "KILLED   [" << w[1] << "] manual kill switch, " << n
                 << " order(s) cancelled, account blocked\n";
        } else if (risk_.unblock(*id) == Status::Ok) {
            out_ << "UNBLOCKED [" << w[1] << "]\n";
        } else {
            out_ << "ERROR: " << w[1] << " is still past its loss limit; raise maxloss first\n";
        }
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
    if (type == "account") {
        if (field(1).empty())
            return false;
        return configureAccount(field(1), {f.begin() + 2, f.end()});
    }
    auto id = parseUnsigned<OrderId>(field(1));
    if (!id)
        return false;
    noteId(*id);
    AccountId account = house_;
    if (!field(5).empty()) {
        auto found = risk_.findAccount(field(5));
        if (!found)
            return false;
        account = *found;
    }

    if (type == "limit") {
        auto side = parseSide(field(2));
        auto price = parsePrice(field(3));
        auto qty = parseUnsigned<Qty>(field(4));
        if (!side || !price || !qty)
            return false;
        submitLimit(account, *id, *side, *price, *qty);
    } else if (type == "market") {
        auto side = parseSide(field(2));
        auto qty = parseUnsigned<Qty>(field(4));
        if (!side || !qty)
            return false;
        submitMarket(account, *id, *side, *qty);
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

    const auto asks = book().depth(Side::Sell, levels);
    const auto bids = book().depth(Side::Buy, levels);
    out_ << "+" << std::string(kWidth, '-') << "+\n";
    if (asks.empty())
        row("ASK    (empty)");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        levelRow("ASK ", *it);
    if (auto spread = book().spread())
        row("------- spread " + formatPrice(*spread) + " -------");
    else
        row("----------------------------");
    for (const auto& lvl : bids)
        levelRow("BID ", lvl);
    if (bids.empty())
        row("BID    (empty)");
    out_ << "+" << std::string(kWidth, '-') << "+\n";
}

void MatchingEngine::printRisk() const {
    const auto mark = risk_.lastTradePrice();
    out_ << "Mark price: " << (mark ? formatPrice(*mark) : std::string("none (no trades yet)"))
         << "   P&L in dollars\n";
    out_ << std::left << std::setw(10) << "ACCOUNT" << std::right << std::setw(8) << "POS"
         << std::setw(10) << "AVG COST" << std::setw(11) << "REALIZED" << std::setw(11) << "UNREAL."
         << std::setw(11) << "TOTAL" << std::setw(12) << "OPEN B/S"
         << "  STATUS\n";
    for (AccountId id = 0; id < risk_.accountCount(); ++id) {
        const AccountSnapshot a = risk_.snapshot(id);
        const std::string open = std::to_string(a.openBuyQty) + "/" + std::to_string(a.openSellQty);
        out_ << std::left << std::setw(10) << a.name << std::right << std::setw(8) << a.position
             << std::setw(10) << (a.position ? formatPrice(a.avgCost) : "-") << std::setw(11)
             << formatPrice(a.realizedPnl) << std::setw(11) << formatPrice(a.unrealizedPnl)
             << std::setw(11) << formatPrice(a.totalPnl) << std::setw(12) << open << "  "
             << (a.blocked ? "BLOCKED (" + a.blockReason + ")" : std::string("active")) << '\n';
    }
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
           "\n"
           "Risk (orders are placed for the current account; default 'house' has no limits):\n"
           "  account <name> [k=v..] create/update an account and switch to it, e.g.\n"
           "                         account alice maxqty=500 maxpos=1000 maxloss=2000 band=500\n"
           "                         keys: maxqty, maxpos (shares), maxvalue, credit, maxloss\n"
           "                         (dollars), band (basis points), stp=on|off\n"
           "  use <name>             switch to an existing account\n"
           "  risk                   positions, P&L, open orders, and status per account\n"
           "  kill <name>            kill switch: cancel the account's orders and block it\n"
           "  unblock <name>         allow a blocked account to trade again\n"
           "  help                   show this message\n"
           "  quit                   exit\n";
}

} // namespace ob
