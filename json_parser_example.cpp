// Example JSON parser using parser-combinators2.
//
// The grammar follows RFC 8259:
//   ws       = *( %x20 / %x09 / %x0A / %x0D )
//   value    = object / array / string / number / "true" / "false" / "null"
//   object   = "{" ws [ member *( "," ws member ) ] ws "}"
//   member   = string ws ":" ws value
//   array    = "[" ws [ value *( "," ws value ) ] ws "]"
//   string   = %x22 *( unescaped / escape ) %x22
//   number   = [ "-" ] int [ frac ] [ exp ]
//   int      = "0" / ( %x31-39 *DIGIT )
//   frac     = "." 1*DIGIT
//   exp      = ( "e" / "E" ) [ "+" / "-" ] 1*DIGIT
//
// The top-level parse requires the whole input to be a single value (EOF checked).

#include "parser_combinators2.hpp"

#include <charconv>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pc = parser_combinators;
namespace dsl = pc::dsl;

// ---------------------------------------------------------------------------
// JSON value model
// ---------------------------------------------------------------------------

struct json_value;
using json_object = std::map<std::string, json_value>;
using json_array = std::vector<json_value>;

struct json_value {
    std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        json_array,
        json_object
    > data;

    template<typename T>
    requires (!std::is_same_v<std::remove_cvref_t<T>, json_value>)
    json_value(T&& v) : data(std::forward<T>(v)) {}

    json_value(const json_value&) = default;
    json_value(json_value&&) = default;
    json_value& operator=(const json_value&) = default;
    json_value& operator=(json_value&&) = default;

    json_value() : data(nullptr) {}
};

void walk_json(const json_value& val, int indent = 0) {
    std::string indent_str(indent * 2, ' ');
    std::visit([&](auto&& inner_val) {
        using T = std::decay_t<decltype(inner_val)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            std::println("{}null", indent_str);
        } else if constexpr (std::is_same_v<T, bool>) {
            std::println("{}{}", indent_str, inner_val ? "true" : "false");
        } else if constexpr (std::is_same_v<T, double>) {
            std::println("{}{}", indent_str, inner_val);
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::println("{}\"{}\"", indent_str, inner_val);
        } else if constexpr (std::is_same_v<T, json_array>) {
            if (inner_val.empty()) {
                std::println("{}[]", indent_str);
                return;
            }
            std::println("{}[", indent_str);
            for (const auto& item : inner_val) {
                walk_json(item, indent + 1);
            }
            std::println("{}]", indent_str);
        } else if constexpr (std::is_same_v<T, json_object>) {
            std::println("{}{{", indent_str);
            for (const auto& [key, value] : inner_val) {
                std::println("{}  \"{}\":", indent_str, key);
                walk_json(value, indent + 2);
            }
            std::println("{}}}", indent_str);
        }
    }, val.data);
}

// ---------------------------------------------------------------------------
// escape decoding (parser already validated the escapes)
// ---------------------------------------------------------------------------

void append_utf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
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

unsigned hex4(std::string_view s) {
    unsigned v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else v |= static_cast<unsigned>(c - 'A' + 10);
    }
    return v;
}

