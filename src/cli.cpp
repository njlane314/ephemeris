#include "ephemeris.h"

namespace eph {

static void help(std::ostream& out) {
    out <<
R"(ephemeris: filing-aware momentum research system

usage:
  ephemeris db init --db data/ephemeris.db
  ephemeris sec sync-companies --db data/ephemeris.db [--asof YYYY-MM-DD] [--limit N]
  ephemeris sec sync-submissions --universe tickers.txt --db data/ephemeris.db [--limit N]
  ephemeris sec sync-companyfacts --db data/ephemeris.db [--limit N]
  ephemeris sec update --db data/ephemeris.db [--limit N] [--facts]
  ephemeris securities import-history --input history.csv --db data/ephemeris.db
  ephemeris prices import --input prices.csv --db data/ephemeris.db
  ephemeris universe build --date YYYY-MM-DD [--min-market-cap N] [--min-adv N] [--min-price N] [--require-current-filing] --db DB
  ephemeris universe explain --date YYYY-MM-DD --ticker TICKER --db DB
  ephemeris signal momentum --date YYYY-MM-DD [--lookbacks 63,126,252] [--skip-days 21] --db DB
  ephemeris signal explain --date YYYY-MM-DD --ticker TICKER --db DB
  ephemeris regime filter --date YYYY-MM-DD [--market SPY] [--model MODEL] --db DB
  ephemeris portfolio build --date YYYY-MM-DD [--top 50] [--max-weight 0.04] [--regime-model MODEL] --db DB
  ephemeris portfolio explain --date YYYY-MM-DD --db DB
  ephemeris audit database --db DB
  ephemeris audit prices --db DB
  ephemeris audit asof --date YYYY-MM-DD --db DB
  ephemeris backtest --from YYYY-MM-DD --to YYYY-MM-DD [--top 50] [--cost-bps 10] [--cost-model MODEL] [--regime] --db DB
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
        if (argc < 3) throw Error("usage: ephemeris sec <sync-companies|sync-submissions|sync-companyfacts|update>");
        std::string sub = argv[2];
        Args a = parse_args(argc, argv, 3);
        if (sub == "sync-companies") sync_companies(a);
        else if (sub == "sync-submissions") sync_submissions(a);
        else if (sub == "sync-companyfacts") sync_companyfacts(a);
        else if (sub == "update") update_sec_database(a);
        else throw Error("unknown sec command: " + sub);
        return 0;
    }
    if (cmd == "prices") {
        if (argc < 3 || std::string(argv[2]) != "import") throw Error("usage: ephemeris prices import --input CSV");
        import_prices(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "securities") {
        if (argc < 3 || std::string(argv[2]) != "import-history") {
            throw Error("usage: ephemeris securities import-history --input CSV");
        }
        import_security_history(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "universe") {
        if (argc < 3) throw Error("usage: ephemeris universe <build|explain>");
        std::string sub = argv[2];
        Args a = parse_args(argc, argv, 3);
        if (sub == "build") {
            Db db(db_path(a));
            init_schema(db);
            build_universe(db, a, false);
        } else if (sub == "explain") {
            command_universe_explain(a);
        } else {
            throw Error("unknown universe command: " + sub);
        }
        return 0;
    }
    if (cmd == "signal") {
        if (argc < 3) throw Error("usage: ephemeris signal <momentum|explain>");
        std::string sub = argv[2];
        Args a = parse_args(argc, argv, 3);
        if (sub == "momentum") command_signal_momentum(a);
        else if (sub == "explain") command_signal_explain(a);
        else throw Error("unknown signal command: " + sub);
        return 0;
    }
    if (cmd == "regime") {
        if (argc < 3 || std::string(argv[2]) != "filter") throw Error("usage: ephemeris regime filter --date DATE");
        command_regime_filter(parse_args(argc, argv, 3));
        return 0;
    }
    if (cmd == "portfolio") {
        if (argc < 3) throw Error("usage: ephemeris portfolio <build|explain>");
        std::string sub = argv[2];
        Args a = parse_args(argc, argv, 3);
        if (sub == "build") command_portfolio_build(a);
        else if (sub == "explain") command_portfolio_explain(a);
        else throw Error("unknown portfolio command: " + sub);
        return 0;
    }
    if (cmd == "audit") {
        if (argc < 3) throw Error("usage: ephemeris audit <database|prices|asof>");
        std::string sub = argv[2];
        Args a = parse_args(argc, argv, 3);
        if (sub == "database") command_audit_database(a);
        else if (sub == "prices") command_audit_prices(a);
        else if (sub == "asof") command_audit_asof(a);
        else throw Error("unknown audit command: " + sub);
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
