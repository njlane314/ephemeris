#!/bin/sh
set -eu

EPHEMERIS=${1:-./ephemeris}
tmp=${TMPDIR:-/tmp}/ephemeris-smoke-$$
db=$tmp/ephemeris.db
prices=$tmp/prices.csv

cleanup() {
        rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

mkdir -p "$tmp"
scripts/make-fixture-prices > "$prices"

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
        --regime \
        --regime-model models/momentum_regime.model \
        --db "$db" > "$tmp/backtest.out"
assert_contains "$tmp/backtest.out" "rebalances	5"
assert_contains "$tmp/backtest.out" "end_nav	1.01224"

"$EPHEMERIS" report --date 2025-04-11 --db "$db" > "$tmp/report.out"
assert_contains "$tmp/report.out" "universe_eligible	8"
assert_contains "$tmp/report.out" "latest_nav	2025-04-11"

echo "smoke test passed"