std::string decode_json_string(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t pos = 0; pos < raw.size(); ) {
        char c = raw[pos];
        if (c != '\\') {
            out.push_back(c);
            ++pos;
            continue;
        }
        char e = raw[pos + 1];
        pos += 2;
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
                char32_t cp = hex4(raw.substr(pos, 4));
                pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= raw.size() &&
                    raw[pos] == '\\' && raw[pos + 1] == 'u') {
                    char32_t lo = hex4(raw.substr(pos + 2, 4));
                    pos += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default:
                // unreachable: the parser only accepts valid escapes
                out.push_back(e);
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON parser (RFC 8259)
// ---------------------------------------------------------------------------

// ws = *( %x20 / %x09 / %x0A / %x0D )
constexpr auto ws_char = dsl::satisfy<[](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}>();
constexpr auto ws0 = ws_char.many().discard();

// wrap a parser so that it skips JSON whitespace on both sides
consteval pc::parser_wrapper token(pc::parser_wrapper p) {
    return dsl::seq({ws0, p, ws0}).map(dsl::unwrap_fn{});
}

// string = %x22 *( unescaped / escape ) %x22
constexpr auto hex_digit = dsl::satisfy<[](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}>();
constexpr auto short_escape_char = dsl::satisfy<[](char c) {
    return c == '"' || c == '\\' || c == '/' || c == 'b' ||
           c == 'f' || c == 'n' || c == 'r' || c == 't';
}>();
constexpr auto escape_body = dsl::choice({
    short_escape_char,
    dsl::seq({dsl::char_<'u'>(), hex_digit, hex_digit, hex_digit, hex_digit}).recognize(),
});
constexpr auto escape = dsl::seq({dsl::char_<'\\'>(), escape_body}).recognize();
constexpr auto unescaped = dsl::satisfy<[](char c) {
    return static_cast<unsigned char>(c) >= 0x20 && c != '"' && c != '\\';
}>();
constexpr auto string_content = dsl::choice({escape, unescaped});

// yields the decoded std::string
constexpr auto string_body = dsl::seq({
    dsl::char_<'"'>().discard(),
    string_content.many().recognize(),
    dsl::char_<'"'>().discard(),
}).map(dsl::unwrap_fn{});
constexpr auto string_lit = token(string_body).map([](std::string_view raw) {
    return decode_json_string(raw);
});

// structural tokens
constexpr auto lbrace = token(dsl::char_<'{'>());
constexpr auto rbrace = token(dsl::char_<'}'>());
constexpr auto lbracket = token(dsl::char_<'['>());
constexpr auto rbracket = token(dsl::char_<']'>());
constexpr auto colon = token(dsl::char_<':'>());
constexpr auto comma = token(dsl::char_<','>());

// the main JSON value parser
constexpr auto json_parser = dsl::fix<json_value>([](auto self) {
    auto value = token(self);

    // number = [ "-" ] int [ frac ] [ exp ]
    auto number = token(dsl::seq({
        dsl::char_<'-'>().optional().discard(),
        dsl::choice({
            dsl::char_<'0'>(),
            dsl::seq({
                dsl::satisfy<[](char c) { return c >= '1' && c <= '9'; }>(),
                dsl::numeric().many().discard(),
            }),
        }).discard(),
        dsl::seq({
            dsl::char_<'.'>().discard(),
            dsl::numeric().many1().discard(),
        }).optional().discard(),
        dsl::seq({
            dsl::satisfy<[](char c) { return c == 'e' || c == 'E'; }>().discard(),
            dsl::choice({dsl::char_<'+'>(), dsl::char_<'-'>()}).optional().discard(),
            dsl::numeric().many1().discard(),
        }).optional().discard(),
    }).recognize()).map([](std::string_view raw) -> json_value {
        double val = 0;
        std::from_chars(raw.data(), raw.data() + raw.size(), val);
        return val;
    });

    auto string_value = string_lit.map([](std::string s) -> json_value {
        return s;
    });

    auto true_value = token(dsl::symbol<"true">()).map([](...) -> json_value { return true; });
    auto false_value = token(dsl::symbol<"false">()).map([](...) -> json_value { return false; });
    auto null_value = token(dsl::symbol<"null">()).map([](...) -> json_value { return nullptr; });

    // member = string ws ":" ws value
    auto member = dsl::seq({
        string_lit,
        colon.discard(),
        value,
    }).map([](std::string key, json_value val) {
        return std::make_pair(std::move(key), std::move(val));
    });

    // array = "[" ws [ value *( "," ws value ) ] ws "]"
    auto array = dsl::seq({
        lbracket.discard(),
        dsl::sep_by(value, comma).optional(),
        rbracket.discard(),
    }).map([](std::optional<std::vector<json_value>> opt) -> json_value {
        return opt.value_or(json_array{});
    });

    // object = "{" ws [ member *( "," ws member ) ] ws "}"
    auto object = dsl::seq({
        lbrace.discard(),
        dsl::sep_by(member, comma).optional(),
        rbrace.discard(),
    }).map([](std::optional<std::vector<std::pair<std::string, json_value>>> opt) -> json_value {
        json_object obj;
        if (opt) {
            for (auto& p : *opt) obj.emplace(std::move(p));
        }
        return obj;
    });

    return dsl::choice({
        object,
        array,
        string_value,
        number,
        true_value,
        false_value,
        null_value,
    }).map([](auto&& v) -> json_value {
        // choice() produces a generated tagged union (all inner types already
        // json_value); flatten it into json_value
        return std::forward<decltype(v)>(v).visit([](auto&& inner) -> json_value {
            return std::forward<decltype(inner)>(inner);
        });
    });
});

// value with surrounding whitespace, followed by end of input
constexpr auto json_text = dsl::seq({
    token(json_parser),
    dsl::eof().discard(),
}).map([](json_value v) -> json_value {
    return v;
});

// ---------------------------------------------------------------------------
// hook implementation for tracing
// ---------------------------------------------------------------------------

struct hook_impl : pc::parser_hook {
    int indent_level = 0;
    void on_enter(std::string_view name, std::string_view input) override {
        std::println("{:>{}}--> Entering parser '{}' with input '{}'", "", indent_level * 2, name, input);
        ++indent_level;
    }

    void on_exit(std::string_view name, std::string_view input) override {
        --indent_level;
        std::println("{:>{}}<-- Exiting parser '{}' with remaining input '{}'", "", indent_level * 2, name, input);
    }

    void on_failure(std::string_view name, std::string_view remaining) override {
        std::println("{:>{}}!!  Parser '{}' failed at remaining input '{}'", "", indent_level * 2, name, remaining);
    }
};

int main() {
    // set up parse context with hook
    hook_impl hook;
    auto parse_context = pc::context();
    parse_context.set_hook(&hook);

    // apply hook to json_text recursively
    // however due to the Y-combinator, it will only apply to the outermost level
    // to apply to all levels, it's best you define the parsers without Y-combinator first,
    // apply_hook to that, then use dsl::fix to wrap it into a Y-combinator
    // but for demonstration purposes, this is sufficient
    constexpr auto trace = pc::apply_hook<json_text>();
    auto str = R"({
        "key1": "value1",
        "escaped": "line\nbreak \u00e9 \/ \"quoted\"",
        "negative": -3.5e2,
        "array": [true, false, null, 0, -0, 1e-3],
        "nested": {"a": [114, 514]}
    })";
    auto result = pc::parser<trace>::parse(str, parse_context);

    // check result
    if (!result.success) {
        // if failed, print error message and position
        // note that the internal position is a pointer into the original input string
        // use diff to get the offset
        std::println("Parse failed: {}, pos={}", parse_context.error.message, parse_context.error.diff(str));
        return 1;
    }
    std::println("Parse succeeded");

    // print the parsed JSON value
    walk_json(result.parsed);
    return 0;
}
