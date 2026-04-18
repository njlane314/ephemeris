#include "ephemeris.h"

namespace eph {

void command_report(const Args& a) {
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

} // namespace eph
