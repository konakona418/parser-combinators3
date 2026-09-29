#include "parser_combinators2.hpp"

#if __has_include(<mimalloc-new-delete.h>)
#include <mimalloc-new-delete.h>
#endif

#include <benchmark/benchmark.h>

#include <charconv>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace pc = parser_combinators;
namespace dsl = pc::dsl;

struct json_value;
using json_kv_pair = std::pair<std::string_view, json_value>;
using json_object = std::vector<json_kv_pair>;
using json_array = std::vector<json_value>;

struct json_value {
    pc::detail::static_variant<
        std::nullptr_t,
        bool,
        double,
        std::string_view,
        json_array,
        json_object
    > data;

    template<typename T>
    requires (!std::is_same_v<std::remove_cvref_t<T>, json_value>)
    json_value(T&& v) : data(std::forward<T>(v)) {}

    json_value(const json_value&) = delete;
    json_value& operator=(const json_value&) = delete;
    json_value(json_value&& other) {
        data = std::move(other.data);
    }
    json_value& operator=(json_value&& other) {
        data = std::move(other.data);
        return *this;
    }

    json_value() : data(nullptr) {}
};

// example JSON parser

// lexeme parser that skips surrounding whitespace
// P is a pc::parser_wrapper
template <auto P>
constexpr auto lexeme = dsl::seq({
    dsl::whitespace().many().discard(), // skip zero or more whitespace
    P,                                  // the parser P
    dsl::whitespace().many().discard(), // drop the result
});

// symbol parsers for JSON syntax
constexpr auto lbracket = lexeme<dsl::symbol<"[" >()>;
constexpr auto rbracket = lexeme<dsl::symbol<"]">()>;
constexpr auto lbrace = lexeme<dsl::symbol<"{">()>;
constexpr auto rbrace = lexeme<dsl::symbol<"}">()>;
constexpr auto colon = lexeme<dsl::symbol<":">()>;
constexpr auto comma = lexeme<dsl::symbol<",">()>;

// helper parser to parse sequences separated by a specific separator
template <auto Sep>
consteval auto sepby(auto P) {
    auto tail_base = dsl::seq({Sep.discard(), P});
    auto tail = tail_base.map([](auto&& element) {
        return std::forward<decltype(element)>(element);
    }).many();

    auto seq_base = dsl::seq({P, tail});
    return seq_base.map([](auto&& first, auto&& rest) {
        using T = std::decay_t<decltype(first)>;
        std::vector<T> result;
        result.push_back(std::forward<decltype(first)>(first));
        for (auto&& val : rest) {
            result.push_back(std::forward<decltype(val)>(val));
        }
        return result;
    });
};

// the main JSON parser
// as JSON is recursive, we use a Y-combinator via dsl::fix
constexpr auto json_parser = dsl::fix<json_value>([](auto self) {
    // parse JSON arrays
    auto array_base = dsl::seq({
        lbracket.discard(),
        sepby<comma>(self).optional(),
        rbracket.discard(),
    });
    auto array = array_base.map([](auto&& opt) -> json_value {
        if (opt) {
            return json_value{std::move(*opt)};
        } else {
            return json_value{json_array{}};
        }
    });

    // string and numeric literals
    auto string_base = dsl::seq({
        dsl::symbol<"\"">().discard(),
        dsl::alphanumeric().many().recognize(),
        dsl::symbol<"\"">().discard(),
    });
    auto string_lit = string_base.map([](std::string_view s) -> json_value {
        return s;
    });

    auto number_base = dsl::seq({
        dsl::symbol<"-">().optional().discard(), // optional minus sign
        dsl::numeric().many().recognize(),
        dsl::seq({
            dsl::symbol<".">().discard(), // decimal point
            dsl::numeric().many().recognize(),
        }).optional(), // optional fractional part
    });
    auto numeric_lit = number_base.recognize()
        .guard(pc::preds::not_empty)
        .map([](std::string_view s) -> json_value {
            double val;
            std::from_chars(s.data(), s.data() + s.size(), val);
            return val;
        });

    auto member_base = dsl::seq({
        string_lit,
        colon.cut().discard(), // cut to avoid backtracking after colon
        self,
    });
    auto kwpair = member_base.map([](auto&& key_val, auto&& value) {
        return std::make_pair(key_val.data.template get<std::string_view>(), std::move(value));
    });

    auto object_base = dsl::seq({
        lbrace.discard(),
        sepby<comma>(kwpair).optional(),
        rbrace.discard(),
    });
    auto object = object_base.map([](auto&& opt) -> json_value {
        if (opt) {
            return json_object{std::move(*opt)};
        } else {
            return json_object{};
        }
    });

    // finally, we use choice to combine all possible JSON value types
    auto choice_base = dsl::choice({
        object,
        array,
        string_lit,
        numeric_lit,
        dsl::symbol<"true">().map([](...) -> json_value { return true; }),
        dsl::symbol<"false">().map([](...) -> json_value { return false; }),
        dsl::symbol<"null">().map([](...) -> json_value { return nullptr; }),
    });
    return choice_base.map([](auto&& v) -> json_value {
        return std::forward<decltype(v)>(v).visit([](auto&& inner_val) -> json_value {
            return std::move(inner_val);
        });
    });
});

std::string read_file_to_string(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        return "";
    }
    std::string content;
    file.seekg(0, std::ios::end);
    content.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(&content[0], content.size());
    return content;
}

#ifdef _WIN32
extern "C" {
    using HANDLE = void*;
    using DWORD_PTR = unsigned __int64;
    using BOOL = int;

    __declspec(dllimport) HANDLE __stdcall GetCurrentThread();
    __declspec(dllimport) DWORD_PTR __stdcall SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
    __declspec(dllimport) HANDLE __stdcall GetCurrentProcess();
    __declspec(dllimport) BOOL __stdcall SetPriorityClass(HANDLE hProcess, unsigned long dwPriorityClass);
}

constexpr unsigned long HIGH_PRIORITY_CLASS = 0x00000080;
#endif

void SetAffinity(const benchmark::State& state) {
#ifdef _WIN32
    HANDLE thread = GetCurrentThread();
    SetThreadAffinityMask(thread, 1); // bind to CPU 0

    HANDLE process = GetCurrentProcess();
    SetPriorityClass(process, HIGH_PRIORITY_CLASS);
#else
    (void)state;
#endif
}

auto parse_context = pc::context();

static void BM_JsonParse(benchmark::State& state) {
    static std::string content = [] {
        return read_file_to_string("test_large.json");
    }();

    if (content.empty()) {
        state.SkipWithError("test_large.json not found");
        return;
    }

    for (auto _ : state) {
        auto result = pc::parser<json_parser>::parse(content, parse_context);
        benchmark::DoNotOptimize(result);
        volatile bool ok = result.success;
        if (!ok) {
            state.SkipWithError("Parse failed");
        }
    }

    state.SetBytesProcessed(state.iterations() * content.size());
}

BENCHMARK(BM_JsonParse)
    ->Threads(1)
    ->Setup(SetAffinity)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(30)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

BENCHMARK_MAIN();
