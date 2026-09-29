// Unit tests for parser_combinators2.
//
// These are intentionally dependency-free: a tiny CHECK macro plus asserts.
// Build with: cmake -S . -B build && cmake --build build && ctest --test-dir build
//
// Note: GCC 16 misparses an inline lambda used as a template argument directly
// after a call/braced-init receiver, e.g. `seq({...}).map<[](auto...){...}>()`.
// The tests therefore bind receivers to named variables first.

#include "parser_combinators2.hpp"

#include <cassert>
#include <cmath>
#include <print>
#include <string>
#include <string_view>

namespace pc = parser_combinators;
namespace dsl = pc::dsl;

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
    constexpr auto sym = dsl::symbol<"hello">();
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

    constexpr auto a = dsl::alpha();
    constexpr auto d = dsl::numeric();
    constexpr auto w = dsl::whitespace();
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
    constexpr auto int_part = dsl::numeric().many().recognize();
    constexpr auto integer = int_part.map([](std::string_view s) {
        return std::stoi(std::string(s));
    });
    constexpr auto bang = dsl::symbol<"!">();
    constexpr auto seq_base = dsl::seq({integer, bang});
    constexpr auto p = seq_base.map(dsl::into<pair_out>);

    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("42!", ctx);
    CHECK(r.success);
    CHECK(r.parsed.number == 42);
    CHECK(r.parsed.word == "!");
}

