#include "ephemeris.h"

namespace eph {

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

void command_backtest(const Args& a) {
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

} // namespace eph
