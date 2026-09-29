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

Primitives: `symbol<"str">()`, `alpha()`, `numeric()`, `alphanumeric()`, `whitespace()`.

Combinators: `seq({...})`, `choice({...})`, `fix<Out>(f)` / `lazy<F>.fix<Out>()`, plus the
member decorators `.map<F>()`, `.guard<Pred>()`, `.label<"msg">()`, `.cut()`, `.many()`,
`.optional()`, `.discard()`, `.recognize()`.

Helpers: `dsl::into<T>`, `dsl::map<F>` / `dsl::guard<Pred>` (via `operator|`), `preds::not_empty`,
and `.visit(...)` / `pc::visit(...)` for choice results.

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
constexpr auto integer =
    pc::dsl::numeric().many().recognize() // 0~9 -> raw string
    | pc::dsl::guard<pc::preds::not_empty> // fail on empty strings
    | pc::dsl::map<[](std::string_view s) { return std::stoi(std::string(s)); }>;

// use seq to parse a sequence
constexpr auto parser = pc::dsl::seq<{
        pc::dsl::symbol<"hello">().many().discard(), // many 'hello's, we just discard them
        integer, // use a defined parser here
        pc::dsl::symbol<"world">().label<"There should be a 'world' here">(), // verbose error message
        pc::dsl::choice({ // one of 'xyz' and 'abc'
            pc::dsl::symbol<"xyz">(),
            pc::dsl::symbol<"abc">(),
        }),
    }>() | pc::dsl::map<pc::dsl::into<ParserOut>>; // fill the struct from the sequence
```

JSON parser. Full code in `json_parser_example.cpp`.

```cpp
using pc = parser_combinators;

// the main JSON parser
// as JSON is recursive, we use a Y-combinator via dsl::lazy
// and the recursive self (a detail::parser_wrapper) is passed as an argument to the lambda
constexpr auto json_parser = pc::dsl::lazy<[](auto self) {
    // parse JSON arrays
    auto array = pc::dsl::seq({
        lbracket.discard(),
        sepby<comma>(self).optional(), // use optional to allow empty arrays
        rbracket.discard(),
    }) | pc::dsl::map<[](std::optional<json_array> opt) -> json_value {
        return opt.value_or(json_array{});
    }>;

    // string and numeric literals
    auto string_lit = pc::dsl::seq({
        pc::dsl::symbol<"\"">().discard(),
        pc::dsl::alphanumeric().many().recognize(),
        pc::dsl::symbol<"\"">().discard(),
    }) | pc::dsl::map<[](std::string_view s) -> json_value {
        return std::string(s);
    }>;

    auto numeric_lit = pc::dsl::seq({
        pc::dsl::numeric().many().recognize(),
        pc::dsl::seq({
            pc::dsl::symbol<".">().discard(), // decimal point
            pc::dsl::numeric().many().recognize(),
        }).optional(), // optional fractional part
    }).recognize().guard<pc::preds::not_empty>() | pc::dsl::map<[](std::string_view s) -> json_value {
        // recognize() ignores all underlying structures and produces a single string_view
        // then guard ensures it's not empty, or it will break std::stod
        return std::stod(std::string(s));
    }>;

    auto kwpair = pc::dsl::seq({
        string_lit,
        colon.cut().discard(), // cut to avoid backtracking after colon
        self,
    }) | pc::dsl::map<[](auto&& key_val, auto&& value) {
        return std::make_pair(std::get<std::string>(key_val.data), value);
    }>;

    auto object = pc::dsl::seq({
        lbrace.discard(),
        sepby<comma>(kwpair).optional(),
        rbrace.discard(),
    }) | pc::dsl::map<[](std::optional<std::vector<std::pair<std::string, json_value>>> opt) -> json_value {
        json_object obj;
        if (opt) {
            for (auto& p : *opt) obj.emplace(std::move(p));
        }
        return obj;
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
        // choice() produces a generated tagged union (though all inner types are json_value)
        // flatten it into json_value
        return std::forward<decltype(v)>(v).visit([](auto&& inner_val) -> json_value {
            return std::forward<decltype(inner_val)>(inner_val);
        });
    }>;
}>.fix<json_value>();

int main() {
    // set up parse context with hook
    hook_impl hook;
    auto parse_context = pc::context();
    parse_context.set_hook(&hook);

    // apply hook to json_parser recursively
    // however due to the Y-combinator, it will only apply to the outermost level
    // to apply to all levels, it's best you define the parsers without Y-combinator first,
    // apply_hook to that, then use dsl::lazy to wrap it into a Y-combinator
    // but for demonstration purposes, this is sufficient
    constexpr auto trace = pc::apply_hook<json_parser>();
    auto str = R"({"key1": "value1", "key2": [true, false, null], "key3": {"nestedKey": 123.4, "anotherKey": [114, 514]}})";
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
```
