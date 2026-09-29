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
// P is a detail::parser_wrapper
template <auto P>
constexpr auto lexeme =
    pc::dsl::seq<{
        pc::dsl::whitespace().many().discard(), // skip zero or more whitespace
        P,                                      // the parser P
        pc::dsl::whitespace().many().discard(), // drop the result
    }>();

// symbol parsers for JSON syntax
constexpr auto lparen = lexeme<pc::dsl::symbol<"(">()>;
constexpr auto rparen = lexeme<pc::dsl::symbol<")">()>;
constexpr auto lbracket = lexeme<pc::dsl::symbol<"[">()>;
constexpr auto rbracket = lexeme<pc::dsl::symbol<"]">()>;
constexpr auto lbrace = lexeme<pc::dsl::symbol<"{">()>;
constexpr auto rbrace = lexeme<pc::dsl::symbol<"}">()>;
constexpr auto colon = lexeme<pc::dsl::symbol<":">()>;
constexpr auto comma = lexeme<pc::dsl::symbol<",">()>;

// helper parser to parse sequences separated by a specific separator
template <auto Sep>
consteval auto sepby(auto P) {
    auto tail = (pc::dsl::seq({
            Sep.discard(),
            P,
        }) | pc::dsl::map<[](auto&& element) {
            return std::forward<decltype(element)>(element);
        }>).many();

    return pc::dsl::seq({
        P,
        tail,
    }) | pc::dsl::map<[](auto&& first, auto&& rest) {
        using T = std::decay_t<decltype(first)>;
        std::vector<T> result;
        result.push_back(std::forward<decltype(first)>(first));
        for (auto&& val : rest) {
            result.push_back(std::forward<decltype(val)>(val));
        }
        return result;
    }>;
};

// the main JSON parser
// as JSON is recursive, we use a Y-combinator via dsl::lazy
constexpr auto json_parser = pc::dsl::lazy<[](auto self) {
    // parse JSON arrays
    auto array = pc::dsl::seq({
        lbracket.discard(),
        sepby<comma>(self).optional(),
        rbracket.discard(),
    }) | pc::dsl::map<[](auto&& opt) -> json_value {
        if (opt) {
            return json_value{std::move(*opt)};
        } else {
            return json_value{json_array{}};
        }
    }>;

    // string and numeric literals
    auto string_lit = pc::dsl::seq({
        pc::dsl::symbol<"\"">().discard(),
        pc::dsl::alphanumeric().many().recognize(),
        pc::dsl::symbol<"\"">().discard(),
    }) | pc::dsl::map<[](std::string_view s) -> json_value {
        return s;
    }>;

    auto numeric_lit = pc::dsl::seq({
        pc::dsl::symbol<"-">().optional().discard(), // optional minus sign
        pc::dsl::numeric().many().recognize(),
        pc::dsl::seq({
            pc::dsl::symbol<".">().discard(), // decimal point
            pc::dsl::numeric().many().recognize(),
        }).optional(), // optional fractional part
    }).recognize().guard<pc::preds::not_empty>() | pc::dsl::map<[](std::string_view s) -> json_value {
        double val;
        std::from_chars(s.data(), s.data() + s.size(), val);
        return val;
    }>;

    auto kwpair = pc::dsl::seq({
        string_lit,
        colon.cut().discard(), // cut to avoid backtracking after colon
        self,
    }) | pc::dsl::map<[](auto&& key_val, auto&& value) {
        return std::make_pair(key_val.data.template get<std::string_view>(), std::move(value));
    }>;

    auto object = pc::dsl::seq({
        lbrace.discard(),
        sepby<comma>(kwpair).optional(),
        rbrace.discard(),
    }) | pc::dsl::map<[](auto&& opt) -> json_value {
        if (opt) {
            return json_object{std::move(*opt)};
        } else {
            return json_object{};
        }
    }>;

    // finally, we use choice to combine all possible JSON value types
    return pc::dsl::choice({
        object,
        array,
        string_lit,
        numeric_lit,
        pc::dsl::symbol<"true">()  | pc::dsl::map<[](...) -> json_value { return true; }>,
        pc::dsl::symbol<"false">() | pc::dsl::map<[](...) -> json_value { return false; }>,
        pc::dsl::symbol<"null">()  | pc::dsl::map<[](...) -> json_value { return nullptr; }>,
    }) | pc::dsl::map<[](auto&& v) -> json_value {
        return std::forward<decltype(v)>(v).visit([](auto&& inner_val) -> json_value {
            return std::move(inner_val);
        });
    }>;
}>.fix<json_value>();

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
