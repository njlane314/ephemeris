#include "ephemeris.h"

namespace eph {

static long long scalar_i64(Db& db, const std::string& sql) {
    auto st = db.prepare(sql);
    return st.step() ? st.i64(0) : 0;
}

static double scalar_num(Db& db, const std::string& sql) {
    auto st = db.prepare(sql);
    return st.step() ? st.number(0) : 0.0;
}

static std::string scalar_text(Db& db, const std::string& sql) {
    auto st = db.prepare(sql);
    return st.step() ? st.text(0) : "";
}

static void print_count(Db& db, const std::string& name, const std::string& table) {
    std::cout << name << '\t' << scalar_i64(db, "select count(*) from " + table) << "\n";
}

void command_audit_database(const Args& a) {
    Db db(db_path(a));
    init_schema(db);

    std::cout << "audit\tdatabase\n";
    print_count(db, "companies", "companies");
    print_count(db, "securities", "securities");
    print_count(db, "security_identifier_history", "security_identifier_history");
    print_count(db, "filings", "filings");
    print_count(db, "facts", "facts");
    print_count(db, "prices", "prices");
    print_count(db, "universe", "universe");
    print_count(db, "signals", "signals");
    print_count(db, "portfolio_targets", "portfolio_targets");
    print_count(db, "regime_outputs", "regime_outputs");
    print_count(db, "regime_models", "regime_models");
    print_count(db, "transaction_cost_models", "transaction_cost_models");

    std::cout << "issue_universe_without_price\t"
              << scalar_i64(db,
                  "select count(*) from universe u"
                  " left join prices p on p.ticker=u.ticker and p.date<=u.date"
                  " where p.ticker is null")
              << "\n";
    std::cout << "issue_signals_without_eligible_universe\t"
              << scalar_i64(db,
                  "select count(*) from signals s"
                  " left join universe u on u.date=s.date and u.ticker=s.ticker and u.eligible=1"
                  " where u.ticker is null")
              << "\n";
    std::cout << "issue_portfolio_without_signal\t"
              << scalar_i64(db,
                  "select count(*) from portfolio_targets p"
                  " left join signals s on s.date=p.date and s.ticker=p.ticker"
                  " where p.ticker<>'CASH' and s.ticker is null")
              << "\n";
    std::cout << "issue_prices_after_delisting\t"
              << scalar_i64(db,
                  "select count(*) from prices p"
                  " join delistings d on d.ticker=p.ticker and p.date>d.delist_date")
              << "\n";
}

void command_audit_prices(const Args& a) {
    Db db(db_path(a));
    init_schema(db);

    std::cout << "audit\tprices\n";
    std::cout << "rows\t" << scalar_i64(db, "select count(*) from prices") << "\n";
    std::cout << "tickers\t" << scalar_i64(db, "select count(distinct ticker) from prices") << "\n";
    std::cout << "first_date\t" << scalar_text(db, "select min(date) from prices") << "\n";
    std::cout << "last_date\t" << scalar_text(db, "select max(date) from prices") << "\n";
    std::cout << "issue_nonpositive_adj_close\t"
              << scalar_i64(db, "select count(*) from prices where adj_close<=0") << "\n";
    std::cout << "issue_negative_volume\t"
              << scalar_i64(db, "select count(*) from prices where volume<0") << "\n";
    std::cout << "issue_missing_market_cap\t"
              << scalar_i64(db, "select count(*) from prices where market_cap<=0") << "\n";
    std::cout << "issue_prices_after_delisting\t"
              << scalar_i64(db,
                  "select count(*) from prices p"
                  " join delistings d on d.ticker=p.ticker and p.date>d.delist_date")
              << "\n";
}

void command_audit_asof(const Args& a) {
    std::string date = a.get("date");
    if (date.empty()) throw Error("audit asof requires --date");
    if (date.size() != 10) throw Error("bad date: " + date);
    date_days(date);

    Db db(db_path(a));
    init_schema(db);

    std::cout << "audit\tasof\n";
    std::cout << "date\t" << date << "\n";
    std::cout << "price_tickers_asof\t"
              << scalar_i64(db, "select count(distinct ticker) from prices where date<='" + date + "'")
              << "\n";
    std::cout << "eligible_universe_rows\t"
              << scalar_i64(db, "select count(*) from universe where date='" + date + "' and eligible=1")
              << "\n";
    std::cout << "signals_asof\t"
              << scalar_i64(db, "select count(*) from signals where date='" + date + "'")
              << "\n";
    std::cout << "portfolio_targets_asof\t"
              << scalar_i64(db, "select count(*) from portfolio_targets where date='" + date + "'")
              << "\n";
    std::cout << "future_prices_present\t"
              << scalar_i64(db,
                  "select count(*) from prices where date>'" + date + "'")
              << "\n";
    std::cout << "future_facts_present\t"
              << scalar_i64(db, "select count(*) from facts where filed>'" + date + "'")
              << "\n";
    std::cout << "avg_universe_adv\t"
              << scalar_num(db, "select coalesce(avg(adv),0) from universe where date='" + date + "' and eligible=1")
              << "\n";
    std::cout << "avg_universe_market_cap\t"
              << scalar_num(db, "select coalesce(avg(market_cap),0) from universe where date='" + date + "' and eligible=1")
              << "\n";
}

} // namespace eph
