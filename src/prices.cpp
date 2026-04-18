#include "ephemeris.h"

namespace eph {

void import_prices(const Args& a) {
    if (!a.has("input")) throw Error("prices import requires --input");
    std::string input = a.get("input");
    if (lower(input).size() >= 8 && lower(input).substr(lower(input).size() - 8) == ".parquet") {
        throw Error("prices import reads CSV. Convert Parquet upstream, e.g. duckdb -c \"copy (select * from 'prices.parquet') to 'prices.csv' with (header, delimiter ',')\"");
    }

    std::ifstream file;
    std::istream* in = &std::cin;
    if (input != "-") {
        file.open(input);
        if (!file) throw Error("open failed: " + input);
        in = &file;
    }

    std::string line;
    if (!std::getline(*in, line)) throw Error("empty price input");
    CsvHeader h(csv_parse_line(line));

    Db db(db_path(a));
    init_schema(db);
    auto st = db.prepare(
        "insert into prices(ticker,date,open,high,low,close,adj_close,volume,market_cap)"
        " values(?,?,?,?,?,?,?,?,?)"
        " on conflict(ticker,date) do update set"
        " open=excluded.open, high=excluded.high, low=excluded.low, close=excluded.close,"
        " adj_close=excluded.adj_close, volume=excluded.volume, market_cap=excluded.market_cap");
    auto sec = db.prepare(
        "insert into securities(ticker,security_type,active,first_seen,last_seen)"
        " values(?,'common',1,?,?)"
        " on conflict(ticker) do update set active=1,"
        " first_seen=coalesce(securities.first_seen,excluded.first_seen), last_seen=excluded.last_seen");

    long long rows = 0;
    db.exec("begin");
    try {
        while (std::getline(*in, line)) {
            if (trim(line).empty()) continue;
            auto row = csv_parse_line(line);
            std::string ticker = upper(h.get(row, {"ticker", "symbol"}));
            std::string date = h.get(row, {"date"});
            if (ticker.empty() || date.empty()) continue;
            double close = to_double(h.get(row, {"close"}), 0.0);
            double adj = to_double(h.get(row, {"adj_close", "adjusted_close", "adjclose", "adjustedclose"}), close);
            if (adj <= 0.0) continue;
            st.reset();
            st.bind(1, ticker);
            st.bind(2, date);
            st.bind(3, to_double(h.get(row, {"open"}), close));
            st.bind(4, to_double(h.get(row, {"high"}), close));
            st.bind(5, to_double(h.get(row, {"low"}), close));
            st.bind(6, close > 0.0 ? close : adj);
            st.bind(7, adj);
            st.bind(8, to_double(h.get(row, {"volume", "vol"}), 0.0));
            st.bind(9, to_double(h.get(row, {"market_cap", "marketcap"}), 0.0));
            st.run();

            sec.reset();
            sec.bind(1, ticker);
            sec.bind(2, date);
            sec.bind(3, date);
            sec.run();
            ++rows;
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "prices\t" << rows << "\n";
}
bool price_on_or_before(Db& db, const std::string& ticker, const std::string& date, Price& p) {
    auto st = db.prepare(
        "select date, adj_close, volume, market_cap from prices"
        " where ticker=? and date<=? and adj_close>0 order by date desc limit 1");
    st.bind(1, ticker);
    st.bind(2, date);
    if (!st.step()) return false;
    p.date = st.text(0);
    p.adj = st.number(1);
    p.volume = st.number(2);
    p.market_cap = st.number(3);
    return true;
}

std::vector<double> price_series(Db& db, const std::string& ticker, const std::string& date) {
    std::vector<double> p;
    auto st = db.prepare(
        "select adj_close from prices where ticker=? and date<=? and adj_close>0 order by date");
    st.bind(1, ticker);
    st.bind(2, date);
    while (st.step()) p.push_back(st.number(0));
    return p;
}

} // namespace eph
