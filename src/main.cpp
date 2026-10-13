// Command-line demo for the order book.
//
//   orderbook                       interactive mode (type 'help')
//   orderbook data/sample_orders.csv   replay a CSV file and print trades + final book
//   orderbook data/risk_demo.csv       replay with trader accounts and risk limits

#include "orderbook/MatchingEngine.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

void usage(const char* prog) {
    std::cout
        << "Usage:\n"
        << "  " << prog << "              interactive mode\n"
        << "  " << prog << " <file.csv>   replay orders from a CSV file\n\n"
        << "CSV columns: type,id,side,price,qty[,account]  (type = limit|market|cancel|modify)\n"
        << "Account rows: account,<name>[,key=value...]\n\n";
    ob::MatchingEngine::printHelp(std::cout);
}

int runCsv(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "error: cannot open " << path << '\n';
        return 1;
    }
    ob::MatchingEngine engine(std::cout);
    std::string line;
    int lineNo = 0;
    int bad = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!engine.executeCsvLine(line)) {
            std::cerr << path << ':' << lineNo << ": malformed row: " << line << '\n';
            ++bad;
        }
    }
    std::cout << '\n';
    engine.printBook();
    if (engine.risk().accountCount() > 1) {
        std::cout << '\n';
        engine.printRisk();
    }
    std::cout << engine.totalTrades() << " trades, " << engine.book().orderCount()
              << " orders resting\n";
    return bad == 0 ? 0 : 2;
}

int runInteractive() {
    const bool tty = isatty(STDIN_FILENO);
    ob::MatchingEngine engine(std::cout);
    if (tty)
        std::cout << "Order book ready. Type 'help' for commands.\n";
    std::string line;
    while (true) {
        if (tty)
            std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line))
            break;
        if (!engine.execute(line))
            break;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        usage(argv[0]);
        return 1;
    }
    if (argc == 2) {
        if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        return runCsv(argv[1]);
    }
    return runInteractive();
}
