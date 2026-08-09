// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "beamng_fsr/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bfsr {
namespace {

const Json kNullJson{};

void appendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

/// Emits the shortest representation that round-trips, and keeps integers
/// integral so a settings file does not fill up with "1.0" noise.
void appendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "0"; return; }
    if (v == static_cast<double>(static_cast<long long>(v)) &&
        v < 9.0e15 && v > -9.0e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
}

class Parser {
public:
    Parser(const std::string& t, std::string* err) : s_(t), err_(err) {}

    bool run(Json& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (i_ != s_.size()) return setErr("trailing characters after top-level value");
        return true;
    }

private:
    static constexpr int kMaxDepth = 64; ///< Guards against stack exhaustion on hostile input.

    bool setErr(const char* msg) {
        if (err_) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s at offset %zu", msg, i_);
            *err_ = buf;
        }
        return false;
    }

    void skipWs() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool literal(const char* lit) {
        const size_t n = std::strlen(lit);
        if (s_.compare(i_, n, lit) != 0) return false;
        i_ += n;
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > kMaxDepth) return setErr("nesting too deep");
        if (i_ >= s_.size()) return setErr("unexpected end of input");
        switch (s_[i_]) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string v;
                if (!parseString(v)) return false;
                out = Json(std::move(v));
                return true;
            }
            case 't': if (!literal("true"))  return setErr("invalid literal"); out = Json(true);  return true;
            case 'f': if (!literal("false")) return setErr("invalid literal"); out = Json(false); return true;
            case 'n': if (!literal("null"))  return setErr("invalid literal"); out = Json();      return true;
            default:  return parseNumber(out);
        }
    }

    bool parseObject(Json& out, int depth) {
        ++i_; // '{'
        out = Json::object();
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            skipWs();
            if (i_ >= s_.size() || s_[i_] != '"') return setErr("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return setErr("expected ':'");
            ++i_;
            skipWs();
            Json value;
            if (!parseValue(value, depth + 1)) return false;
            out.set(key, std::move(value));
            skipWs();
            if (i_ >= s_.size()) return setErr("unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return setErr("expected ',' or '}'");
        }
    }

    bool parseArray(Json& out, int depth) {
        ++i_; // '['
        out = Json::array();
        skipWs();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            skipWs();
            Json value;
            if (!parseValue(value, depth + 1)) return false;
            out.push(std::move(value));
            skipWs();
            if (i_ >= s_.size()) return setErr("unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return setErr("expected ',' or ']'");
        }
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(unsigned& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + k];
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            out = (out << 4) | d;
        }
        i_ += 4;
        return true;
    }

    bool parseString(std::string& out) {
        ++i_; // opening quote
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parseHex4(cp)) return setErr("bad \\u escape");
                    // Recombine a surrogate pair when the low half follows.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        const size_t save = i_;
                        i_ += 2;
                        unsigned lo = 0;
                        if (parseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i_ = save;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return setErr("unknown escape sequence");
            }
        }
        return setErr("unterminated string");
    }

    bool parseNumber(Json& out) {
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin) return setErr("invalid number");
        i_ += static_cast<size_t>(end - begin);
        out = Json(v);
        return true;
    }

    const std::string& s_;
    std::string* err_;
    size_t i_ = 0;
};

} // namespace

bool Json::asBool(bool def) const noexcept {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return num_ != 0.0;
    return def;
}

double Json::asNumber(double def) const noexcept {
    if (type_ == Type::Number) return num_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return def;
}

int Json::asInt(int def) const noexcept {
    if (type_ == Type::Number) return static_cast<int>(num_);
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return def;
}

std::string Json::asString(const std::string& def) const {
    if (type_ == Type::String) return str_;
    return def;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNullJson;
    const auto it = obj_.find(key);
    return it == obj_.end() ? kNullJson : it->second;
}

const Json& Json::at(size_t index) const {
    if (type_ != Type::Array || index >= arr_.size()) return kNullJson;
    return arr_[index];
}

size_t Json::size() const noexcept {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

bool Json::has(const std::string& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
}

void Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) { *this = Json::object(); }
    if (obj_.find(key) == obj_.end()) keyOrder_.push_back(key);
    obj_[key] = std::move(value);
}

void Json::push(Json value) {
    if (type_ != Type::Array) { *this = Json::array(); }
    arr_.push_back(std::move(value));
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const auto newlineIndent = [&](int d) {
        if (!pretty) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * d), ' ');
    };

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: appendNumber(out, num_); break;
        case Type::String: appendEscaped(out, str_); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t k = 0; k < arr_.size(); ++k) {
                if (k) out.push_back(',');
                newlineIndent(depth + 1);
                arr_[k].dumpTo(out, indent, depth + 1);
            }
            newlineIndent(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out.push_back('{');
            bool first = true;
            for (const auto& key : keyOrder_) {
                const auto it = obj_.find(key);
                if (it == obj_.end()) continue;
                if (!first) out.push_back(',');
                first = false;
                newlineIndent(depth + 1);
                appendEscaped(out, key);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                it->second.dumpTo(out, indent, depth + 1);
            }
            newlineIndent(depth);
            out.push_back('}');
            break;
        }
    }
}

Json Json::parse(const std::string& text, std::string* error) {
    if (error) error->clear();
    Json out;
    Parser p(text, error);
    if (!p.run(out)) return Json{};
    return out;
}

} // namespace bfsr
