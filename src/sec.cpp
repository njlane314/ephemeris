#include "ephemeris.h"

namespace eph {

static std::string cik10(long long cik) {
    std::ostringstream out;
    out << std::setw(10) << std::setfill('0') << cik;
    return out.str();
}

static void upsert_company(Db& db, long long cik, const std::string& ticker, const std::string& name,
                           const std::string& exchange, const std::string& sic,
                           const std::string& fiscal_year_end) {
    auto st = db.prepare(
        "insert into companies(cik,ticker,name,exchange,sic,fiscal_year_end,updated_at)"
        " values(?,?,?,?,?,?,current_timestamp)"
        " on conflict(cik) do update set"
        " ticker=coalesce(nullif(excluded.ticker,''),companies.ticker),"
        " name=coalesce(nullif(excluded.name,''),companies.name),"
        " exchange=coalesce(nullif(excluded.exchange,''),companies.exchange),"
        " sic=coalesce(nullif(excluded.sic,''),companies.sic),"
        " fiscal_year_end=coalesce(nullif(excluded.fiscal_year_end,''),companies.fiscal_year_end),"
        " updated_at=current_timestamp");
    st.bind64(1, cik);
    st.bind(2, ticker);
    st.bind(3, name);
    st.bind(4, exchange);
    st.bind(5, sic);
    st.bind(6, fiscal_year_end);
    st.run();

    if (!ticker.empty()) {
        auto sec = db.prepare(
            "insert into securities(ticker,cik,name,exchange,security_type,sic,active,first_seen,last_seen)"
            " values(?,?,?,?, 'common', ?, 1, date('now'), date('now'))"
            " on conflict(ticker) do update set"
            " cik=coalesce(excluded.cik,securities.cik),"
            " name=coalesce(nullif(excluded.name,''),securities.name),"
            " exchange=coalesce(nullif(excluded.exchange,''),securities.exchange),"
            " sic=coalesce(nullif(excluded.sic,''),securities.sic),"
            " active=1,last_seen=date('now')");
        sec.bind(1, ticker);
        sec.bind64(2, cik);
        sec.bind(3, name);
        sec.bind(4, exchange);
        sec.bind(5, sic);
        sec.run();
    }
}

struct SecCompany {
    long long cik = 0;
    std::string name;
    std::string ticker;
    std::string exchange;
};

static std::vector<SecCompany> fetch_sec_companies() {
    std::string body = http_get("https://www.sec.gov/files/company_tickers_exchange.json");
    Json root = parse_json(body);
    std::unordered_map<std::string, int> fidx;
    const Json& fields = root.get("fields");
    for (size_t i = 0; i < fields.a.size(); ++i) fidx[fields.at(i).str()] = static_cast<int>(i);
    auto idx = [&](const std::string& k) -> int {
        auto it = fidx.find(k);
        if (it == fidx.end()) throw Error("SEC company map missing field: " + k);
        return it->second;
    };
    int cik_i = idx("cik");
    int name_i = idx("name");
    int ticker_i = idx("ticker");
    int exchange_i = idx("exchange");

    std::vector<SecCompany> out;
    for (const Json& row : root.get("data").a) {
        SecCompany c;
        c.cik = static_cast<long long>(row.at(cik_i).num());
        c.name = row.at(name_i).str();
        c.ticker = upper(row.at(ticker_i).str());
        c.exchange = row.at(exchange_i).str();
        if (!c.ticker.empty()) out.push_back(c);
    }
    return out;
}

static int arg_offset(const Args& a) {
    return std::max(0, a.geti("offset", 0));
}

static int arg_limit(const Args& a) {
    return std::max(0, a.geti("limit", 0));
}

static std::unordered_map<std::string, SecCompany> sec_company_map(const std::vector<SecCompany>& companies) {
    std::unordered_map<std::string, SecCompany> out;
    for (const SecCompany& c : companies) out[c.ticker] = c;
    return out;
}

static void record_identifier_history(Db& db, const SecCompany& c, const std::string& asof) {
    auto st = db.prepare(
        "insert into security_identifier_history(ticker,cik,valid_from,valid_to,source)"
        " values(?,?,coalesce(nullif(?,''),date('now')),'','sec-company-tickers')"
        " on conflict(ticker,valid_from,source) do update set cik=excluded.cik");
    st.bind(1, c.ticker);
    st.bind64(2, c.cik);
    st.bind(3, asof);
    st.run();
}

void sync_companies(const Args& a) {
    Db db(db_path(a));
    init_schema(db);
    int limit = arg_limit(a);
    int offset = arg_offset(a);
    std::string asof = a.get("asof");

    auto companies = fetch_sec_companies();
    long long rows = 0;
    long long seen = 0;
    db.exec("begin");
    try {
        for (const SecCompany& c : companies) {
            if (seen++ < offset) continue;
            if (limit && rows >= limit) break;
            upsert_company(db, c.cik, c.ticker, c.name, c.exchange, "", "");
            record_identifier_history(db, c, asof);
            ++rows;
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "companies\t" << rows << "\noffset\t" << offset
              << "\nsecurity_identifier_history\t" << rows << "\n";
}

static std::vector<SecCompany> db_companies(Db& db, int limit, int offset, int stale_days, const std::string& kind) {
    std::vector<SecCompany> out;
    std::string stale_col = kind == "facts" ? "facts_synced_at" : "submissions_synced_at";
    std::string sql =
        "select c.cik,coalesce(c.ticker,''),coalesce(c.name,''),coalesce(c.exchange,'')"
        " from companies c left join sec_sync_state s on s.cik=c.cik"
        " where c.cik is not null and c.cik>0";
    if (stale_days > 0) {
        sql += " and (s." + stale_col + " is null or s." + stale_col + " < datetime('now', '-"
             + std::to_string(stale_days) + " days'))";
    }
    sql += " order by c.cik";
    if (limit > 0) sql += " limit " + std::to_string(limit);
    if (offset > 0) sql += " offset " + std::to_string(offset);
    auto st = db.prepare(sql);
    while (st.step()) {
        SecCompany c;
        c.cik = st.i64(0);
        c.ticker = upper(st.text(1));
        c.name = st.text(2);
        c.exchange = st.text(3);
        if (!c.ticker.empty()) out.push_back(c);
    }
    return out;
}

static std::vector<SecCompany> submission_targets(Db& db, const Args& a) {
    int limit = arg_limit(a);
    int offset = arg_offset(a);
    int stale_days = a.geti("stale-days", 0);
    if (!a.has("universe")) return db_companies(db, limit, offset, stale_days, "submissions");

    auto wanted = read_universe_file(a.get("universe"));
    auto map = sec_company_map(fetch_sec_companies());
    std::vector<SecCompany> out;
    int seen = 0;
    for (const std::string& ticker : wanted) {
        if (seen++ < offset) continue;
        if (limit && static_cast<int>(out.size()) >= limit) break;
        auto it = map.find(ticker);
        if (it == map.end()) {
            std::cerr << "missing_sec_ticker\t" << ticker << "\n";
            continue;
        }
        out.push_back(it->second);
    }
    return out;
}

static void mark_submissions_synced(Db& db, long long cik, const std::string& error = "") {
    auto st = db.prepare(
        "insert into sec_sync_state(cik,submissions_synced_at,last_error)"
        " values(?,datetime('now'),?)"
        " on conflict(cik) do update set"
        " submissions_synced_at=excluded.submissions_synced_at,last_error=excluded.last_error");
    st.bind64(1, cik);
    st.bind(2, error);
    st.run();
}

static void mark_facts_synced(Db& db, long long cik, const std::string& error = "") {
    auto st = db.prepare(
        "insert into sec_sync_state(cik,facts_synced_at,last_error)"
        " values(?,datetime('now'),?)"
        " on conflict(cik) do update set"
        " facts_synced_at=excluded.facts_synced_at,last_error=excluded.last_error");
    st.bind64(1, cik);
    st.bind(2, error);
    st.run();
}

void sync_submissions(const Args& a) {
    Db db(db_path(a));
    init_schema(db);

    auto targets = submission_targets(db, a);
    long long companies = 0;
    long long filings = 0;

    auto filing = db.prepare(
        "insert into filings(cik,accession,form,filing_date,report_date,acceptance_datetime,primary_document)"
        " values(?,?,?,?,?,?,?)"
        " on conflict(cik,accession) do update set"
        " form=excluded.form, filing_date=excluded.filing_date, report_date=excluded.report_date,"
        " acceptance_datetime=excluded.acceptance_datetime, primary_document=excluded.primary_document");

    db.exec("begin");
    try {
        for (const SecCompany& c : targets) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            Json sub;
            try {
                sub = parse_json(http_get("https://data.sec.gov/submissions/CIK" + cik10(c.cik) + ".json"));
            } catch (const std::exception& e) {
                std::cerr << "sync_error\t" << c.ticker << "\t" << e.what() << "\n";
                mark_submissions_synced(db, c.cik, e.what());
                continue;
            }
            std::string name = sub.get("name").str().empty() ? c.name : sub.get("name").str();
            std::string sic = sub.get("sic").str();
            std::string fye = sub.get("fiscalYearEnd").str();
            std::string exchange = c.exchange;
            if (sub.get("exchanges").is_array() && !sub.get("exchanges").a.empty()) {
                exchange = sub.get("exchanges").at(0).str();
            }
            std::string primary_ticker = c.ticker;
            if (sub.get("tickers").is_array() && !sub.get("tickers").a.empty()) {
                primary_ticker = upper(sub.get("tickers").at(0).str());
            }
            upsert_company(db, c.cik, primary_ticker, name, exchange, sic, fye);
            ++companies;

            const Json& recent = sub.get("filings").get("recent");
            const auto& forms = recent.get("form").a;
            const auto& accns = recent.get("accessionNumber").a;
            const auto& fdates = recent.get("filingDate").a;
            const auto& rdates = recent.get("reportDate").a;
            const auto& adt = recent.get("acceptanceDateTime").a;
            const auto& docs = recent.get("primaryDocument").a;
            for (size_t i = 0; i < accns.size(); ++i) {
                filing.reset();
                filing.bind64(1, c.cik);
                filing.bind(2, accns[i].str());
                filing.bind(3, i < forms.size() ? forms[i].str() : "");
                filing.bind(4, i < fdates.size() ? fdates[i].str() : "");
                filing.bind(5, i < rdates.size() ? rdates[i].str() : "");
                filing.bind(6, i < adt.size() ? adt[i].str() : "");
                filing.bind(7, i < docs.size() ? docs[i].str() : "");
                filing.run();
                ++filings;
            }
            mark_submissions_synced(db, c.cik);
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "companies\t" << companies << "\noffset\t" << arg_offset(a)
              << "\nfilings\t" << filings << "\n";
}

void sync_companyfacts(const Args& a) {
    Db db(db_path(a));
    init_schema(db);
    int limit = arg_limit(a);
    int offset = arg_offset(a);
    int stale_days = a.geti("stale-days", 0);
    std::set<std::string> tags = {
        "Revenues",
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "NetIncomeLoss",
        "Assets",
        "Liabilities",
        "StockholdersEquity",
        "CommonStockSharesOutstanding",
        "EntityCommonStockSharesOutstanding",
        "CashAndCashEquivalentsAtCarryingValue",
        "LongTermDebt",
        "LongTermDebtCurrent",
        "DebtCurrent",
        "DebtInstrumentCarryingAmount"
    };

    std::vector<long long> ciks;
    std::string sql =
        "select c.cik from companies c left join sec_sync_state s on s.cik=c.cik"
        " where c.cik is not null and c.cik>0";
    if (stale_days > 0) {
        sql += " and (s.facts_synced_at is null or s.facts_synced_at < datetime('now', '-"
             + std::to_string(stale_days) + " days'))";
    }
    sql += " order by c.cik";
    if (limit > 0) sql += " limit " + std::to_string(limit);
    if (offset > 0) sql += " offset " + std::to_string(offset);
    auto q = db.prepare(sql);
    while (q.step()) ciks.push_back(q.i64(0));

    auto ins = db.prepare(
        "insert into facts(cik,tag,period_end,filed,form,fy,fp,unit,value,accession,frame)"
        " values(?,?,?,?,?,?,?,?,?,?,?)"
        " on conflict(cik,tag,period_end,filed,unit,accession) do update set"
        " form=excluded.form, fy=excluded.fy, fp=excluded.fp, value=excluded.value, frame=excluded.frame");

    long long rows = 0;
    int processed = 0;
    db.exec("begin");
    try {
        for (long long cik : ciks) {
            if (limit && processed >= limit) break;
            ++processed;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            Json root;
            try {
                root = parse_json(http_get("https://data.sec.gov/api/xbrl/companyfacts/CIK" + cik10(cik) + ".json"));
            } catch (const std::exception& e) {
                std::cerr << "facts_error\t" << cik << "\t" << e.what() << "\n";
                mark_facts_synced(db, cik, e.what());
                continue;
            }
            const Json& gaap = root.get("facts").get("us-gaap");
            if (!gaap.is_object()) continue;
            for (const auto& kv : gaap.o) {
                const std::string& tag = kv.first;
                if (!tags.count(tag)) continue;
                const Json& units = kv.second.get("units");
                if (!units.is_object()) continue;
                for (const auto& uv : units.o) {
                    const std::string& unit = uv.first;
                    for (const Json& rec : uv.second.a) {
                        std::string end = rec.get("end").str();
                        std::string filed = rec.get("filed").str();
                        if (end.empty() || filed.empty() || !rec.get("val").is_number()) continue;
                        ins.reset();
                        ins.bind64(1, cik);
                        ins.bind(2, tag);
                        ins.bind(3, end);
                        ins.bind(4, filed);
                        ins.bind(5, rec.get("form").str());
                        if (rec.get("fy").is_number()) ins.bind(6, static_cast<int>(rec.get("fy").num()));
                        else ins.bind_null(6);
                        ins.bind(7, rec.get("fp").str());
                        ins.bind(8, unit);
                        ins.bind(9, rec.get("val").num());
                        ins.bind(10, rec.get("accn").str());
                        ins.bind(11, rec.get("frame").str());
                        ins.run();
                        ++rows;
                    }
                }
            }
            mark_facts_synced(db, cik);
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "companies\t" << processed << "\noffset\t" << offset << "\nfacts\t" << rows << "\n";
}

void update_sec_database(const Args& a) {
    sync_companies(a);
    sync_submissions(a);
    if (a.has("facts") || a.has("companyfacts")) {
        sync_companyfacts(a);
    }
}

} // namespace eph
