#include "ephemeris.h"

namespace eph {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string keynorm(std::string s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

double to_double(const std::string& s, double fallback) {
    if (trim(s).empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (errno || end == s.c_str()) return fallback;
    return v;
}

long long to_i64(const std::string& s, long long fallback) {
    if (trim(s).empty()) return fallback;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return fallback;
    return v;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(s);
    while (std::getline(in, item, delim)) out.push_back(trim(item));
    return out;
}

int days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

std::tuple<int, unsigned, unsigned> civil_from_days(int z) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    return {y, m, d};
}

int date_days(const std::string& date) {
    if (date.size() < 10) throw Error("bad date: " + date);
    int y = static_cast<int>(to_i64(date.substr(0, 4), -1));
    int m = static_cast<int>(to_i64(date.substr(5, 2), -1));
    int d = static_cast<int>(to_i64(date.substr(8, 2), -1));
    if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) throw Error("bad date: " + date);
    return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

std::string date_from_days(int z) {
    auto [y, m, d] = civil_from_days(z);
    std::ostringstream out;
    out << std::setw(4) << std::setfill('0') << y << '-'
        << std::setw(2) << std::setfill('0') << m << '-'
        << std::setw(2) << std::setfill('0') << d;
    return out.str();
}

std::string date_add(const std::string& date, int days) {
    return date_from_days(date_days(date) + days);
}

std::string month_key(const std::string& date) {
    return date.size() >= 7 ? date.substr(0, 7) : date;
}

void ensure_parent_dir(const std::string& path) {
    std::filesystem::path p(path);
    if (!p.has_parent_path()) return;
    std::filesystem::create_directories(p.parent_path());
}

} // namespace eph
