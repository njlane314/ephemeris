#include "ephemeris.h"

#include <sqlite3.h>

namespace eph {

Stmt::Stmt(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw Error(sqlite3_errmsg(db_));
    }
}

Stmt::~Stmt() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Stmt::Stmt(Stmt&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

Stmt& Stmt::operator=(Stmt&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

void Stmt::bind(int i, const std::string& v) {
    if (sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        throw Error(sqlite3_errmsg(db_));
    }
}

void Stmt::bind(int i, const char* v) {
    bind(i, std::string(v ? v : ""));
}

void Stmt::bind(int i, double v) {
    if (sqlite3_bind_double(stmt_, i, v) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
}

void Stmt::bind(int i, int v) {
    if (sqlite3_bind_int(stmt_, i, v) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
}

void Stmt::bind64(int i, long long v) {
    if (sqlite3_bind_int64(stmt_, i, static_cast<sqlite3_int64>(v)) != SQLITE_OK) {
        throw Error(sqlite3_errmsg(db_));
    }
}

void Stmt::bind_null(int i) {
    if (sqlite3_bind_null(stmt_, i) != SQLITE_OK) throw Error(sqlite3_errmsg(db_));
}

bool Stmt::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw Error(sqlite3_errmsg(db_));
}

void Stmt::run() {
    if (step()) throw Error("statement returned rows");
}

void Stmt::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

std::string Stmt::text(int i) const {
    const unsigned char* p = sqlite3_column_text(stmt_, i);
    return p ? reinterpret_cast<const char*>(p) : "";
}

double Stmt::number(int i) const {
    return sqlite3_column_double(stmt_, i);
}

long long Stmt::i64(int i) const {
    return sqlite3_column_int64(stmt_, i);
}

Db::Db(const std::string& path) {
    ensure_parent_dir(path);
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
        throw Error(msg);
    }
    exec("pragma foreign_keys=on; pragma journal_mode=wal; pragma synchronous=normal;");
}

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

void Db::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        throw Error(msg);
    }
}

Stmt Db::prepare(const std::string& sql) {
    return Stmt(db_, sql);
}

long long Db::changes() const {
    return sqlite3_changes64(db_);
}

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

void init_schema(Db& db) {
    db.exec(schema_sql);
}

} // namespace eph
