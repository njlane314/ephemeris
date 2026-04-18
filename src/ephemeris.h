#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
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
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace eph {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string trim(std::string s);
std::string lower(std::string s);
std::string upper(std::string s);
std::string keynorm(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
double to_double(const std::string& s, double fallback = 0.0);
long long to_i64(const std::string& s, long long fallback = 0);
std::vector<std::string> split(const std::string& s, char delim);
int date_days(const std::string& date);
std::string date_from_days(int z);
std::string date_add(const std::string& date, int days);
std::string month_key(const std::string& date);
void ensure_parent_dir(const std::string& path);

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& other) noexcept;
    Stmt& operator=(Stmt&& other) noexcept;

    void bind(int i, const std::string& v);
    void bind(int i, const char* v);
    void bind(int i, double v);
    void bind(int i, int v);
    void bind64(int i, long long v);
    void bind_null(int i);
    bool step();
    void run();
    void reset();
    std::string text(int i) const;
    double number(int i) const;
    long long i64(int i) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void exec(const std::string& sql);
    Stmt prepare(const std::string& sql);
    long long changes() const;

private:
    sqlite3* db_ = nullptr;
};

void init_schema(Db& db);

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

Json parse_json(const std::string& s);
std::string http_get(const std::string& url);
std::vector<std::string> csv_parse_line(const std::string& line);

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

Args parse_args(int argc, char** argv, int start);
std::string db_path(const Args& a);
std::vector<std::string> read_universe_file(const std::string& path);

void sync_submissions(const Args& a);
void sync_companyfacts(const Args& a);

struct Price {
    std::string date;
    double adj = 0.0;
    double volume = 0.0;
    double market_cap = 0.0;
};

void import_prices(const Args& a);
bool price_on_or_before(Db& db, const std::string& ticker, const std::string& date, Price& p);
std::vector<double> price_series(Db& db, const std::string& ticker, const std::string& date);

int build_universe(Db& db, const Args& a, bool quiet = false);
std::vector<std::string> eligible_tickers(Db& db, const std::string& date);

struct Signal {
    std::string ticker;
    double score = 0.0;
    double mom12 = 0.0;
    double mom6 = 0.0;
    double mom3 = 0.0;
    double vol3 = 0.0;
    int rank = 0;
};

double realized_vol(const std::vector<double>& p, int end_index, int n = 63);
std::vector<int> parse_lookbacks(const std::string& s);
std::vector<Signal> compute_momentum(Db& db, const std::string& date,
                                     const std::vector<int>& lookbacks,
                                     int skip_days, bool store);
void command_signal_momentum(const Args& a);

struct RegimeFeatures {
    double trend = 0.0;
    double vol = 0.0;
    double breadth = 0.5;
    double spread = 0.0;
    double concentration = 0.0;
    double drawdown = 0.0;
};

struct RegimeOut {
    RegimeFeatures f;
    double pf = 0.0;
    double pn = 0.0;
    double ps = 0.0;
    double edge = 0.0;
    double exposure = 1.0;
};

RegimeOut compute_regime(Db& db, const Args& a, bool store);
void command_regime_filter(const Args& a);
double exposure_for_date(Db& db, const std::string& date);

std::map<std::string, double> portfolio_weights(Db& db, const std::string& date, int topn,
                                                double exposure, double max_weight);
void command_portfolio_build(const Args& a);
void command_backtest(const Args& a);
void command_report(const Args& a);

int run(int argc, char** argv);

} // namespace eph
