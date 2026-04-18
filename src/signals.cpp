#include "ephemeris.h"

namespace eph {

double realized_vol(const std::vector<double>& p, int end_index, int n) {
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

std::vector<int> parse_lookbacks(const std::string& s) {
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

std::vector<Signal> compute_momentum(Db& db, const std::string& date,
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

void command_signal_momentum(const Args& a) {
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

} // namespace eph
