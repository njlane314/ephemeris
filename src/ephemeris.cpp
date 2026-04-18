#include <sqlite3.h>
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eph {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string keynorm(std::string s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static double to_double(const std::string& s, double fallback = 0.0) {
    if (trim(s).empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (errno || end == s.c_str()) return fallback;
    return v;
}

static long long to_i64(const std::string& s, long long fallback = 0) {
    if (trim(s).empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return fallback;
    return v;
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(s);
    while (std::getline(in, item, delim)) out.push_back(trim(item));
    return out;
}

static int days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

static std::tuple<int, unsigned, unsigned> civil_from_days(int z) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    return {y, m, d};
}

static int date_days(const std::string& date) {
    if (date.size() < 10) throw Error("bad date: " + date);
    int y = static_cast<int>(to_i64(date.substr(0, 4), -1));
    int m = static_cast<int>(to_i64(date.substr(5, 2), -1));
    int d = static_cast<int>(to_i64(date.substr(8, 2), -1));
    if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) throw Error("bad date: " + date);
    return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

static std::string date_from_days(int z) {
    auto [y, m, d] = civil_from_days(z);
    std::ostringstream out;
    out << std::setw(4) << std::setfill('0') << y << '-'
        << std::setw(2) << std::setfill('0') << m << '-'
        << std::setw(2) << std::setfill('0') << d;
    return out.str();
}

static std::string date_add(const std::string& date, int days) {
    return date_from_days(date_days(date) + days);
}

static std::string month_key(const std::string& date) {
    return date.size() >= 7 ? date.substr(0, 7) : date;
}

static void ensure_parent_dir(const std::string& path) {
    std::filesystem::path p(path);
    if (!p.has_parent_path()) return;
    std::filesystem::create_directories(p.parent_path());
}

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw Error(sqlite3_errmsg(db_));
        }
    }
    ~Stmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
        other.stmt_ = nullptr;
    }
    Stmt& operator=(Stmt&& other) noexcept {
        if (this != &other) {
            if (stmt_) sqlite3_finalize(stmt_);
            db_ = other.db_;
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }

    void bind(int i, const std::string& v) {
        if (sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            throw Error(sqlite3_errmsg(db_));
        }
    }
    void bind(int i, const char* v) { bind(i, std::string(v ? v : "")); }
    void bind(int i, double v) {
        if (sqlite3_bind_double(stmt_, i, v) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
    }
    void bind(int i, int v) {
        if (sqlite3_bind_int(stmt_, i, v) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
    }
    void bind64(int i, long long v) {
        if (sqlite3_bind_int64(stmt_, i, static_cast<sqlite3_int64>(v)) != SQLITE_OK) {
            throw Error(sqlite3_errmsg(db_));
        }
    }
    void bind_null(int i) {
        if (sqlite3_bind_null(stmt_, i) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
    }
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw Error(sqlite3_errmsg(db_));
    }
    void run() {
        if (step()) throw Error("statement returned rows");
    }
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    std::string text(int i) const {
        const unsigned char* p = sqlite3_column_text(stmt_, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    double number(int i) const {
        return sqlite3_column_double(stmt_, i);
    }
    long long i64(int i) const {
        return sqlite3_column_int64(stmt_, i);
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Db {
public:
    explicit Db(const std::string& path) {
        ensure_parent_dir(path);
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
            throw Error(msg);
        }
        exec("pragma foreign_keys=on; pragma journal_mode=wal; pragma synchronous=normal;");
    }
    ~Db() {
        if (db_) sqlite3_close(db_);
    }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : sqlite3_errmsg(db_);
            sqlite3_free(err);
            throw Error(msg);
        }
    }
    Stmt prepare(const std::string& sql) { return Stmt(db_, sql); }
    long long changes() const { return sqlite3_changes64(db_); }

private:
    sqlite3* db_ = nullptr;
};

static const char* schema_sql = R"SQL(
create table if not exists companies (
    cik integer primary key,
    ticker text,
    name text not null default '',
    exchange text not null default '',
    sic text not null default '',
    fiscal_year_end text not null default '',
    updated_at text not null default current_timestamp
);

create unique index if not exists companies_ticker_idx
on companies(ticker) where ticker is not null and ticker <> '';

create table if not exists securities (
    ticker text primary key,
    cik integer,
    name text not null default '',
    exchange text not null default '',
    security_type text not null default 'common',
    sector text not null default '',
    sic text not null default '',
    active integer not null default 1,
    first_seen text,
    last_seen text
);

create table if not exists filings (
    cik integer not null,
    accession text not null,
    form text not null,
    filing_date text not null,
    report_date text not null default '',
    acceptance_datetime text not null default '',
    primary_document text not null default '',
    primary key (cik, accession)
);

create index if not exists filings_date_idx on filings(filing_date);
create index if not exists filings_form_idx on filings(form);

create table if not exists facts (
    cik integer not null,
    tag text not null,
    period_end text not null,
    filed text not null,
    form text not null default '',
    fy integer,
    fp text not null default '',
    unit text not null,
    value real not null,
    accession text not null default '',
    frame text not null default '',
    primary key (cik, tag, period_end, filed, unit, accession)
);

create index if not exists facts_lookup_idx on facts(cik, tag, filed, period_end);

create table if not exists prices (
    ticker text not null,
    date text not null,
    open real,
    high real,
    low real,
    close real,
    adj_close real not null,
    volume real not null default 0,
    market_cap real not null default 0,
    primary key (ticker, date)
);

create index if not exists prices_date_idx on prices(date);

create table if not exists universe (
    date text not null,
    ticker text not null,
    cik integer,
    eligible integer not null,
    reason text not null,
    price real not null default 0,
    market_cap real not null default 0,
    adv real not null default 0,
    primary key (date, ticker)
);

create index if not exists universe_date_eligible_idx on universe(date, eligible);

create table if not exists signals (
    date text not null,
    ticker text not null,
    score real not null,
    mom_12_1 real not null default 0,
    mom_6_1 real not null default 0,
    mom_3_1 real not null default 0,
    vol_3m real not null default 0,
    rank integer not null,
    primary key (date, ticker)
);

create index if not exists signals_date_rank_idx on signals(date, rank);

create table if not exists regime_features (
    date text primary key,
    market_trend real not null,
    market_vol real not null,
    breadth_200d real not null,
    momentum_spread real not null,
    winner_concentration real not null,
    momentum_drawdown real not null
);

create table if not exists regime_outputs (
    date text primary key,
    p_favorable real not null,
    p_neutral real not null,
    p_stress real not null,
    expected_momentum_edge real not null,
    exposure real not null
);

create table if not exists portfolio_targets (
    date text not null,
    ticker text not null,
    weight real not null,
    score real not null default 0,
    rank integer not null default 0,
    exposure real not null default 1,
    primary key (date, ticker)
);

create table if not exists backtest_nav (
    date text primary key,
    portfolio_value real not null,
    cash real not null,
    gross_exposure real not null,
    turnover real not null default 0,
    cost real not null default 0
);

create table if not exists backtest_trades (
    date text not null,
    ticker text not null,
    shares real not null,
    price real not null,
    notional real not null,
    cost real not null,
    reason text not null default 'rebalance'
);
)SQL";

static void init_schema(Db& db) {
    db.exec(schema_sql);
}

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }
    std::string str() const {
        if (is_string()) return s;
        if (is_number()) {
            std::ostringstream out;
            out << std::setprecision(17) << n;
            return out.str();
        }
        if (is_bool()) return b ? "true" : "false";
        return "";
    }
    double num(double fallback = 0.0) const { return is_number() ? n : fallback; }
    const Json& get(const std::string& k) const {
        static const Json null;
        auto it = o.find(k);
        return it == o.end() ? null : it->second;
    }
    const Json& at(size_t i) const {
        static const Json null;
        return i < a.size() ? a[i] : null;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : s_(src) {}
    Json parse() {
        Json v = value();
        ws();
        if (i_ != s_.size()) throw Error("trailing JSON input");
        return v;
    }

private:
    void ws() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    char peek() {
        ws();
        if (i_ >= s_.size()) throw Error("unexpected end of JSON");
        return s_[i_];
    }
    bool take(char c) {
        ws();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!take(c)) throw Error(std::string("expected JSON character: ") + c);
    }
    Json value() {
        char c = peek();
        if (c == '"') return string();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == 't') return literal("true", true);
        if (c == 'f') return literal("false", false);
        if (c == 'n') return null();
        return number();
    }
    Json null() {
        if (s_.compare(i_, 4, "null") != 0) throw Error("bad JSON null");
        i_ += 4;
        return Json{};
    }
    Json literal(const char* lit, bool v) {
        size_t len = std::strlen(lit);
        if (s_.compare(i_, len, lit) != 0) throw Error("bad JSON literal");
        i_ += len;
        Json j;
        j.type = Json::Type::Bool;
        j.b = v;
        return j;
    }
    Json string() {
        expect('"');
        Json j;
        j.type = Json::Type::String;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return j;
            if (c != '\\') {
                j.s.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) throw Error("bad JSON escape");
            char e = s_[i_++];
            switch (e) {
                case '"': j.s.push_back('"'); break;
                case '\\': j.s.push_back('\\'); break;
                case '/': j.s.push_back('/'); break;
                case 'b': j.s.push_back('\b'); break;
                case 'f': j.s.push_back('\f'); break;
                case 'n': j.s.push_back('\n'); break;
                case 'r': j.s.push_back('\r'); break;
                case 't': j.s.push_back('\t'); break;
                case 'u':
                    if (i_ + 4 > s_.size()) throw Error("bad JSON unicode escape");
                    j.s.push_back('?');
                    i_ += 4;
                    break;
                default:
                    throw Error("bad JSON escape");
            }
        }
        throw Error("unterminated JSON string");
    }
    Json number() {
        ws();
        size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (start == i_) throw Error("bad JSON number");
        Json j;
        j.type = Json::Type::Number;
        j.n = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return j;
    }
    Json array() {
        expect('[');
        Json j;
        j.type = Json::Type::Array;
        if (take(']')) return j;
        for (;;) {
            j.a.push_back(value());
            if (take(']')) return j;
            expect(',');
        }
    }
    Json object() {
        expect('{');
        Json j;
        j.type = Json::Type::Object;
        if (take('}')) return j;
        for (;;) {
            Json k = string();
            expect(':');
            j.o.emplace(k.s, value());
            if (take('}')) return j;
            expect(',');
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

static Json parse_json(const std::string& s) {
    return JsonParser(s).parse();
}

static size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string http_get(const std::string& url) {
    static bool curl_ready = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)curl_ready;

    CURL* curl = curl_easy_init();
    if (!curl) throw Error("curl init failed");
    std::string body;
    std::string ua = std::getenv("EPHEMERIS_USER_AGENT")
        ? std::getenv("EPHEMERIS_USER_AGENT")
        : "ephemeris/0.1 set-EPHEMERIS_USER_AGENT@example.invalid";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) throw Error(std::string("curl: ") + curl_easy_strerror(rc));
    if (status < 200 || status >= 300) {
        throw Error("HTTP " + std::to_string(status) + " for " + url);
    }
    return body;
}

