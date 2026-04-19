#include "ephemeris.h"

namespace eph {

double exposure_for_date(Db& db, const std::string& date) {
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

std::map<std::string, double> portfolio_weights(Db& db, const std::string& date, int topn,
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

void command_portfolio_build(const Args& a) {
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
    auto out = db.prepare(
        "select ticker,weight from portfolio_targets where date=?"
        " order by case when ticker='CASH' then 1 else 0 end, rank, ticker");
    out.bind(1, date);
    while (out.step()) std::cout << out.text(0) << '\t' << out.number(1) << "\n";
}

} // namespace eph
