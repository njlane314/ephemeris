#include "ephemeris.h"

namespace eph {

long long cik_for_ticker_asof(Db& db, const std::string& ticker, const std::string& date, long long fallback) {
    auto st = db.prepare(
        "select cik from security_identifier_history"
        " where ticker=? and valid_from<=? and (valid_to is null or valid_to='' or valid_to>=?)"
        " order by valid_from desc limit 1");
    st.bind(1, upper(ticker));
    st.bind(2, date);
    st.bind(3, date);
    if (st.step()) {
        long long cik = st.i64(0);
        if (cik) return cik;
    }
    return fallback;
}

void import_security_history(const Args& a) {
    if (!a.has("input")) throw Error("securities import-history requires --input");

    std::ifstream file;
    std::istream* in = &std::cin;
    std::string input = a.get("input");
    if (input != "-") {
        file.open(input);
        if (!file) throw Error("open failed: " + input);
        in = &file;
    }

    std::string line;
    if (!std::getline(*in, line)) throw Error("empty security history input");
    CsvHeader h(csv_parse_line(line));

    Db db(db_path(a));
    init_schema(db);
    auto hist = db.prepare(
        "insert into security_identifier_history(ticker,cik,valid_from,valid_to,source)"
        " values(?,?,?,?,?)"
        " on conflict(ticker,valid_from,source) do update set"
        " cik=excluded.cik, valid_to=excluded.valid_to");
    auto sec = db.prepare(
        "insert into securities(ticker,cik,security_type,active,first_seen,last_seen)"
        " values(?,?,'common',1,?,?)"
        " on conflict(ticker) do update set"
        " cik=coalesce(securities.cik,excluded.cik),"
        " first_seen=coalesce(securities.first_seen,excluded.first_seen),"
        " last_seen=coalesce(excluded.last_seen,securities.last_seen), active=1");

    long long rows = 0;
    db.exec("begin");
    try {
        while (std::getline(*in, line)) {
            if (trim(line).empty()) continue;
            auto row = csv_parse_line(line);
            std::string ticker = upper(h.get(row, {"ticker", "symbol"}));
            std::string valid_from = h.get(row, {"valid_from", "from", "start_date"});
            if (ticker.empty() || valid_from.empty()) continue;
            long long cik = to_i64(h.get(row, {"cik"}), 0);
            std::string valid_to = h.get(row, {"valid_to", "to", "end_date"});
            std::string source = h.get(row, {"source"});
            if (source.empty()) source = "import";

            hist.reset();
            hist.bind(1, ticker);
            if (cik) hist.bind64(2, cik); else hist.bind_null(2);
            hist.bind(3, valid_from);
            hist.bind(4, valid_to);
            hist.bind(5, source);
            hist.run();

            sec.reset();
            sec.bind(1, ticker);
            if (cik) sec.bind64(2, cik); else sec.bind_null(2);
            sec.bind(3, valid_from);
            sec.bind(4, valid_to.empty() ? valid_from : valid_to);
            sec.run();
            ++rows;
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "security_identifier_history\t" << rows << "\n";
}

} // namespace eph
