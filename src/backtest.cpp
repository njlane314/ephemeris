#include "ephemeris.h"

namespace eph {

struct CostModel {
    std::string name = "inline_cost_bps";
    std::string description = "inline cost-bps argument";
    double spread_bps = 0.0;
    double slippage_bps = 0.0;
    double impact_coefficient = 0.0;
};

static CostModel load_cost_model(const Args& a) {
    CostModel m;
    m.slippage_bps = a.getd("cost-bps", 10.0);
    std::string path = a.get("cost-model");
    if (path.empty()) return m;

    std::ifstream file(path);
    if (!file) throw Error("open failed: " + path);
    m = CostModel{};
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "name") {
            in >> m.name;
        } else if (key == "spread_bps") {
            in >> m.spread_bps;
        } else if (key == "slippage_bps") {
            in >> m.slippage_bps;
        } else if (key == "impact_coefficient") {
            in >> m.impact_coefficient;
        } else if (key == "description") {
            std::getline(in, m.description);
            m.description = trim(m.description);
        } else {
            throw Error("unknown cost model key: " + key);
        }
    }
    return m;
}

static void store_cost_model(Db& db, const CostModel& m) {
    auto st = db.prepare(
        "insert into transaction_cost_models(model_name,spread_bps,slippage_bps,impact_coefficient,active,description)"
        " values(?,?,?,?,1,?)"
        " on conflict(model_name) do update set"
        " spread_bps=excluded.spread_bps, slippage_bps=excluded.slippage_bps,"
        " impact_coefficient=excluded.impact_coefficient, active=1, description=excluded.description");
    st.bind(1, m.name);
    st.bind(2, m.spread_bps);
    st.bind(3, m.slippage_bps);
    st.bind(4, m.impact_coefficient);
    st.bind(5, m.description);
    st.run();
}

static double universe_adv(Db& db, const std::string& date, const std::string& ticker) {
    auto st = db.prepare("select adv from universe where date=? and ticker=?");
    st.bind(1, date);
    st.bind(2, ticker);
    return st.step() ? st.number(0) : 0.0;
}

static double trade_cost(Db& db, const CostModel& m, const std::string& date,
                         const std::string& ticker, double notional) {
    double base_rate = (m.spread_bps + m.slippage_bps) / 10000.0;
    double adv = universe_adv(db, date, ticker);
    double impact_rate = 0.0;
    if (m.impact_coefficient > 0.0 && adv > 0.0) {
        impact_rate = m.impact_coefficient * std::abs(notional) / adv;
    }
    return std::abs(notional) * (base_rate + impact_rate);
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

void command_backtest(const Args& a) {
    std::string from = a.get("from");
    std::string to = a.get("to");
    if (from.empty() || to.empty()) throw Error("backtest requires --from and --to");
    Db db(db_path(a));
    init_schema(db);
    int topn = a.geti("top", 50);
    double maxw = a.getd("max-weight", 0.04);
    CostModel cost_model = load_cost_model(a);
    store_cost_model(db, cost_model);
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
                double cost = trade_cost(db, cost_model, date, t, notional);
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
    std::cout << "from\t" << from << "\nto\t" << to << "\ncost_model\t" << cost_model.name
              << "\nrebalances\t" << rebalances
              << "\nstart_nav\t" << start << "\nend_nav\t" << end
              << "\ncagr\t" << cagr << "\nmax_drawdown\t" << maxdd << "\n";
}

} // namespace eph
