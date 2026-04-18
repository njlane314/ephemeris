#!/bin/sh
set -eu

EPHEMERIS=${1:-./ephemeris}
tmp=${TMPDIR:-/tmp}/ephemeris-smoke-$$
db=$tmp/ephemeris.db
prices=$tmp/prices.csv
history=$tmp/security-history.csv

cleanup() {
        rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

mkdir -p "$tmp"
scripts/make-fixture-prices > "$prices"
cat > "$history" <<'EOF'
ticker,cik,valid_from,valid_to,source
SPY,1000001,2024-01-01,,fixture
AAA,1000002,2024-01-01,,fixture
BBB,1000003,2024-01-01,,fixture
CCC,1000004,2024-01-01,,fixture
DDD,1000005,2024-01-01,,fixture
EEE,1000006,2024-01-01,,fixture
FFF,1000007,2024-01-01,,fixture
GGG,1000008,2024-01-01,,fixture
EOF

assert_contains() {
        file=$1
        text=$2
        if ! grep -F "$text" "$file" >/dev/null; then
                echo "missing expected output: $text" >&2
                echo "from file: $file" >&2
                cat "$file" >&2
                exit 1
        fi
}

"$EPHEMERIS" db init --db "$db" > "$tmp/db.out"
assert_contains "$tmp/db.out" "db	$db"

"$EPHEMERIS" prices import --input "$prices" --db "$db" > "$tmp/prices.out"
assert_contains "$tmp/prices.out" "prices	2688"

"$EPHEMERIS" securities import-history --input "$history" --db "$db" > "$tmp/history.out"
assert_contains "$tmp/history.out" "security_identifier_history	8"

"$EPHEMERIS" universe build \
        --date 2025-04-11 \
        --min-market-cap 1000000000 \
        --min-adv 1000000 \
        --min-price 5 \
        --db "$db" > "$tmp/universe.out"
assert_contains "$tmp/universe.out" "eligible	8"

"$EPHEMERIS" signal momentum --date 2025-04-11 --db "$db" > "$tmp/signal.out"
assert_contains "$tmp/signal.out" "signals	8"
assert_contains "$tmp/signal.out" "1	EEE"

"$EPHEMERIS" regime filter \
        --date 2025-04-11 \
        --model models/momentum_regime.model \
        --db "$db" > "$tmp/regime.out"
assert_contains "$tmp/regime.out" "exposure	1"
assert_contains "$tmp/regime.out" "model_name	momentum_regime_v1"

"$EPHEMERIS" portfolio build \
        --date 2025-04-11 \
        --top 5 \
        --regime-model models/momentum_regime.model \
        --db "$db" > "$tmp/portfolio.out"
assert_contains "$tmp/portfolio.out" "holdings	5"
assert_contains "$tmp/portfolio.out" "CASH	0.8"

"$EPHEMERIS" universe explain \
        --date 2025-04-11 \
        --ticker EEE \
        --db "$db" > "$tmp/universe-explain.out"
assert_contains "$tmp/universe-explain.out" "eligible	1"
assert_contains "$tmp/universe-explain.out" "reason	eligible"
assert_contains "$tmp/universe-explain.out" "cik	1000006"

"$EPHEMERIS" signal explain \
        --date 2025-04-11 \
        --ticker EEE \
        --db "$db" > "$tmp/signal-explain.out"
assert_contains "$tmp/signal-explain.out" "rank	1"
assert_contains "$tmp/signal-explain.out" "252	2024-03-26"

"$EPHEMERIS" portfolio explain \
        --date 2025-04-11 \
        --db "$db" > "$tmp/portfolio-explain.out"
assert_contains "$tmp/portfolio-explain.out" "rule	long_only_equal_weight_capped"
assert_contains "$tmp/portfolio-explain.out" "EEE	0.04"

"$EPHEMERIS" backtest \
        --from 2024-12-02 \
        --to 2025-04-11 \
        --top 5 \
        --cost-bps 10 \
        --cost-model models/cost_default.model \
        --regime \
        --regime-model models/momentum_regime.model \
        --db "$db" > "$tmp/backtest.out"
assert_contains "$tmp/backtest.out" "cost_model	default_cost_v1"
assert_contains "$tmp/backtest.out" "rebalances	5"
assert_contains "$tmp/backtest.out" "end_nav	1.01224"

"$EPHEMERIS" audit database --db "$db" > "$tmp/audit-database.out"
assert_contains "$tmp/audit-database.out" "audit	database"
assert_contains "$tmp/audit-database.out" "security_identifier_history	8"

"$EPHEMERIS" audit prices --db "$db" > "$tmp/audit-prices.out"
assert_contains "$tmp/audit-prices.out" "audit	prices"
assert_contains "$tmp/audit-prices.out" "tickers	8"

"$EPHEMERIS" audit asof --date 2025-04-11 --db "$db" > "$tmp/audit-asof.out"
assert_contains "$tmp/audit-asof.out" "audit	asof"
assert_contains "$tmp/audit-asof.out" "eligible_universe_rows	8"

"$EPHEMERIS" report --date 2025-04-11 --db "$db" > "$tmp/report.out"
assert_contains "$tmp/report.out" "universe_eligible	8"
assert_contains "$tmp/report.out" "latest_nav	2025-04-11"

echo "smoke test passed"
