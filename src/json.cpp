#include "ephemeris.h"

namespace eph {

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : s_(src) {}
    Json parse() {
        Json v = value();
        ws();
        if (i_ != s_.size()) throw Error("trailing JSON input");
        return v;
    }

private:
    void ws() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    char peek() {
        ws();
        if (i_ >= s_.size()) throw Error("unexpected end of JSON");
        return s_[i_];
    }
    bool take(char c) {
        ws();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!take(c)) throw Error(std::string("expected JSON character: ") + c);
    }
    Json value() {
        char c = peek();
        if (c == '"') return string();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == 't') return literal("true", true);
        if (c == 'f') return literal("false", false);
        if (c == 'n') return null();
        return number();
    }
    Json null() {
        if (s_.compare(i_, 4, "null") != 0) throw Error("bad JSON null");
        i_ += 4;
        return Json{};
    }
    Json literal(const char* lit, bool v) {
        size_t len = std::strlen(lit);
        if (s_.compare(i_, len, lit) != 0) throw Error("bad JSON literal");
        i_ += len;
        Json j;
        j.type = Json::Type::Bool;
        j.b = v;
        return j;
    }
    Json string() {
        expect('"');
        Json j;
        j.type = Json::Type::String;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return j;
            if (c != '\\') {
                j.s.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) throw Error("bad JSON escape");
            char e = s_[i_++];
            switch (e) {
                case '"': j.s.push_back('"'); break;
                case '\\': j.s.push_back('\\'); break;
                case '/': j.s.push_back('/'); break;
                case 'b': j.s.push_back('\b'); break;
                case 'f': j.s.push_back('\f'); break;
                case 'n': j.s.push_back('\n'); break;
                case 'r': j.s.push_back('\r'); break;
                case 't': j.s.push_back('\t'); break;
                case 'u':
                    if (i_ + 4 > s_.size()) throw Error("bad JSON unicode escape");
                    j.s.push_back('?');
                    i_ += 4;
                    break;
                default:
                    throw Error("bad JSON escape");
            }
        }
        throw Error("unterminated JSON string");
    }
    Json number() {
        ws();
        size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (start == i_) throw Error("bad JSON number");
        Json j;
        j.type = Json::Type::Number;
        j.n = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return j;
    }
    Json array() {
        expect('[');
        Json j;
        j.type = Json::Type::Array;
        if (take(']')) return j;
        for (;;) {
            j.a.push_back(value());
            if (take(']')) return j;
            expect(',');
        }
    }
    Json object() {
        expect('{');
        Json j;
        j.type = Json::Type::Object;
        if (take('}')) return j;
        for (;;) {
            Json k = string();
            expect(':');
            j.o.emplace(k.s, value());
            if (take('}')) return j;
            expect(',');
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

Json parse_json(const std::string& s) {
    return JsonParser(s).parse();
}

} // namespace eph
