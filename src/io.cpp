#include "ephemeris.h"

#include <curl/curl.h>

namespace eph {

static size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string http_get(const std::string& url) {
    static bool curl_ready = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)curl_ready;

    CURL* curl = curl_easy_init();
    if (!curl) throw Error("curl init failed");
    std::string body;
    std::string ua = std::getenv("EPHEMERIS_USER_AGENT")
        ? std::getenv("EPHEMERIS_USER_AGENT")
        : "ephemeris/0.1 set-EPHEMERIS_USER_AGENT@example.invalid";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) throw Error(std::string("curl: ") + curl_easy_strerror(rc));
    if (status < 200 || status >= 300) {
        throw Error("HTTP " + std::to_string(status) + " for " + url);
    }
    return body;
}

std::vector<std::string> csv_parse_line(const std::string& line) {
    std::vector<std::string> row;
    std::string cell;
    bool quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else if (c == '"') {
                quote = false;
            } else {
                cell.push_back(c);
            }
        } else if (c == '"') {
            quote = true;
        } else if (c == ',') {
            row.push_back(cell);
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    row.push_back(cell);
    return row;
}
Args parse_args(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string tok = argv[i];
        if (starts_with(tok, "--")) {
            std::string key = tok.substr(2);
            auto eq = key.find('=');
            if (eq != std::string::npos) {
                a.opt[key.substr(0, eq)] = key.substr(eq + 1);
            } else if (i + 1 < argc && !starts_with(argv[i + 1], "--")) {
                a.opt[key] = argv[++i];
            } else {
                a.flags.insert(key);
            }
        } else {
            a.pos.push_back(tok);
        }
    }
    return a;
}

std::string db_path(const Args& a) {
    return a.get("db", "ephemeris.db");
}

std::vector<std::string> read_universe_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw Error("open failed: " + path);
    std::vector<std::string> tickers;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        tickers.push_back(upper(line));
    }
    return tickers;
}

} // namespace eph
