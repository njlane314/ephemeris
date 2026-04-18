#include "ephemeris.h"

namespace eph {

static double adv63(Db& db, const std::string& ticker, const std::string& date) {
    auto st = db.prepare(
        "select avg(dv) from ("
        " select adj_close * volume as dv from prices"
        " where ticker=? and date<=? and adj_close>0 order by date desc limit 63)");
    st.bind(1, ticker);
    st.bind(2, date);
    return st.step() ? st.number(0) : 0.0;
}

static double latest_shares(Db& db, long long cik, const std::string& date) {
    if (!cik) return 0.0;
    auto st = db.prepare(
        "select value from facts"
        " where cik=? and tag in ('CommonStockSharesOutstanding','EntityCommonStockSharesOutstanding')"
        " and filed<=? and period_end<=?"
        " order by filed desc, period_end desc limit 1");
    st.bind64(1, cik);
    st.bind(2, date);
    st.bind(3, date);
    return st.step() ? st.number(0) : 0.0;
}

static bool has_recent_filing(Db& db, long long cik, const std::string& date, int max_age_days) {
    if (!cik) return false;
    auto st = db.prepare(
        "select 1 from filings where cik=? and filing_date<=? and filing_date>=?"
        " and (form like '10-K%' or form like '10-Q%') limit 1");
    st.bind64(1, cik);
    st.bind(2, date);
    st.bind(3, date_add(date, -max_age_days));
    return st.step();
}

static bool excluded_security(const std::string& ticker, const std::string& name, const std::string& type) {
    std::string t = upper(ticker);
    std::string n = upper(name);
    std::string ty = upper(type);
    if (t.empty() || t == "CASH") return true;
    if (ty.find("WARRANT") != std::string::npos || ty.find("PREFERRED") != std::string::npos ||
        ty.find("UNIT") != std::string::npos || ty.find("RIGHT") != std::string::npos ||
        ty.find("FUND") != std::string::npos) return true;
    const char* bad[] = {
        " ETF", " ETN", " FUND", "WARRANT", "RIGHTS", " UNIT",
        " PREFERRED", "PREFERENCE", "BLANK CHECK", "SPAC"
    };
    for (const char* k : bad) {
        if (n.find(k) != std::string::npos) return true;
    }
    return false;
}

struct SecurityRow {
    std::string ticker;
    long long cik = 0;
    std::string name;
    std::string exchange;
    std::string type;
    std::string sic;
};

static std::vector<SecurityRow> candidate_securities(Db& db) {
    std::vector<SecurityRow> out;
    auto st = db.prepare(
        "select ticker, max(cik), max(name), max(exchange), max(security_type), max(sic) from ("
        " select ticker,cik,name,exchange,security_type,sic from securities where active=1"
        " union all"
        " select distinct ticker,null,'','','common','' from prices"
        ") group by ticker order by ticker");
    while (st.step()) {
        SecurityRow r;
        r.ticker = st.text(0);
        r.cik = st.i64(1);
        r.name = st.text(2);
        r.exchange = st.text(3);
        r.type = st.text(4);
        r.sic = st.text(5);
        out.push_back(r);
    }
    return out;
}

int build_universe(Db& db, const Args& a, bool quiet) {
    std::string date = a.get("date");
    if (date.empty()) throw Error("universe build requires --date");
    double min_mcap = a.getd("min-market-cap", 0.0);
    double min_adv = a.getd("min-adv", 0.0);
    double min_price = a.getd("min-price", 0.0);
    int filing_age = a.geti("filing-age-days", 550);
    bool require_filing = a.has("require-current-filing");

    auto ins = db.prepare(
        "insert into universe(date,ticker,cik,eligible,reason,price,market_cap,adv)"
        " values(?,?,?,?,?,?,?,?)"
        " on conflict(date,ticker) do update set"
        " cik=excluded.cik, eligible=excluded.eligible, reason=excluded.reason,"
        " price=excluded.price, market_cap=excluded.market_cap, adv=excluded.adv");

    int total = 0;
    int eligible = 0;
    db.exec("begin");
    try {
        for (const SecurityRow& s : candidate_securities(db)) {
            ++total;
            Price p;
            bool ok = true;
            std::vector<std::string> reasons;
            if (excluded_security(s.ticker, s.name, s.type)) {
                ok = false;
                reasons.push_back("security_type");
            }
            if (!price_on_or_before(db, s.ticker, date, p)) {
                ok = false;
                reasons.push_back("no_price");
            }
            double adv = p.adj > 0.0 ? adv63(db, s.ticker, date) : 0.0;
            double mcap = p.market_cap;
            if (mcap <= 0.0 && s.cik) {
                double sh = latest_shares(db, s.cik, date);
                if (sh > 0.0) mcap = sh * p.adj;
            }
            if (min_price > 0.0 && p.adj < min_price) {
                ok = false;
                reasons.push_back("price");
            }
            if (min_adv > 0.0 && adv < min_adv) {
                ok = false;
                reasons.push_back("adv");
            }
            if (min_mcap > 0.0 && mcap < min_mcap) {
                ok = false;
                reasons.push_back("market_cap");
            }
            if (require_filing && !has_recent_filing(db, s.cik, date, filing_age)) {
                ok = false;
                reasons.push_back("filing");
            }
            if (ok) ++eligible;
            std::string reason = ok ? "eligible" : "";
            for (size_t i = 0; i < reasons.size(); ++i) {
                if (i) reason += ",";
                reason += reasons[i];
            }
            ins.reset();
            ins.bind(1, date);
            ins.bind(2, s.ticker);
            if (s.cik) ins.bind64(3, s.cik); else ins.bind_null(3);
            ins.bind(4, ok ? 1 : 0);
            ins.bind(5, reason);
            ins.bind(6, p.adj);
            ins.bind(7, mcap);
            ins.bind(8, adv);
            ins.run();
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    if (!quiet) std::cout << "date\t" << date << "\ntotal\t" << total << "\neligible\t" << eligible << "\n";
    return eligible;
}

std::vector<std::string> eligible_tickers(Db& db, const std::string& date) {
    std::vector<std::string> out;
    auto st = db.prepare("select ticker from universe where date=? and eligible=1 order by ticker");
    st.bind(1, date);
    while (st.step()) out.push_back(st.text(0));
    if (!out.empty()) return out;
    auto all = db.prepare("select distinct ticker from prices where date<=? and ticker<>'CASH' order by ticker");
    all.bind(1, date);
    while (all.step()) out.push_back(all.text(0));
    return out;
}

} // namespace eph
