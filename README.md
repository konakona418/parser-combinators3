# parser-combinators2

PoC parser combinator library with C++26 reflection.

This is a lightweight, header-only parser combinator library designed for C++26. It leverages the
P2996 reflection proposal to provide a declarative, DSL-like syntax for building efficient parsers
at compile-time. You can define complex grammars with minimal boilerplate while maintaining a
relatively high performance and type safety.

For debugging, the library includes a non-intrusive hook system. By performing `apply_hook` on a
parser, you can automatically generate execution traces to monitor how the engine traverses your
grammar.

__Do not use this for production purposes.__

## Data structures

`std::tuple` and `std::variant` have poor runtime performance. Instead of using them for parser
results, the library uses `define_aggregate` to generate purpose-built types at compile time:

- `seq(...)` produces a **generated aggregate** whose members are exactly the non-discarded
  sub-parser values, named `m0, m1, ...`. Access it with `pc::apply`/reflection, or map it into
  your own type with `dsl::into<T>`.
- `choice(...)` produces a **generated tagged union** (`pc::variant`) with the maximum size and
  alignment of its alternatives. Use `.visit(...)` (or `pc::visit`) to inspect it.

## Backtracking model

The library follows the **Parsec consumption model**:

- A failure is *empty* (backtrackable) when no input was consumed before it.
- A failure is *committed* (fatal, never backtracked) when input was consumed, or when a `cut`
  is taken.
- `choice` only tries the next alternative on an empty failure.
- `optional`, `many` and `recognize` propagate committed failures instead of swallowing them.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
./build/json_parser_example
```

Requires GCC >= 16 (or the clang-p2996 fork) with `-std=c++26 -freflection`. The CMake target
`parser_combinators2::parser_combinators2` sets these flags for you.

## API

Primitives: `symbol<"str">()`, `alpha()`, `numeric()`, `alphanumeric()`, `whitespace()`,
`satisfy<Pred>()`, `char_<C>()`, `any_char()`, `eof()`.

Combinators: `seq({...})`, `choice({...})`, `between(open, close, inner)`,
`sep_by(p, sep)` / `sep_by1(p, sep)`, `chainl1(p, op)` / `chainr1(p, op)`,
`fix<Out>(f)` for recursion, plus the member decorators:

| method | meaning |
|---|---|
| `.map(f)` | map the value (unpacks a `seq` aggregate into `f`'s arguments) |
| `.guard(pred)` | fail if `pred(value)` is false |
| `.label<"msg">()`, `.cut()` | error message / commit (no backtracking) |
| `.attempt()` | inverse of `cut`: make failures backtrackable |
| `.many()` / `.many1()` | 0..n / 1..n |
| `.count<N>()` | exactly N |
| `.optional()` | 0..1 |
| `.look_ahead()` | run without consuming |
| `.not_followed_by()` | succeed iff this parser fails |
| `.discard()` | drop the value from a `seq` result |
| `.recognize()` | return the raw consumed `string_view` |

`map`/`guard` take the callable as a **value**; its type is captured statically, so it must be
stateless and default-constructible (a captureless lambda, or `dsl::into<T>`). `.map(pc::dsl::into<T>)`
builds `T` from the members of a `seq` result.

Helpers: predicates in `preds` (`not_empty`, `is_digit`, `is_alpha`, `is_alphanumeric`,
`is_space`, `is_upper`, `is_lower`, `one_of<...>`), and `.visit(...)` / `pc::visit(...)` for
choice results.

> **Note on `bind`.** True monadic bind (`p >>= f` where `f` returns a parser chosen from the
> *runtime* parsed value) is not expressible here: parsers are compile-time types and
> `parser_wrapper` carries a `std::meta::info` that cannot exist at runtime. Use `satisfy`/`guard`
> for value-dependent checks, or a fixed-arity runtime loop (e.g. `count` with a compile-time N).

## Examples

A simple parser which parses a sequence like `hellohello12345worldabc`:

```cpp
// the structure of the output sequence
struct ParserOut {
    int value1; // the digit 12345
    std::string_view value2; // 'world'
    pc::variant<std::string_view, std::string_view> choice_value; // 'xyz' or 'abc'
    // other values are discarded
};

// parses an integer
constexpr auto digits = pc::dsl::numeric().many().recognize(); // 0~9 -> raw string
constexpr auto integer = digits
    .guard(pc::preds::not_empty) // fail on empty strings
    .map([](std::string_view s) { return std::stoi(std::string(s)); });

// use seq to parse a sequence
constexpr auto parser = pc::dsl::seq<{
        pc::dsl::symbol<"hello">().many().discard(), // many 'hello's, we discard them
        integer, // use a defined parser here
        pc::dsl::symbol<"world">().label<"There should be a 'world' here">(), // verbose error
        pc::dsl::choice({ // one of 'xyz' and 'abc'
            pc::dsl::symbol<"xyz">(),
            pc::dsl::symbol<"abc">(),
        }),
    }>() // fill the struct from the sequence (members in declaration order)
    .map(pc::dsl::into<ParserOut>);
```

A full RFC 8259 JSON parser is in `json_parser_example.cpp`. It is a genuine JSON grammar
(whitespace is exactly space/tab/LF/CR, strings support `\uXXXX` escapes and reject raw control
characters, numbers follow `-? int frac? exp?`, and the top level requires EOF). Sketch:

```cpp
using pc = parser_combinators;
namespace dsl = pc::dsl;

// skip exactly the four JSON whitespace characters
constexpr auto ws0 = dsl::satisfy<[](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}>().many().discard();

consteval pc::parser_wrapper token(pc::parser_wrapper p) {
    return dsl::seq({ws0, p, ws0}).map(dsl::unwrap_fn{});
}

// value = object / array / string / number / true / false / null
constexpr auto json_parser = dsl::fix<json_value>([](auto self) {
    auto value = token(self);

    // number = [ "-" ] int [ frac ] [ exp ]
    auto number = token(dsl::seq({
        dsl::char_<'-'>().optional().discard(),
        dsl::choice({
            dsl::char_<'0'>(),
            dsl::seq({dsl::satisfy<[](char c) { return c >= '1' && c <= '9'; }>(),
                      dsl::numeric().many().discard()}),
        }).discard(),
        dsl::seq({dsl::char_<'.'>().discard(), dsl::numeric().many1().discard()})
            .optional().discard(),
        dsl::seq({dsl::satisfy<[](char c) { return c == 'e' || c == 'E'; }>().discard(),
                  dsl::choice({dsl::char_<'+'>(), dsl::char_<'-'>()}).optional().discard(),
                  dsl::numeric().many1().discard()})
            .optional().discard(),
    }).recognize()).map([](std::string_view raw) -> json_value {
        double val = 0;
        std::from_chars(raw.data(), raw.data() + raw.size(), val);
        return val;
    });

    // ... string/object/array, then:
    return dsl::choice({object, array, string_value, number, true_value, false_value, null_value})
        .map([](auto&& v) -> json_value {
            return std::forward<decltype(v)>(v).visit([](auto&& inner) -> json_value {
                return std::forward<decltype(inner)>(inner);
            });
        });
});

// the whole input must be a single value
constexpr auto json_text = dsl::seq({token(json_parser), dsl::eof().discard()})
    .map([](json_value v) { return v; });
```

See `json_parser_example.cpp` for the complete grammar, the escape decoder, and a tracing hook
wired up via `pc::apply_hook`.
