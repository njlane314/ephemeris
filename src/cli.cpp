#include "ephemeris.h"

namespace eph {

static void help(std::ostream& out) {
    out <<
R"(ephemeris: filing-aware momentum research system

usage:
  ephemeris db init --db data/ephemeris.db
  ephemeris sec sync-submissions --universe tickers.txt --db data/ephemeris.db [--limit N]
  ephemeris sec sync-companyfacts --db data/ephemeris.db [--limit N]
  ephemeris prices import --input prices.csv --db data/ephemeris.db
  ephemeris universe build --date YYYY-MM-DD [--min-market-cap N] [--min-adv N] [--min-price N] [--require-current-filing] --db DB
  ephemeris signal momentum --date YYYY-MM-DD [--lookbacks 63,126,252] [--skip-days 21] --db DB
  ephemeris regime filter --date YYYY-MM-DD [--market SPY] --db DB
  ephemeris portfolio build --date YYYY-MM-DD [--top 50] [--max-weight 0.04] [--regime-model MODEL] --db DB
  ephemeris backtest --from YYYY-MM-DD --to YYYY-MM-DD [--top 50] [--cost-bps 10] [--regime] --db DB
  ephemeris report [--date YYYY-MM-DD] --db DB

price CSV columns:
  date,ticker,open,high,low,close,adj_close,volume,market_cap

notes:
  SEC requests use EPHEMERIS_USER_AGENT if set.
  Backtests use filtered regime probabilities only when --regime is supplied.
)";
}

int run(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        help(std::cout);
        return 0;
    }

    std::string cmd = argv[1];
    if (cmd == "db") {
        if (argc < 3 || std::string(argv[2]) != "init") throw Error("usage: ephemeris db init --db DB");
        Args a = parse_args(argc, argv, 3);
        Db db(db_path(a));
        init_schema(db);
        std::cout << "db\t" << db_path(a) << "\n";
        return 0;
    }
    if (cmd == "sec") {
        if (argc < 3) throw Error("usage: ephemeris sec <sync-submissions|sync-companyfacts>");
        std::string sub = argv[2];
        Args a = parse_args(argc, argv, 3);
        if (sub == "sync-submissions") sync_submissions(a);
        else if (sub == "sync-companyfacts") sync_companyfacts(a);
        else throw Error("unknown sec command: " + sub);
        return 0;
    }
    if (cmd == "prices") {
        if (argc < 3 || std::string(argv[2]) != "import") throw Error("usage: ephemeris prices import --input CSV");
        import_prices(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "universe") {
        if (argc < 3 || std::string(argv[2]) != "build") throw Error("usage: ephemeris universe build --date DATE");
        Args a = parse_args(argc, argv, 3);
        Db db(db_path(a));
        init_schema(db);
        build_universe(db, a, false);
        return 0;
    }
    if (cmd == "signal") {
        if (argc < 3 || std::string(argv[2]) != "momentum") throw Error("usage: ephemeris signal momentum --date DATE");
        command_signal_momentum(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "regime") {
        if (argc < 3 || std::string(argv[2]) != "filter") throw Error("usage: ephemeris regime filter --date DATE");
        command_regime_filter(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "portfolio") {
        if (argc < 3 || std::string(argv[2]) != "build") throw Error("usage: ephemeris portfolio build --date DATE");
        command_portfolio_build(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "backtest") {
        command_backtest(parse_args(argc, argv, 2));
        return 0;
    }
    if (cmd == "report") {
        command_report(parse_args(argc, argv, 2));
        return 0;
    }
    throw Error("unknown command: " + cmd);
}


} // namespace eph
