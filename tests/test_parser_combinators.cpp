// Unit tests for parser_combinators2.
//
// These are intentionally dependency-free: a tiny CHECK macro plus asserts.
// Build with: cmake -S . -B build && cmake --build build && ctest --test-dir build

#include "parser_combinators2.hpp"

#include <cassert>
#include <cmath>
#include <print>
#include <string>
#include <string_view>

namespace pc = parser_combinators;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::println("FAIL {}:{}: {}", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------------
// primitives
// ---------------------------------------------------------------------------

static void test_primitives() {
    constexpr auto sym = pc::dsl::symbol<"hello">();
    {
        auto ctx = pc::context();
        auto r = pc::parser<sym>::parse("hello world", ctx);
        CHECK(r.success);
        CHECK(r.parsed == "hello");
        CHECK(r.remaining == " world");
        CHECK(!r.committed);
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<sym>::parse("help", ctx);
        CHECK(!r.success);
        CHECK(!r.committed);
        CHECK(ctx.error.message.find("hello") != std::string::npos);
    }

    constexpr auto a = pc::dsl::alpha();
    constexpr auto d = pc::dsl::numeric();
    constexpr auto w = pc::dsl::whitespace();
    {
        auto ctx = pc::context();
        CHECK(pc::parser<a>::parse("abc", ctx).success);
        CHECK(pc::parser<d>::parse("123", ctx).success);
        CHECK(pc::parser<w>::parse(" x", ctx).success);
        CHECK(!pc::parser<d>::parse("x", ctx).success);
    }
}

// ---------------------------------------------------------------------------
// seq / map / into / recognize
// ---------------------------------------------------------------------------

struct pair_out {
    int number;
    std::string_view word;
};

static void test_seq_map_into() {
    // "42!" -> {42, "!"}
    constexpr auto p = pc::dsl::seq({
        pc::dsl::numeric().many().recognize()
            | pc::dsl::map<[](std::string_view s) { return std::stoi(std::string(s)); }>,
        pc::dsl::symbol<"!">(),
    }) | pc::dsl::map<pc::dsl::into<pair_out>>;

    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("42!", ctx);
    CHECK(r.success);
    CHECK(r.parsed.number == 42);
    CHECK(r.parsed.word == "!");
}

static void test_recognize_keeps_consumed_text() {
    constexpr auto p = pc::dsl::seq({
        pc::dsl::symbol<"[">().discard(),
        pc::dsl::numeric().many().discard(),
        pc::dsl::symbol<"]">().discard(),
    }).recognize();

    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("[123]", ctx);
    CHECK(r.success);
    CHECK(r.parsed == "[123]");
    CHECK(r.remaining.empty());
}

// ---------------------------------------------------------------------------
// choice / variant
// ---------------------------------------------------------------------------

static void test_choice() {
    constexpr auto p = pc::dsl::choice({
        pc::dsl::symbol<"abc">(),
        pc::dsl::symbol<"xyz">(),
    });

    {
        auto ctx = pc::context();
        auto r = pc::parser<p>::parse("xyz", ctx);
        CHECK(r.success);
        CHECK(r.parsed.index() == 1);
        CHECK(r.parsed.get<1>() == "xyz");
        CHECK(r.parsed.visit([](std::string_view s) { return std::string(s); }) == "xyz");
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<p>::parse("nope", ctx);
        CHECK(!r.success);
        CHECK(!r.committed);
    }
}

// ---------------------------------------------------------------------------
// optional / many
// ---------------------------------------------------------------------------

static void test_optional_many() {
    constexpr auto opt = pc::dsl::symbol<"a">().optional();
    {
        auto ctx = pc::context();
        auto r = pc::parser<opt>::parse("bbb", ctx);
        CHECK(r.success);
        CHECK(!r.parsed.has_value());
        CHECK(!r.committed);
    }

    constexpr auto digits = pc::dsl::numeric().many().recognize();
    {
        auto ctx = pc::context();
        auto r = pc::parser<digits>::parse("123abc", ctx);
        CHECK(r.success);
        CHECK(r.parsed == "123");
        CHECK(r.remaining == "abc");
    }
    {
        // zero repetitions is a success that consumes nothing
        auto ctx = pc::context();
        auto r = pc::parser<digits>::parse("abc", ctx);
        CHECK(r.success);
        CHECK(r.parsed.empty());
    }
}

// ---------------------------------------------------------------------------
// label / guard
// ---------------------------------------------------------------------------

static void test_label() {
    constexpr auto p = pc::dsl::symbol<"world">().label<"expected world">();
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("nope", ctx);
    CHECK(!r.success);
    CHECK(ctx.error.message == "expected world");
}

static void test_guard() {
    constexpr auto p = pc::dsl::numeric().many().recognize()
        .guard<pc::preds::not_empty>()
        | pc::dsl::map<[](std::string_view s) { return std::stoi(std::string(s)); }>;

    {
        auto ctx = pc::context();
        auto r = pc::parser<p>::parse("77", ctx);
        CHECK(r.success);
        CHECK(r.parsed == 77);
    }
    {
        // empty digits -> guard fails without consuming -> empty failure
        auto ctx = pc::context();
        auto r = pc::parser<p>::parse("abc", ctx);
        CHECK(!r.success);
        CHECK(!r.committed);
    }
}

// ---------------------------------------------------------------------------
// Parsec consumption model regressions
// ---------------------------------------------------------------------------

// A: a zero-width success inside a sequence must not commit the sequence.
static void test_zero_width_success_backtracks() {
    constexpr auto p = pc::dsl::choice({
        pc::dsl::seq({
            pc::dsl::symbol<"a">().optional(),
            pc::dsl::symbol<"x">(),
        }),
        pc::dsl::symbol<"b">(),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("b", ctx);
    CHECK(r.success);
    CHECK(r.parsed.index() == 1);
}

// B: a committed failure inside a nested choice must propagate to the outer one.
static void test_nested_committed_failure_propagates() {
    constexpr auto p = pc::dsl::choice({
        pc::dsl::choice({
            pc::dsl::seq({pc::dsl::symbol<"a">(), pc::dsl::symbol<"x">()}),
        }),
        pc::dsl::symbol<"a">(),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// C: many must propagate a committed failure instead of silently stopping.
static void test_many_propagates_committed_failure() {
    constexpr auto p = pc::dsl::choice({
        pc::dsl::seq({pc::dsl::symbol<"a">(), pc::dsl::symbol<"x">()}).many(),
        pc::dsl::seq({pc::dsl::symbol<"a">(), pc::dsl::symbol<"b">()}),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// D: consuming input before failing makes the failure non-backtrackable.
static void test_consumed_failure_is_committed() {
    constexpr auto p = pc::dsl::seq({pc::dsl::symbol<"a">(), pc::dsl::symbol<"x">()});
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// E: cut makes an otherwise empty failure fatal.
static void test_cut_makes_failure_fatal() {
    constexpr auto with_cut = pc::dsl::choice({
        pc::dsl::seq({pc::dsl::symbol<"a">().cut(), pc::dsl::symbol<"b">()}),
        pc::dsl::symbol<"a">(),
    });
    auto ctx = pc::context();
    auto r = pc::parser<with_cut>::parse("ac", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// ---------------------------------------------------------------------------
// recursion (Y-combinator)
// ---------------------------------------------------------------------------

constexpr auto nested = pc::dsl::lazy<[](auto self) {
    return pc::dsl::choice({
        pc::dsl::seq({
            pc::dsl::symbol<"(">().discard(),
            self,
            pc::dsl::symbol<")">().discard(),
        }) | pc::dsl::map<[](auto depth) -> int { return depth + 1; }>,
        pc::dsl::symbol<"x">() | pc::dsl::map<[](...) -> int { return 0; }>,
    }) | pc::dsl::map<[](auto&& v) -> int {
        return std::forward<decltype(v)>(v).visit([](int depth) { return depth; });
    }>;
}>.fix<int>();

static void test_recursion() {
    auto parse_depth = [](std::string_view in) {
        auto ctx = pc::context();
        return pc::parser<nested>::parse(in, ctx);
    };
    CHECK(parse_depth("x").parsed == 0);
    CHECK(parse_depth("(x)").parsed == 1);
    CHECK(parse_depth("(((x)))").parsed == 3);
    CHECK(!parse_depth("(x").success);
}

// ---------------------------------------------------------------------------
// non-trivial values (generated aggregate / variant lifetimes)
// ---------------------------------------------------------------------------

static void test_non_trivial_values() {
    constexpr auto p = pc::dsl::seq({
        pc::dsl::symbol<"a">() | pc::dsl::map<[](...) { return std::string("first"); }>,
        pc::dsl::symbol<"b">() | pc::dsl::map<[](...) { return std::string("second"); }>,
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(r.success);
    auto joined = pc::apply(
        [](std::string a, std::string b) { return a + "+" + b; }, std::move(r.parsed));
    CHECK(joined == "first+second");
}

// ---------------------------------------------------------------------------
// satisfy / char / any_char / eof
// ---------------------------------------------------------------------------

static void test_satisfy_char_eof() {
    constexpr auto vowel = pc::dsl::satisfy<[](char c) {
        return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
    }>();
    {
        auto ctx = pc::context();
        auto r = pc::parser<vowel>::parse("e", ctx);
        CHECK(r.success);
        CHECK(r.parsed == 'e');
    }
    {
        auto ctx = pc::context();
        CHECK(!pc::parser<vowel>::parse("z", ctx).success);
    }
    {
        auto ctx = pc::context();
        CHECK(pc::parser<vowel>::parse("z", ctx).committed == false);
    }

    constexpr auto x = pc::dsl::char_<'x'>();
    {
        auto ctx = pc::context();
        CHECK(pc::parser<x>::parse("xyz", ctx).parsed == 'x');
    }

    constexpr auto anyc = pc::dsl::any_char();
    {
        auto ctx = pc::context();
        CHECK(pc::parser<anyc>::parse("Z", ctx).parsed == 'Z');
    }

    constexpr auto end = pc::dsl::eof();
    {
        auto ctx = pc::context();
        CHECK(pc::parser<end>::parse("", ctx).success);
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<end>::parse("x", ctx);
        CHECK(!r.success);
        CHECK(!r.committed);
    }
}

// ---------------------------------------------------------------------------
// many1 / count
// ---------------------------------------------------------------------------

static void test_many1_count() {
    constexpr auto digits = pc::dsl::numeric().many1().recognize();
    {
        auto ctx = pc::context();
        auto r = pc::parser<digits>::parse("123a", ctx);
        CHECK(r.success);
        CHECK(r.parsed == "123");
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<digits>::parse("a", ctx);
        CHECK(!r.success);
        CHECK(!r.committed);
    }

    constexpr auto three = pc::dsl::any_char().count<3>().recognize();
    {
        auto ctx = pc::context();
        auto r = pc::parser<three>::parse("abcd", ctx);
        CHECK(r.success);
        CHECK(r.parsed == "abc");
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<three>::parse("ab", ctx);
        CHECK(!r.success);
        CHECK(r.committed);
    }
}

// ---------------------------------------------------------------------------
// sep_by / sep_by1 / between
// ---------------------------------------------------------------------------

static void test_sep_by_between() {
    constexpr auto comma = pc::dsl::char_<','>();
    constexpr auto nums = pc::dsl::numeric().many1().recognize().sep_by(comma);
    {
        auto ctx = pc::context();
        auto r = pc::parser<nums>::parse("1,23,4", ctx);
        CHECK(r.success);
        CHECK(r.parsed.size() == 3);
        CHECK(r.parsed[0] == "1");
        CHECK(r.parsed[1] == "23");
        CHECK(r.parsed[2] == "4");
        CHECK(r.remaining.empty());
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<nums>::parse("", ctx);
        CHECK(r.success);
        CHECK(r.parsed.empty());
    }
    {
        // trailing separator is a consumed failure
        auto ctx = pc::context();
        auto r = pc::parser<nums>::parse("1,", ctx);
        CHECK(!r.success);
        CHECK(r.committed);
    }

    constexpr auto nums1 = pc::dsl::numeric().many1().recognize().sep_by1(comma);
    {
        auto ctx = pc::context();
        CHECK(!pc::parser<nums1>::parse("", ctx).success);
    }

    constexpr auto bracketed =
        pc::dsl::between(pc::dsl::char_<'['>(), pc::dsl::char_<']'>(), nums);
    {
        auto ctx = pc::context();
        auto r = pc::parser<bracketed>::parse("[1,2]", ctx);
        CHECK(r.success);
        CHECK(r.parsed.size() == 2);
        CHECK(r.remaining.empty());
    }
}

// ---------------------------------------------------------------------------
// look_ahead / not_followed_by
// ---------------------------------------------------------------------------

static void test_look_ahead_not() {
    constexpr auto la = pc::dsl::symbol<"ab">().look_ahead();
    {
        auto ctx = pc::context();
        auto r = pc::parser<la>::parse("abc", ctx);
        CHECK(r.success);
        CHECK(r.remaining == "abc"); // nothing consumed
    }

    constexpr auto not_x = pc::dsl::char_<'x'>().not_followed_by();
    {
        auto ctx = pc::context();
        auto r = pc::parser<not_x>::parse("y", ctx);
        CHECK(r.success);
        CHECK(r.remaining == "y");
    }
    {
        auto ctx = pc::context();
        CHECK(!pc::parser<not_x>::parse("x", ctx).success);
    }
}

// ---------------------------------------------------------------------------
// attempt (inverse of cut)
// ---------------------------------------------------------------------------

static void test_attempt() {
    constexpr auto p = pc::dsl::choice({
        pc::dsl::seq({pc::dsl::symbol<"ab">(), pc::dsl::symbol<"c">()}).attempt(),
        pc::dsl::seq({pc::dsl::symbol<"a">(), pc::dsl::symbol<"bd">()}),
    });
    // first alternative consumes "ab" then fails; attempt makes it backtrackable
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("abd", ctx);
    CHECK(r.success);
    CHECK(r.parsed.index() == 1);
}

// ---------------------------------------------------------------------------
// chainl1 / chainr1
// ---------------------------------------------------------------------------

static void test_chain() {
    constexpr auto number = pc::dsl::numeric().many1().recognize()
        | pc::dsl::map<[](std::string_view s) { return std::stod(std::string(s)); }>;

    constexpr auto plus = pc::dsl::char_<'+'>()
        | pc::dsl::map<[](...) { return [](double a, double b) { return a + b; }; }>;
    constexpr auto minus = pc::dsl::char_<'-'>()
        | pc::dsl::map<[](...) { return [](double a, double b) { return a - b; }; }>;

    constexpr auto add = pc::dsl::chainl1(number, plus);
    constexpr auto sub = pc::dsl::chainl1(number, minus);
    {
        auto ctx = pc::context();
        auto r = pc::parser<add>::parse("1+2+3", ctx);
        CHECK(r.success);
        CHECK(r.parsed == 6.0);
    }
    {
        auto ctx = pc::context();
        auto r = pc::parser<sub>::parse("10-3-2", ctx);
        CHECK(r.success);
        CHECK(r.parsed == 5.0); // left-associative: (10-3)-2
    }

    constexpr auto pow = pc::dsl::char_<'^'>()
        | pc::dsl::map<[](...) { return [](double a, double b) { return std::pow(a, b); }; }>;
    constexpr auto exp = pc::dsl::chainr1(number, pow);
    {
        auto ctx = pc::context();
        auto r = pc::parser<exp>::parse("2^3^2", ctx);
        CHECK(r.success);
        CHECK(r.parsed == 512.0); // right-associative: 2^(3^2)
    }
}

// ---------------------------------------------------------------------------
// hook application over the new combinators
// ---------------------------------------------------------------------------

constexpr auto hook_test_parser = pc::dsl::seq({
    pc::dsl::numeric().many1().recognize().sep_by(pc::dsl::char_<','>()),
    pc::dsl::eof(),
});
constexpr auto hook_test_hooked = pc::apply_hook<hook_test_parser>();

static void test_apply_hook() {
    auto ctx = pc::context();
    auto r = pc::parser<hook_test_hooked>::parse("1,2", ctx);
    CHECK(r.success);
}

int main() {
    test_primitives();
    test_seq_map_into();
    test_recognize_keeps_consumed_text();
    test_choice();
    test_optional_many();
    test_label();
    test_guard();
    test_zero_width_success_backtracks();
    test_nested_committed_failure_propagates();
    test_many_propagates_committed_failure();
    test_consumed_failure_is_committed();
    test_cut_makes_failure_fatal();
    test_recursion();
    test_non_trivial_values();
    test_satisfy_char_eof();
    test_many1_count();
    test_sep_by_between();
    test_look_ahead_not();
    test_attempt();
    test_chain();
    test_apply_hook();

    if (g_failures == 0) {
        std::println("all tests passed");
        return 0;
    }
    std::println("{} test(s) failed", g_failures);
    return 1;
}
