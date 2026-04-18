#include "ephemeris.h"

namespace eph {

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
RegimeOut compute_regime(Db& db, const Args& a, bool store) {
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

void command_regime_filter(const Args& a) {
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

} // namespace eph