static std::vector<std::string> csv_parse_line(const std::string& line) {
    std::vector<std::string> row;
    std::string cell;
    bool quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else if (c == '"') {
                quote = false;
            } else {
                cell.push_back(c);
            }
        } else if (c == '"') {
            quote = true;
        } else if (c == ',') {
            row.push_back(cell);
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    row.push_back(cell);
    return row;
}

struct CsvHeader {
    std::unordered_map<std::string, size_t> idx;
    explicit CsvHeader(const std::vector<std::string>& header) {
        for (size_t i = 0; i < header.size(); ++i) idx[keynorm(header[i])] = i;
    }
    std::string get(const std::vector<std::string>& row, std::initializer_list<const char*> names) const {
        for (const char* name : names) {
            auto it = idx.find(keynorm(name));
            if (it != idx.end() && it->second < row.size()) return trim(row[it->second]);
        }
        return "";
    }
};

struct Args {
    std::unordered_map<std::string, std::string> opt;
    std::set<std::string> flags;
    std::vector<std::string> pos;

    bool has(const std::string& k) const {
        return opt.count(k) || flags.count(k);
    }
    std::string get(const std::string& k, const std::string& fallback = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? fallback : it->second;
    }
    int geti(const std::string& k, int fallback = 0) const {
        return static_cast<int>(to_i64(get(k), fallback));
    }
    double getd(const std::string& k, double fallback = 0.0) const {
        return to_double(get(k), fallback);
    }
};