static void test_recognize_keeps_consumed_text() {
    constexpr auto seq_base = dsl::seq({
        dsl::symbol<"[">().discard(),
        dsl::numeric().many().discard(),
        dsl::symbol<"]">().discard(),
    });
    constexpr auto p = seq_base.recognize();

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
    constexpr auto p = dsl::choice({
        dsl::symbol<"abc">(),
        dsl::symbol<"xyz">(),
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
    constexpr auto opt = dsl::symbol<"a">().optional();
    {
        auto ctx = pc::context();
        auto r = pc::parser<opt>::parse("bbb", ctx);
        CHECK(r.success);
        CHECK(!r.parsed.has_value());
        CHECK(!r.committed);
    }

    constexpr auto digits = dsl::numeric().many().recognize();
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
    constexpr auto p = dsl::symbol<"world">().label<"expected world">();
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("nope", ctx);
    CHECK(!r.success);
    CHECK(ctx.error.message == "expected world");
}

static void test_guard() {
    constexpr auto digits = dsl::numeric().many().recognize();
    constexpr auto guarded = digits.guard(pc::preds::not_empty);
    constexpr auto p = guarded.map([](std::string_view s) {
        return std::stoi(std::string(s));
    });

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
    constexpr auto p = dsl::choice({
        dsl::seq({
            dsl::symbol<"a">().optional(),
            dsl::symbol<"x">(),
        }),
        dsl::symbol<"b">(),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("b", ctx);
    CHECK(r.success);
    CHECK(r.parsed.index() == 1);
}

// B: a committed failure inside a nested choice must propagate to the outer one.
static void test_nested_committed_failure_propagates() {
    constexpr auto p = dsl::choice({
        dsl::choice({
            dsl::seq({dsl::symbol<"a">(), dsl::symbol<"x">()}),
        }),
        dsl::symbol<"a">(),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// C: many must propagate a committed failure instead of silently stopping.
static void test_many_propagates_committed_failure() {
    constexpr auto seq_fail = dsl::seq({dsl::symbol<"a">(), dsl::symbol<"x">()});
    constexpr auto p = dsl::choice({
        seq_fail.many(),
        dsl::seq({dsl::symbol<"a">(), dsl::symbol<"b">()}),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// D: consuming input before failing makes the failure non-backtrackable.
static void test_consumed_failure_is_committed() {
    constexpr auto p = dsl::seq({dsl::symbol<"a">(), dsl::symbol<"x">()});
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// E: cut makes an otherwise empty failure fatal.
static void test_cut_makes_failure_fatal() {
    constexpr auto with_cut = dsl::choice({
        dsl::seq({dsl::symbol<"a">().cut(), dsl::symbol<"b">()}),
        dsl::symbol<"a">(),
    });
    auto ctx = pc::context();
    auto r = pc::parser<with_cut>::parse("ac", ctx);
    CHECK(!r.success);
    CHECK(r.committed);
}

// ---------------------------------------------------------------------------
// recursion (Y-combinator) via dsl::fix
// ---------------------------------------------------------------------------

constexpr auto nested = dsl::fix<int>([](auto self) {
    auto paren_base = dsl::seq({
        dsl::char_<'('>().discard(),
        self,
        dsl::char_<')'>().discard(),
    });
    auto paren = paren_base.map([](auto depth) -> int { return depth + 1; });

    auto x_base = dsl::char_<'x'>();
    auto x = x_base.map([](...) -> int { return 0; });

    auto options = dsl::choice({paren, x});
    return options.map([](auto&& v) -> int {
        return std::forward<decltype(v)>(v).visit([](int depth) { return depth; });
    });
});

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
    constexpr auto a = dsl::symbol<"a">();
    constexpr auto b = dsl::symbol<"b">();
    constexpr auto p = dsl::seq({
        a.map([](...) { return std::string("first"); }),
        b.map([](...) { return std::string("second"); }),
    });
    auto ctx = pc::context();
    auto r = pc::parser<p>::parse("ab", ctx);
    CHECK(r.success);
    auto joined = pc::apply(
        [](std::string a_, std::string b_) { return a_ + "+" + b_; }, std::move(r.parsed));
    CHECK(joined == "first+second");
}

// ---------------------------------------------------------------------------
// satisfy / char / any_char / eof
// ---------------------------------------------------------------------------

static void test_satisfy_char_eof() {
    constexpr auto vowel = dsl::satisfy<[](char c) {
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
        CHECK(!pc::parser<vowel>::parse("z", ctx).committed);
    }

    constexpr auto x = dsl::char_<'x'>();
    {
        auto ctx = pc::context();
        CHECK(pc::parser<x>::parse("xyz", ctx).parsed == 'x');
    }

    constexpr auto anyc = dsl::any_char();
    {
        auto ctx = pc::context();
        CHECK(pc::parser<anyc>::parse("Z", ctx).parsed == 'Z');
    }

    constexpr auto end = dsl::eof();
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
    constexpr auto digits = dsl::numeric().many1().recognize();
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

    constexpr auto any3_base = dsl::any_char().count<3>();
    constexpr auto three = any3_base.recognize();
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
    constexpr auto comma = dsl::char_<','>();
    constexpr auto element = dsl::numeric().many1().recognize();
    constexpr auto nums = dsl::sep_by(element, comma);
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

    constexpr auto nums1 = dsl::sep_by1(element, comma);
    {
        auto ctx = pc::context();
        CHECK(!pc::parser<nums1>::parse("", ctx).success);
    }

    constexpr auto bracketed = dsl::between(dsl::char_<'['>(), dsl::char_<']'>(), nums);
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
    constexpr auto la = dsl::symbol<"ab">().look_ahead();
    {
        auto ctx = pc::context();
        auto r = pc::parser<la>::parse("abc", ctx);
        CHECK(r.success);
        CHECK(r.remaining == "abc"); // nothing consumed
    }

    constexpr auto not_x = dsl::char_<'x'>().not_followed_by();
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
    constexpr auto p = dsl::choice({
        dsl::seq({dsl::symbol<"ab">(), dsl::symbol<"c">()}).attempt(),
        dsl::seq({dsl::symbol<"a">(), dsl::symbol<"bd">()}),
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
    constexpr auto number_base = dsl::numeric().many1().recognize();
    constexpr auto number = number_base.map([](std::string_view s) {
        return std::stod(std::string(s));
    });

    constexpr auto plus_char = dsl::char_<'+'>();
    constexpr auto plus = plus_char.map([](...) {
        return [](double a, double b) { return a + b; };
    });
    constexpr auto minus_char = dsl::char_<'-'>();
    constexpr auto minus = minus_char.map([](...) {
        return [](double a, double b) { return a - b; };
    });

    constexpr auto add = dsl::chainl1(number, plus);
    constexpr auto sub = dsl::chainl1(number, minus);
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

    constexpr auto pow_char = dsl::char_<'^'>();
    constexpr auto pow_op = pow_char.map([](...) {
        return [](double a, double b) { return std::pow(a, b); };
    });
    constexpr auto exp = dsl::chainr1(number, pow_op);
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

constexpr auto hook_element = dsl::numeric().many1().recognize().guard(pc::preds::not_empty);
constexpr auto hook_test_parser = dsl::seq({
    dsl::sep_by(hook_element, dsl::char_<','>()),
    dsl::eof(),
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
