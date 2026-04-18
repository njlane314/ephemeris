#include "ephemeris.h"

namespace eph {

static std::vector<std::pair<std::string, double>> dated_prices(Db& db, const std::string& ticker,
                                                                const std::string& date) {
    std::vector<std::pair<std::string, double>> rows;
    auto st = db.prepare(
        "select date, adj_close from prices where ticker=? and date<=? and adj_close>0 order by date");
    st.bind(1, ticker);
    st.bind(2, date);
    while (st.step()) rows.push_back({st.text(0), st.number(1)});
    return rows;
}

void command_universe_explain(const Args& a) {
    std::string date = a.get("date");
    std::string ticker = upper(a.get("ticker"));
    if (date.empty() || ticker.empty()) throw Error("universe explain requires --date and --ticker");

    Db db(db_path(a));
    init_schema(db);

    auto row = db.prepare(
        "select date,ticker,coalesce(cik,0),eligible,reason,price,market_cap,adv"
        " from universe where date=? and ticker=?");
    row.bind(1, date);
    row.bind(2, ticker);
    if (!row.step()) {
        std::cout << "date\t" << date << "\nticker\t" << ticker << "\nuniverse_row\tmissing\n";
        return;
    }

    long long cik = row.i64(2);
    std::cout << "date\t" << row.text(0) << "\nticker\t" << row.text(1)
              << "\ncik\t" << cik << "\neligible\t" << row.i64(3)
              << "\nreason\t" << row.text(4) << "\nprice\t" << row.number(5)
              << "\nmarket_cap\t" << row.number(6) << "\nadv\t" << row.number(7) << "\n";

    auto sec = db.prepare(
        "select name,exchange,security_type,sic,active from securities where ticker=?");
    sec.bind(1, ticker);
    if (sec.step()) {
        std::cout << "security_name\t" << sec.text(0) << "\nexchange\t" << sec.text(1)
                  << "\nsecurity_type\t" << sec.text(2) << "\nsic\t" << sec.text(3)
                  << "\nactive\t" << sec.i64(4) << "\n";
    }

    if (cik) {
        auto filing = db.prepare(
            "select form,filing_date,accession from filings"
            " where cik=? and filing_date<=? order by filing_date desc limit 1");
        filing.bind64(1, cik);
        filing.bind(2, date);
        if (filing.step()) {
            std::cout << "latest_filing_form\t" << filing.text(0)
                      << "\nlatest_filing_date\t" << filing.text(1)
                      << "\nlatest_filing_accession\t" << filing.text(2) << "\n";
        }
    }
}

void command_signal_explain(const Args& a) {
    std::string date = a.get("date");
    std::string ticker = upper(a.get("ticker"));
    if (date.empty() || ticker.empty()) throw Error("signal explain requires --date and --ticker");

    Db db(db_path(a));
    init_schema(db);

    auto sig = db.prepare(
        "select rank,score,mom_12_1,mom_6_1,mom_3_1,vol_3m"
        " from signals where date=? and ticker=?");
    sig.bind(1, date);
    sig.bind(2, ticker);
    if (!sig.step()) {
        std::cout << "date\t" << date << "\nticker\t" << ticker << "\nsignal_row\tmissing\n";
        return;
    }

    std::cout << "date\t" << date << "\nticker\t" << ticker
              << "\nrank\t" << sig.i64(0) << "\nscore\t" << sig.number(1)
              << "\nmom_12_1\t" << sig.number(2) << "\nmom_6_1\t" << sig.number(3)
              << "\nmom_3_1\t" << sig.number(4) << "\nvol_3m\t" << sig.number(5) << "\n";

    auto prices = dated_prices(db, ticker, date);
    int skip_days = a.geti("skip-days", 21);
    int end = static_cast<int>(prices.size()) - 1 - skip_days;
    std::cout << "price_count\t" << prices.size() << "\nskip_days\t" << skip_days << "\n";
    if (end <= 0) {
        std::cout << "scoring_prices\tinsufficient\n";
        return;
    }

    std::cout << "scoring_end_date\t" << prices[end].first
              << "\nscoring_end_price\t" << prices[end].second << "\n";
    std::cout << "lookback\tstart_date\tstart_price\tend_date\tend_price\treturn\n";
    for (int lb : parse_lookbacks(a.get("lookbacks"))) {
        if (end - lb < 0 || prices[end - lb].second <= 0.0) {
            std::cout << lb << "\tinsufficient\n";
            continue;
        }
        const auto& start = prices[end - lb];
        const auto& finish = prices[end];
        double ret = finish.second / start.second - 1.0;
        std::cout << lb << '\t' << start.first << '\t' << start.second << '\t'
                  << finish.first << '\t' << finish.second << '\t' << ret << "\n";
    }
}

void command_portfolio_explain(const Args& a) {
    std::string date = a.get("date");
    if (date.empty()) throw Error("portfolio explain requires --date");

    Db db(db_path(a));
    init_schema(db);

    std::cout << "date\t" << date << "\nrule\tlong_only_equal_weight_capped\n";
    auto regime = db.prepare(
        "select p_favorable,p_neutral,p_stress,expected_momentum_edge,exposure"
        " from regime_outputs where date<=? order by date desc limit 1");
    regime.bind(1, date);
    if (regime.step()) {
        std::cout << "p_favorable\t" << regime.number(0) << "\np_neutral\t" << regime.number(1)
                  << "\np_stress\t" << regime.number(2)
                  << "\nexpected_momentum_edge\t" << regime.number(3)
                  << "\nexposure\t" << regime.number(4) << "\n";
    } else {
        std::cout << "regime_row\tmissing\n";
    }

    auto targets = db.prepare(
        "select ticker,weight,rank,score,exposure from portfolio_targets"
        " where date=? order by case when ticker='CASH' then 1 else 0 end, rank, ticker");
    targets.bind(1, date);
    std::cout << "ticker\tweight\trank\tscore\texposure\n";
    bool any = false;
    while (targets.step()) {
        any = true;
        std::cout << targets.text(0) << '\t' << targets.number(1) << '\t'
                  << targets.i64(2) << '\t' << targets.number(3) << '\t'
                  << targets.number(4) << "\n";
    }
    if (!any) std::cout << "portfolio_targets\tmissing\n";
}

} // namespace eph