static Args parse_args(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string tok = argv[i];
        if (starts_with(tok, "--")) {
            std::string key = tok.substr(2);
            auto eq = key.find('=');
            if (eq != std::string::npos) {
                a.opt[key.substr(0, eq)] = key.substr(eq + 1);
            } else if (i + 1 < argc && !starts_with(argv[i + 1], "--")) {
                a.opt[key] = argv[++i];
            } else {
                a.flags.insert(key);
            }
        } else {
            a.pos.push_back(tok);
        }
    }
    return a;
}

static std::string db_path(const Args& a) {
    return a.get("db", "ephemeris.db");
}

static std::vector<std::string> read_universe_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw Error("open failed: " + path);
    std::vector<std::string> tickers;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        tickers.push_back(upper(line));
    }
    return tickers;
}

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

static std::unordered_map<std::string, SecCompany> fetch_sec_company_map() {
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

    std::unordered_map<std::string, SecCompany> out;
    for (const Json& row : root.get("data").a) {
        SecCompany c;
        c.cik = static_cast<long long>(row.at(cik_i).num());
        c.name = row.at(name_i).str();
        c.ticker = upper(row.at(ticker_i).str());
        c.exchange = row.at(exchange_i).str();
        if (!c.ticker.empty()) out[c.ticker] = c;
    }
    return out;
}

static void sync_submissions(const Args& a) {
    if (!a.has("universe")) throw Error("sec sync-submissions requires --universe");
    Db db(db_path(a));
    init_schema(db);

    auto wanted = read_universe_file(a.get("universe"));
    int limit = a.geti("limit", 0);
    auto map = fetch_sec_company_map();
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
        int processed = 0;
        for (const std::string& ticker : wanted) {
            if (limit && processed >= limit) break;
            auto it = map.find(ticker);
            if (it == map.end()) {
                std::cerr << "missing_sec_ticker\t" << ticker << "\n";
                continue;
            }
            ++processed;
            const SecCompany& c = it->second;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            Json sub;
            try {
                sub = parse_json(http_get("https://data.sec.gov/submissions/CIK" + cik10(c.cik) + ".json"));
            } catch (const std::exception& e) {
                std::cerr << "sync_error\t" << ticker << "\t" << e.what() << "\n";
                continue;
            }
            std::string name = sub.get("name").str().empty() ? c.name : sub.get("name").str();
            std::string sic = sub.get("sic").str();
            std::string fye = sub.get("fiscalYearEnd").str();
            std::string exchange = c.exchange;
            if (sub.get("exchanges").is_array() && !sub.get("exchanges").a.empty()) {
                exchange = sub.get("exchanges").at(0).str();
            }
            std::string primary_ticker = ticker;
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
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "companies\t" << companies << "\nfilings\t" << filings << "\n";
}

static void sync_companyfacts(const Args& a) {
    Db db(db_path(a));
    init_schema(db);
    int limit = a.geti("limit", 0);
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
    auto q = db.prepare("select cik from companies order by cik");
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
        }
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }
    std::cout << "companies\t" << processed << "\nfacts\t" << rows << "\n";
}

static void import_prices(const Args& a) {
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

struct Price {
    std::string date;
    double adj = 0.0;
    double volume = 0.0;
    double market_cap = 0.0;
};

static bool price_on_or_before(Db& db, const std::string& ticker, const std::string& date, Price& p) {
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

static std::vector<double> price_series(Db& db, const std::string& ticker, const std::string& date) {
    std::vector<double> p;
    auto st = db.prepare(
        "select adj_close from prices where ticker=? and date<=? and adj_close>0 order by date");
    st.bind(1, ticker);
    st.bind(2, date);
    while (st.step()) p.push_back(st.number(0));
    return p;
}

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

static int build_universe(Db& db, const Args& a, bool quiet = false) {
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

static std::vector<std::string> eligible_tickers(Db& db, const std::string& date) {
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

struct Signal {
    std::string ticker;
    double score = 0.0;
    double mom12 = 0.0;
    double mom6 = 0.0;
    double mom3 = 0.0;
    double vol3 = 0.0;
    int rank = 0;
};

static double realized_vol(const std::vector<double>& p, int end_index, int n = 63) {
    if (end_index <= 0) return 0.0;
    int start = std::max(1, end_index - n + 1);
    std::vector<double> r;
    for (int i = start; i <= end_index; ++i) {
        if (p[i] > 0.0 && p[i - 1] > 0.0) r.push_back(std::log(p[i] / p[i - 1]));
    }
    if (r.size() < 2) return 0.0;
    double mean = std::accumulate(r.begin(), r.end(), 0.0) / static_cast<double>(r.size());
    double ss = 0.0;
    for (double x : r) ss += (x - mean) * (x - mean);
    return std::sqrt(ss / static_cast<double>(r.size() - 1)) * std::sqrt(252.0);
}

static std::vector<int> parse_lookbacks(const std::string& s) {
    std::vector<int> out;
    for (const std::string& x : split(s.empty() ? "63,126,252" : s, ',')) {
        int n = static_cast<int>(to_i64(x, 0));
        if (n > 0) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) throw Error("no valid lookbacks");
    return out;
}

static std::vector<Signal> compute_momentum(Db& db, const std::string& date,
                                            const std::vector<int>& lookbacks,
                                            int skip_days, bool store) {
    std::vector<Signal> sigs;
    for (const std::string& ticker : eligible_tickers(db, date)) {
        auto p = price_series(db, ticker, date);
        int end = static_cast<int>(p.size()) - 1 - skip_days;
        if (end <= 0) continue;

        std::map<int, double> rets;
        bool enough = true;
        for (int lb : lookbacks) {
            if (end - lb < 0 || p[end - lb] <= 0.0) {
                enough = false;
                break;
            }
            rets[lb] = p[end] / p[end - lb] - 1.0;
        }
        if (!enough) continue;

        Signal s;
        s.ticker = ticker;
        s.vol3 = realized_vol(p, end, 63);
        s.mom3 = rets.count(63) ? rets[63] : rets.begin()->second;
        s.mom6 = rets.count(126) ? rets[126] : rets.rbegin()->second;
        s.mom12 = rets.count(252) ? rets[252] : rets.rbegin()->second;
        if (lookbacks.size() >= 3) {
            s.score = 0.50 * s.mom12 + 0.30 * s.mom6 + 0.20 * s.mom3 - 0.25 * s.vol3;
        } else {
            double sum = 0.0;
            for (auto& kv : rets) sum += kv.second;
            s.score = sum / static_cast<double>(rets.size()) - 0.25 * s.vol3;
        }
        sigs.push_back(s);
    }
    std::sort(sigs.begin(), sigs.end(), [](const Signal& a, const Signal& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.ticker < b.ticker;
    });
    for (size_t i = 0; i < sigs.size(); ++i) sigs[i].rank = static_cast<int>(i + 1);

    if (store) {
        auto del = db.prepare("delete from signals where date=?");
        del.bind(1, date);
        auto ins = db.prepare(
            "insert into signals(date,ticker,score,mom_12_1,mom_6_1,mom_3_1,vol_3m,rank)"
            " values(?,?,?,?,?,?,?,?)");
        db.exec("begin");
        try {
            del.run();
            for (const Signal& s : sigs) {
                ins.reset();
                ins.bind(1, date);
                ins.bind(2, s.ticker);
                ins.bind(3, s.score);
                ins.bind(4, s.mom12);
                ins.bind(5, s.mom6);
                ins.bind(6, s.mom3);
                ins.bind(7, s.vol3);
                ins.bind(8, s.rank);
                ins.run();
            }
            db.exec("commit");
        } catch (...) {
            db.exec("rollback");
            throw;
        }
    }
    return sigs;
}

static void command_signal_momentum(const Args& a) {
    std::string date = a.get("date");
    if (date.empty()) throw Error("signal momentum requires --date");
    Db db(db_path(a));
    init_schema(db);
    auto sigs = compute_momentum(db, date, parse_lookbacks(a.get("lookbacks")), a.geti("skip-days", 21), true);
    std::cout << "date\t" << date << "\nsignals\t" << sigs.size() << "\n";
    std::cout << "rank\tticker\tscore\tmom_12_1\tmom_6_1\tmom_3_1\tvol_3m\n";
    for (size_t i = 0; i < std::min<size_t>(20, sigs.size()); ++i) {
        const Signal& s = sigs[i];
        std::cout << s.rank << '\t' << s.ticker << '\t' << s.score << '\t'
                  << s.mom12 << '\t' << s.mom6 << '\t' << s.mom3 << '\t' << s.vol3 << "\n";
    }
}

static double ret_lookback(Db& db, const std::string& ticker, const std::string& date, int lookback) {
    auto p = price_series(db, ticker, date);
    if (static_cast<int>(p.size()) <= lookback || p[p.size() - 1 - lookback] <= 0.0) return 0.0;
    return p.back() / p[p.size() - 1 - lookback] - 1.0;
}

static double market_ret_proxy(Db& db, const std::string& date, int lookback) {
    double sum = 0.0;
    int n = 0;
    for (const std::string& t : eligible_tickers(db, date)) {
        double r = ret_lookback(db, t, date, lookback);
        if (r != 0.0 && std::isfinite(r)) {
            sum += r;
            ++n;
        }
        if (n >= 250) break;
    }
    return n ? sum / n : 0.0;
}

static double series_vol(Db& db, const std::string& ticker, const std::string& date, int n) {
    auto p = price_series(db, ticker, date);
    if (p.size() < 3) return 0.0;
    return realized_vol(p, static_cast<int>(p.size()) - 1, n);
}

struct RegimeFeatures {
    double trend = 0.0;
    double vol = 0.0;
    double breadth = 0.5;
    double spread = 0.0;
    double concentration = 0.0;
    double drawdown = 0.0;
};

static bool has_ticker(Db& db, const std::string& ticker) {
    auto st = db.prepare("select 1 from prices where ticker=? limit 1");
    st.bind(1, ticker);
    return st.step();
}

static double breadth_200d(Db& db, const std::string& date) {
    int above = 0;
    int total = 0;
    for (const std::string& t : eligible_tickers(db, date)) {
        auto p = price_series(db, t, date);
        if (p.size() < 200) continue;
        double avg = std::accumulate(p.end() - 200, p.end(), 0.0) / 200.0;
        if (p.back() > avg) ++above;
        ++total;
    }
    return total ? static_cast<double>(above) / total : 0.5;
}

static double momentum_spread(Db& db, const std::string& date) {
    std::vector<double> scores;
    auto st = db.prepare("select score from signals where date=? order by score desc");
    st.bind(1, date);
    while (st.step()) scores.push_back(st.number(0));
    if (scores.size() < 10) return 0.0;
    size_t dec = std::max<size_t>(1, scores.size() / 10);
    double top = std::accumulate(scores.begin(), scores.begin() + static_cast<long>(dec), 0.0) / dec;
    double med = scores[scores.size() / 2];
    return top - med;
}

static double winner_concentration(Db& db, const std::string& date, int topn) {
    std::map<std::string, int> buckets;
    int n = 0;
    auto st = db.prepare(
        "select s.ticker, coalesce(nullif(sec.sic,''), nullif(c.sic,''), sec.exchange, '')"
        " from signals s"
        " left join securities sec on sec.ticker=s.ticker"
        " left join companies c on c.cik=sec.cik"
        " where s.date=? order by s.rank limit ?");
    st.bind(1, date);
    st.bind(2, topn);
    while (st.step()) {
        std::string b = st.text(1);
        if (b.size() > 2 && std::isdigit(static_cast<unsigned char>(b[0]))) b = b.substr(0, 2);
        if (b.empty()) b = "unknown";
        buckets[b]++;
        ++n;
    }
    if (!n) return 0.0;
    double h = 0.0;
    for (auto& kv : buckets) {
        double w = static_cast<double>(kv.second) / n;
        h += w * w;
    }
    return h;
}

static double latest_drawdown(Db& db, const std::string& date) {
    auto st = db.prepare(
        "select portfolio_value from backtest_nav where date<=? order by date");
    st.bind(1, date);
    double peak = 0.0;
    double last = 0.0;
    while (st.step()) {
        last = st.number(0);
        peak = std::max(peak, last);
    }
    if (peak <= 0.0 || last <= 0.0) return 0.0;
    return std::max(0.0, 1.0 - last / peak);
}

static RegimeFeatures compute_regime_features(Db& db, const std::string& date, const std::string& market) {
    RegimeFeatures f;
    bool have_market = has_ticker(db, market);
    double r1 = have_market ? ret_lookback(db, market, date, 21) : market_ret_proxy(db, date, 21);
    double r3 = have_market ? ret_lookback(db, market, date, 63) : market_ret_proxy(db, date, 63);
    double r12 = have_market ? ret_lookback(db, market, date, 252) : market_ret_proxy(db, date, 252);
    f.trend = 0.45 * r12 + 0.35 * r3 + 0.20 * r1;
    f.vol = have_market ? series_vol(db, market, date, 63) : 0.22;
    f.breadth = breadth_200d(db, date);
    f.spread = momentum_spread(db, date);
    f.concentration = winner_concentration(db, date, 50);
    f.drawdown = latest_drawdown(db, date);
    return f;
}

static double log_t_kernel(double x, double mu, double scale, double nu) {
    double z = (x - mu) / std::max(scale, 1e-9);
    return -0.5 * (nu + 1.0) * std::log1p((z * z) / nu) - std::log(scale);
}

static std::array<double, 3> previous_regime(Db& db, const std::string& date) {
    auto st = db.prepare(
        "select p_favorable,p_neutral,p_stress from regime_outputs where date<? order by date desc limit 1");
    st.bind(1, date);
    if (st.step()) return {st.number(0), st.number(1), st.number(2)};
    return {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
}

struct RegimeOut {
    RegimeFeatures f;
    double pf = 0.0;
    double pn = 0.0;
    double ps = 0.0;
    double edge = 0.0;
    double exposure = 1.0;
};

static RegimeOut compute_regime(Db& db, const Args& a, bool store) {
    std::string date = a.get("date");
    if (date.empty()) throw Error("regime filter requires --date");
    std::string market = upper(a.get("market", "SPY"));
    auto lookbacks = parse_lookbacks("63,126,252");
    if (eligible_tickers(db, date).size() && momentum_spread(db, date) == 0.0) {
        compute_momentum(db, date, lookbacks, 21, true);
    }

    RegimeOut o;
    o.f = compute_regime_features(db, date, market);
    auto prev = previous_regime(db, date);

    double fav_bias = 2.5 * o.f.trend + 2.0 * (o.f.breadth - 0.50) - 2.0 * (o.f.vol - 0.20) - 1.5 * o.f.drawdown;
    double stress_bias = -2.5 * o.f.trend + 2.8 * (o.f.vol - 0.22) + 2.0 * (0.45 - o.f.breadth)
                       + 2.0 * o.f.drawdown + 1.2 * (o.f.concentration - 0.20);
    double neutral_bias = -0.35 * std::abs(fav_bias) - 0.20 * std::abs(stress_bias);

    double base[3][3] = {
        {0.88, 0.10, 0.02},
        {0.18, 0.70, 0.12},
        {0.05, 0.25, 0.70}
    };
    double bias[3] = {fav_bias, neutral_bias, stress_bias};
    double trans[3][3];
    for (int i = 0; i < 3; ++i) {
        double row_sum = 0.0;
        for (int j = 0; j < 3; ++j) {
            trans[i][j] = base[i][j] * std::exp(std::max(-4.0, std::min(4.0, bias[j])));
            row_sum += trans[i][j];
        }
        for (int j = 0; j < 3; ++j) trans[i][j] /= row_sum;
    }

    double pred[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) pred[j] += prev[i] * trans[i][j];
    }

    std::array<std::array<double, 6>, 3> mu {{
        { 0.12, 0.16, 0.66, 0.12, 0.12, 0.02 },
        { 0.03, 0.23, 0.50, 0.07, 0.18, 0.06 },
        {-0.10, 0.38, 0.30, 0.02, 0.30, 0.16 }
    }};
    std::array<double, 6> sc {{0.16, 0.10, 0.20, 0.14, 0.12, 0.10}};
    std::array<double, 6> x {{o.f.trend, o.f.vol, o.f.breadth, o.f.spread, o.f.concentration, o.f.drawdown}};
    double logp[3];
    for (int st = 0; st < 3; ++st) {
        logp[st] = std::log(std::max(pred[st], 1e-12));
        for (int k = 0; k < 6; ++k) logp[st] += log_t_kernel(x[k], mu[st][k], sc[k], 5.0);
    }
    double mx = std::max({logp[0], logp[1], logp[2]});
    double z = std::exp(logp[0] - mx) + std::exp(logp[1] - mx) + std::exp(logp[2] - mx);
    o.pf = std::exp(logp[0] - mx) / z;
    o.pn = std::exp(logp[1] - mx) / z;
    o.ps = std::exp(logp[2] - mx) / z;
    o.edge = 0.006 * o.pf + 0.000 * o.pn - 0.012 * o.ps;
    double raw = o.pf + 0.55 * o.pn + 0.10 * o.ps + 6.0 * o.edge;
    double min_exp = a.getd("min-exposure", 0.0);
    double max_exp = a.getd("max-exposure", 1.0);
    o.exposure = std::max(min_exp, std::min(max_exp, raw));

    if (store) {
        auto feat = db.prepare(
            "insert into regime_features(date,market_trend,market_vol,breadth_200d,momentum_spread,winner_concentration,momentum_drawdown)"
            " values(?,?,?,?,?,?,?)"
            " on conflict(date) do update set"
            " market_trend=excluded.market_trend, market_vol=excluded.market_vol,"
            " breadth_200d=excluded.breadth_200d, momentum_spread=excluded.momentum_spread,"
            " winner_concentration=excluded.winner_concentration, momentum_drawdown=excluded.momentum_drawdown");
        auto out = db.prepare(
            "insert into regime_outputs(date,p_favorable,p_neutral,p_stress,expected_momentum_edge,exposure)"
            " values(?,?,?,?,?,?)"
            " on conflict(date) do update set"
            " p_favorable=excluded.p_favorable, p_neutral=excluded.p_neutral, p_stress=excluded.p_stress,"
            " expected_momentum_edge=excluded.expected_momentum_edge, exposure=excluded.exposure");
        db.exec("begin");
        try {
            feat.bind(1, date);
            feat.bind(2, o.f.trend);
            feat.bind(3, o.f.vol);
            feat.bind(4, o.f.breadth);
            feat.bind(5, o.f.spread);
            feat.bind(6, o.f.concentration);
            feat.bind(7, o.f.drawdown);
            feat.run();
            out.bind(1, date);
            out.bind(2, o.pf);
            out.bind(3, o.pn);
            out.bind(4, o.ps);
            out.bind(5, o.edge);
            out.bind(6, o.exposure);
            out.run();
            db.exec("commit");
        } catch (...) {
            db.exec("rollback");
            throw;
        }
    }
    return o;
}

static void command_regime_filter(const Args& a) {
    Db db(db_path(a));
    init_schema(db);
    RegimeOut o = compute_regime(db, a, true);
    std::cout << "date\t" << a.get("date") << "\n";
    std::cout << "p_favorable\t" << o.pf << "\np_neutral\t" << o.pn << "\np_stress\t" << o.ps
              << "\nexpected_momentum_edge\t" << o.edge << "\nexposure\t" << o.exposure << "\n";
    std::cout << "market_trend\t" << o.f.trend << "\nmarket_vol\t" << o.f.vol
              << "\nbreadth_200d\t" << o.f.breadth << "\nmomentum_spread\t" << o.f.spread
              << "\nwinner_concentration\t" << o.f.concentration << "\nmomentum_drawdown\t" << o.f.drawdown << "\n";
}

static double exposure_for_date(Db& db, const std::string& date) {
    auto st = db.prepare("select exposure from regime_outputs where date<=? order by date desc limit 1");
    st.bind(1, date);
    return st.step() ? st.number(0) : 1.0;
}

static std::vector<Signal> load_ranked_signals(Db& db, const std::string& date, int topn) {
    std::vector<Signal> out;
    auto st = db.prepare(
        "select ticker,score,mom_12_1,mom_6_1,mom_3_1,vol_3m,rank"
        " from signals where date=? order by rank limit ?");
    st.bind(1, date);
    st.bind(2, topn);
    while (st.step()) {
        Signal s;
        s.ticker = st.text(0);
        s.score = st.number(1);
        s.mom12 = st.number(2);
        s.mom6 = st.number(3);
        s.mom3 = st.number(4);
        s.vol3 = st.number(5);
        s.rank = static_cast<int>(st.i64(6));
        out.push_back(s);
    }
    return out;
}

static std::map<std::string, double> portfolio_weights(Db& db, const std::string& date, int topn,
                                                       double exposure, double max_weight) {
    auto sigs = load_ranked_signals(db, date, topn);
    if (sigs.empty()) {
        sigs = compute_momentum(db, date, parse_lookbacks("63,126,252"), 21, true);
        if (static_cast<int>(sigs.size()) > topn) sigs.resize(topn);
    }
    std::map<std::string, double> w;
    if (sigs.empty() || exposure <= 0.0) return w;
    double each = std::min(max_weight, exposure / static_cast<double>(sigs.size()));
    for (const Signal& s : sigs) w[s.ticker] = each;
    return w;
}

static void command_portfolio_build(const Args& a) {
    std::string date = a.get("date");
    if (date.empty()) throw Error("portfolio build requires --date");
    Db db(db_path(a));
    init_schema(db);
    int topn = a.geti("top", 50);
    double maxw = a.getd("max-weight", 0.04);
    double exposure = exposure_for_date(db, date);
    if (a.has("regime") || a.has("regime-model") || a.has("model")) {
        exposure = compute_regime(db, a, true).exposure;
    }
    auto weights = portfolio_weights(db, date, topn, exposure, maxw);
    double used = 0.0;
    for (auto& kv : weights) used += kv.second;

    auto del = db.prepare("delete from portfolio_targets where date=?");
    del.bind(1, date);
    auto ins = db.prepare(
        "insert into portfolio_targets(date,ticker,weight,score,rank,exposure)"
        " values(?,?,?,?,?,?)");
    db.exec("begin");
    try {
        del.run();
        for (auto& kv : weights) {
            auto sig = db.prepare("select score,rank from signals where date=? and ticker=?");
            sig.bind(1, date);
            sig.bind(2, kv.first);
            double score = 0.0;
            int rank = 0;
            if (sig.step()) {
                score = sig.number(0);
                rank = static_cast<int>(sig.i64(1));
            }
            ins.reset();
            ins.bind(1, date);
            ins.bind(2, kv.first);
            ins.bind(3, kv.second);
            ins.bind(4, score);
            ins.bind(5, rank);
            ins.bind(6, exposure);
            ins.run();
        }
        ins.reset();
        ins.bind(1, date);
        ins.bind(2, "CASH");
        ins.bind(3, std::max(0.0, 1.0 - used));
        ins.bind(4, 0.0);
        ins.bind(5, 0);
        ins.bind(6, exposure);
        ins.run();
        db.exec("commit");
    } catch (...) {
        db.exec("rollback");
        throw;
    }

    std::cout << "date\t" << date << "\nexposure\t" << exposure << "\nholdings\t" << weights.size() << "\n";
    std::cout << "ticker\tweight\n";
    for (auto& kv : weights) std::cout << kv.first << '\t' << kv.second << "\n";
    std::cout << "CASH\t" << std::max(0.0, 1.0 - used) << "\n";
}

static std::vector<std::string> trading_dates(Db& db, const std::string& from, const std::string& to) {
    std::vector<std::string> dates;
    auto st = db.prepare("select distinct date from prices where date>=? and date<=? order by date");
    st.bind(1, from);
    st.bind(2, to);
    while (st.step()) dates.push_back(st.text(0));
    return dates;
}

static std::set<std::string> monthly_rebalance_dates(const std::vector<std::string>& dates) {
    std::set<std::string> out;
    std::string current_month;
    std::string last;
    for (const std::string& d : dates) {
        std::string m = month_key(d);
        if (!current_month.empty() && m != current_month) out.insert(last);
        current_month = m;
        last = d;
    }
    if (!last.empty()) out.insert(last);
    return out;
}

static double holding_value(Db& db, const std::map<std::string, double>& shares, const std::string& date) {
    double v = 0.0;
    for (auto& kv : shares) {
        Price p;
        if (price_on_or_before(db, kv.first, date, p)) v += kv.second * p.adj;
    }
    return v;
}

static void command_backtest(const Args& a) {
    std::string from = a.get("from");
    std::string to = a.get("to");
    if (from.empty() || to.empty()) throw Error("backtest requires --from and --to");
    Db db(db_path(a));
    init_schema(db);
    int topn = a.geti("top", 50);
    double maxw = a.getd("max-weight", 0.04);
    double cost_rate = a.getd("cost-bps", 10.0) / 10000.0;
    bool use_regime = a.has("regime") || a.has("regime-model");

    auto dates = trading_dates(db, from, to);
    if (dates.empty()) throw Error("no prices in backtest range");
    auto rebal = monthly_rebalance_dates(dates);

    auto nav_ins = db.prepare(
        "insert into backtest_nav(date,portfolio_value,cash,gross_exposure,turnover,cost)"
        " values(?,?,?,?,?,?)"
        " on conflict(date) do update set portfolio_value=excluded.portfolio_value,"
        " cash=excluded.cash,gross_exposure=excluded.gross_exposure,turnover=excluded.turnover,cost=excluded.cost");
    auto trd = db.prepare(
        "insert into backtest_trades(date,ticker,shares,price,notional,cost,reason)"
        " values(?,?,?,?,?,?,?)");

    std::map<std::string, double> shares;
    double cash = 1.0;
    double nav = 1.0;
    int rebalances = 0;

    auto del_nav = db.prepare("delete from backtest_nav where date>=? and date<=?");
    del_nav.bind(1, from);
    del_nav.bind(2, to);
    auto del_trd = db.prepare("delete from backtest_trades where date>=? and date<=?");
    del_trd.bind(1, from);
    del_trd.bind(2, to);

    del_nav.run();
    del_trd.run();
    for (const std::string& date : dates) {
        double hv = holding_value(db, shares, date);
        nav = cash + hv;
        double turnover = 0.0;
        double costs = 0.0;

        if (rebal.count(date)) {
            Args ua = a;
            ua.opt["date"] = date;
            build_universe(db, ua, true);
            compute_momentum(db, date, parse_lookbacks(a.get("lookbacks")), a.geti("skip-days", 21), true);
            double exposure = 1.0;
            if (use_regime) exposure = compute_regime(db, ua, true).exposure;
            auto target = portfolio_weights(db, date, topn, exposure, maxw);
            std::set<std::string> names;
            for (auto& kv : shares) names.insert(kv.first);
            for (auto& kv : target) names.insert(kv.first);
            for (const std::string& t : names) {
                Price p;
                if (!price_on_or_before(db, t, date, p) || p.adj <= 0.0) continue;
                double cur_sh = shares.count(t) ? shares[t] : 0.0;
                double cur_val = cur_sh * p.adj;
                double tgt_val = (target.count(t) ? target[t] : 0.0) * nav;
                double notional = tgt_val - cur_val;
                if (std::abs(notional) < 1e-12) continue;
                double cost = std::abs(notional) * cost_rate;
                double delta_sh = notional / p.adj;
                shares[t] = cur_sh + delta_sh;
                if (std::abs(shares[t]) < 1e-12) shares.erase(t);
                cash -= notional + cost;
                turnover += std::abs(notional) / std::max(nav, 1e-12);
                costs += cost;

                trd.reset();
                trd.bind(1, date);
                trd.bind(2, t);
                trd.bind(3, delta_sh);
                trd.bind(4, p.adj);
                trd.bind(5, notional);
                trd.bind(6, cost);
                trd.bind(7, "rebalance");
                trd.run();
            }
            ++rebalances;
            hv = holding_value(db, shares, date);
            nav = cash + hv;
        }

        hv = holding_value(db, shares, date);
        nav = cash + hv;
        nav_ins.reset();
        nav_ins.bind(1, date);
        nav_ins.bind(2, nav);
        nav_ins.bind(3, cash);
        nav_ins.bind(4, nav > 0.0 ? hv / nav : 0.0);
        nav_ins.bind(5, turnover);
        nav_ins.bind(6, costs);
        nav_ins.run();
    }

    double start = 0.0;
    double end = 0.0;
    double peak = 0.0;
    double maxdd = 0.0;
    auto q = db.prepare("select portfolio_value from backtest_nav where date>=? and date<=? order by date");
    q.bind(1, from);
    q.bind(2, to);
    while (q.step()) {
        double v = q.number(0);
        if (start == 0.0) start = v;
        end = v;
        peak = std::max(peak, v);
        if (peak > 0.0) maxdd = std::max(maxdd, 1.0 - v / peak);
    }
    double years = std::max(1.0 / 252.0, static_cast<double>(date_days(to) - date_days(from)) / 365.25);
    double cagr = start > 0.0 ? std::pow(end / start, 1.0 / years) - 1.0 : 0.0;
    std::cout << "from\t" << from << "\nto\t" << to << "\nrebalances\t" << rebalances
              << "\nstart_nav\t" << start << "\nend_nav\t" << end
              << "\ncagr\t" << cagr << "\nmax_drawdown\t" << maxdd << "\n";
}

static void command_report(const Args& a) {
    Db db(db_path(a));
    init_schema(db);
    std::string date = a.get("date");
    if (date.empty()) {
        auto st = db.prepare("select max(date) from prices");
        if (st.step()) date = st.text(0);
    }
    if (date.empty()) throw Error("report requires --date or prices");

    std::cout << "date\t" << date << "\n";
    auto u = db.prepare("select count(*), sum(eligible) from universe where date=?");
    u.bind(1, date);
    if (u.step()) std::cout << "universe_total\t" << u.i64(0) << "\nuniverse_eligible\t" << u.i64(1) << "\n";

    auto r = db.prepare(
        "select p_favorable,p_neutral,p_stress,expected_momentum_edge,exposure"
        " from regime_outputs where date<=? order by date desc limit 1");
    r.bind(1, date);
    if (r.step()) {
        std::cout << "p_favorable\t" << r.number(0) << "\np_neutral\t" << r.number(1)
                  << "\np_stress\t" << r.number(2) << "\nexpected_momentum_edge\t" << r.number(3)
                  << "\nexposure\t" << r.number(4) << "\n";
    }

    std::cout << "top_signals\nrank\tticker\tscore\n";
    auto s = db.prepare("select rank,ticker,score from signals where date=? order by rank limit 10");
    s.bind(1, date);
    while (s.step()) std::cout << s.i64(0) << '\t' << s.text(1) << '\t' << s.number(2) << "\n";

    std::cout << "portfolio\n";
    auto p = db.prepare("select ticker,weight from portfolio_targets where date=? order by weight desc,ticker limit 20");
    p.bind(1, date);
    while (p.step()) std::cout << p.text(0) << '\t' << p.number(1) << "\n";

    auto nav = db.prepare("select date,portfolio_value,cash,gross_exposure from backtest_nav where date<=? order by date desc limit 1");
    nav.bind(1, date);
    if (nav.step()) {
        std::cout << "latest_nav\t" << nav.text(0) << '\t' << nav.number(1)
                  << "\tcash\t" << nav.number(2) << "\tgross\t" << nav.number(3) << "\n";
    }
}

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

static int run(int argc, char** argv) {
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

int main(int argc, char** argv) {
    try {
        return eph::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "ephemeris: " << e.what() << "\n";
        return 1;
    }
}
