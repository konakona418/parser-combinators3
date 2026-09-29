// parser_combinators2.hpp
// An experimental parser combinator library using C++26 reflection features.
//
// Changelog:
//
// 2026-01-27:
//      basic parser combinators implemented:
//      symbol, alpha, numeric, optional, many, choice, combine
// 2026-01-27:
//      Added collect, fmap functionalities
// 2026-01-27:
//      Added recursive parsers via Y-combinator
//      Convenient dsl::fmap to prevent writing .template fmap<...>() everywhere
// 2026-01-28:
//      Some refactoring and cleanup
//      Added parse_context for error reporting
//      Added expect decorator for better error messages
//      Added guard decorator for predicate checks
//      Added commit decorator for committed parsing
//      Added parser hooks for tracing
//      Minor optimizations and bug fixes
// 2026-09-29:
//      Reworked backtracking to the Parsec consumption model:
//        - a failure is only fatal (non-backtrackable) when input was
//          consumed, or when a cut was taken
//        - `committed` is now propagated through every combinator
//        - choice only tries the next alternative on an empty failure
//        - optional/many/recognize no longer swallow committed failures
//      Renamed the public DSL to standard combinator vocabulary:
//        combine -> seq, fmap -> map, expect -> label,
//        commit -> cut, collect -> recognize, fmap_struct -> into
//      Renamed `details` to `detail`, exposed the hook/context API publicly.
//      Made the header self-contained and merged the optimized variant in.
//      Sequence results are now generated aggregates (via define_aggregate)
//      instead of std::tuple; choice results use a generated tagged union
//      instead of std::variant.
// 2026-09-29:
//      Added the remaining standard combinators:
//      satisfy/char_/any_char, many1, count<N>, sep_by/sep_by1, between,
//      look_ahead, not_followed_by, attempt (inverse of cut),
//      chainl1/chainr1, eof, and character predicates in `preds`.
// 2026-09-29:
//      Unified the public API: unary decorators are member methods, primitives
//      and multi-parser combinators are free functions in `dsl`; removed the
//      selective `operator|` sugar and `lazy` (recursion is now fix<Out>(f)).
//      Rewrote the JSON example to conform to RFC 8259.
// 2026-09-29:
//      map/guard now take the callable as a value (`.map(f)` / `.guard(pred)`);
//      its type is captured statically, so it must be stateless and
//      default-constructible. This also avoids a GCC mis-parse of lambda
//      template arguments placed right after a call/braced-init receiver.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace parser_combinators {
    namespace detail {
        // ---------------------------------------------------------------------
        // reflection helpers
        // ---------------------------------------------------------------------

        consteval std::string index_name(std::size_t i) {
            std::string s = "m";
            if (i == 0) {
                s += '0';
                return s;
            }
            char buf[20];
            int n = 0;
            while (i) {
                buf[n++] = static_cast<char>('0' + i % 10);
                i /= 10;
            }
            while (n) s += buf[--n];
            return s;
        }

        template <std::size_t N>
        consteval auto make_index_array() {
            std::array<std::size_t, N> arr = {};
            for (std::size_t i = 0; i < N; ++i) {
                arr[i] = i;
            }
            return arr;
        }

        template <typename T>
        struct remove_cvref { using type = std::remove_cv_t<std::remove_reference_t<T>>; };
        template <typename T>
        using remove_cvref_t = typename remove_cvref<T>::type;

        template <std::size_t N, std::size_t A>
        struct alignas(A) aligned_buffer {
            std::byte bytes[N];
        };

        template <typename T, size_t N>
        struct list_wrapper {
            T data[N];

            template <typename... Ps>
            consteval list_wrapper(Ps... ps) : data{ps...} {}

            consteval size_t size() const {
                return N;
            }
        };

        // A tuple-like aggregate generated at compile time with define_aggregate.
        // Members are named m0, m1, ... and can be accessed by index via
        // reflection (see data_member_at_index / apply_aggregate).
        template <typename... Ts>
        struct generated_tuple {
            struct type;
            consteval {
                std::vector<std::meta::info> specs;
                if constexpr (sizeof...(Ts) > 0) {
                    std::size_t i = 0;
                    template for (constexpr auto t : std::define_static_array(std::array{^^Ts...})) {
                        specs.push_back(std::meta::data_member_spec(t, {.name = index_name(i)}));
                        ++i;
                    }
                }
                std::meta::define_aggregate(^^type, specs);
            }
        };

        template <typename... Ts>
        using generated_tuple_t = typename generated_tuple<Ts...>::type;

        // A tagged union storage generated at compile time with define_aggregate.
        // Non-trivial alternatives are handled by the `variant` wrapper below
        // (the union itself only holds raw aligned storage).
        template <typename... Ts>
        struct generated_variant {
            static constexpr std::size_t size = std::max({sizeof(Ts)...});
            static constexpr std::size_t align = std::max({alignof(Ts)...});
            using buffer_type = aligned_buffer<size, align>;

            struct repr;
            consteval {
                std::meta::define_aggregate(^^repr, {
                    std::meta::data_member_spec(^^unsigned char, {.name = "tag"}),
                    std::meta::data_member_spec(^^buffer_type, {.name = "storage"}),
                });
            }
        };

        // A std::variant-like sum type with reflection-generated storage.
        template <typename... Ts>
        struct variant {
            using repr_type = typename generated_variant<Ts...>::repr;

            static_assert(sizeof...(Ts) > 0, "variant requires at least one alternative");

            repr_type data;

            constexpr variant() { data.tag = 255; }

            template <typename T>
                requires (!std::is_same_v<remove_cvref_t<T>, variant>)
            variant(T&& v) {
                using U = remove_cvref_t<T>;
                constexpr std::size_t I = index_of<U>();
                ::new (static_cast<void*>(data.storage.bytes)) U(std::forward<T>(v));
                data.tag = static_cast<unsigned char>(I);
            }

            variant(const variant&) = delete;
            variant& operator=(const variant&) = delete;

            variant(variant&& other) { move_from(other); }
            variant& operator=(variant&& other) {
                if (this != &other) {
                    destroy();
                    move_from(other);
                }
                return *this;
            }

            ~variant() { destroy(); }

            std::size_t index() const { return data.tag; }
            bool has_value() const { return data.tag != 255; }

            template <std::size_t I, typename... Args>
            void emplace(Args&&... args) {
                static_assert(I < sizeof...(Ts), "variant index out of bounds");
                destroy();
                using T = std::tuple_element_t<I, std::tuple<Ts...>>;
                ::new (static_cast<void*>(data.storage.bytes)) T(std::forward<Args>(args)...);
                data.tag = static_cast<unsigned char>(I);
            }

            template <typename T>
            T& get() {
                return get<index_of<T>()>();
            }

            template <std::size_t I>
            auto& get() {
                static_assert(I < sizeof...(Ts), "variant index out of bounds");
                assert(data.tag == I && "bad variant access");
                using T = std::tuple_element_t<I, std::tuple<Ts...>>;
                return *reinterpret_cast<T*>(data.storage.bytes);
            }

            template <typename F>
            decltype(auto) visit(F&& f) & {
                template for (constexpr auto i : std::define_static_array(make_index_array<sizeof...(Ts)>())) {
                    if (data.tag == i) {
                        using T = std::tuple_element_t<i, std::tuple<Ts...>>;
                        return std::invoke(std::forward<F>(f), *reinterpret_cast<T*>(data.storage.bytes));
                    }
                }
                __builtin_unreachable();
            }

            template <typename F>
            decltype(auto) visit(F&& f) && {
                template for (constexpr auto i : std::define_static_array(make_index_array<sizeof...(Ts)>())) {
                    if (data.tag == i) {
                        using T = std::tuple_element_t<i, std::tuple<Ts...>>;
                        return std::invoke(std::forward<F>(f), std::move(*reinterpret_cast<T*>(data.storage.bytes)));
                    }
                }
                __builtin_unreachable();
            }

        private:
            void destroy() {
                if (data.tag == 255) return;
                template for (constexpr auto i : std::define_static_array(make_index_array<sizeof...(Ts)>())) {
                    if (data.tag == i) {
                        using T = std::tuple_element_t<i, std::tuple<Ts...>>;
                        std::destroy_at(reinterpret_cast<T*>(data.storage.bytes));
                        break;
                    }
                }
                data.tag = 255;
            }

            void move_from(variant& other) {
                if (other.data.tag == 255) {
                    data.tag = 255;
                    return;
                }
                template for (constexpr auto i : std::define_static_array(make_index_array<sizeof...(Ts)>())) {
                    if (other.data.tag == i) {
                        using T = std::tuple_element_t<i, std::tuple<Ts...>>;
                        ::new (static_cast<void*>(data.storage.bytes))
                            T(std::move(*reinterpret_cast<T*>(other.data.storage.bytes)));
                        data.tag = static_cast<unsigned char>(i);
                        break;
                    }
                }
            }

            template <typename T>
            static consteval std::size_t index_of() {
                std::size_t i = 0;
                template for (constexpr auto t : std::define_static_array(std::array{^^Ts...})) {
                    if (t == ^^T) return i;
                    ++i;
                }
                return i;
            }
        };

        // Legacy name kept for the benchmark's JSON value container.
        template <typename... Ts>
        using static_variant = variant<Ts...>;

        // Apply a callable to every member of a generated aggregate.
        template <typename Agg>
        consteval std::size_t aggregate_member_count() {
            return std::meta::nonstatic_data_members_of(
                ^^remove_cvref_t<Agg>, std::meta::access_context::current()).size();
        }

        template <std::size_t... Is, typename F, typename Agg>
        auto apply_aggregate_impl(F&& f, Agg&& agg, std::index_sequence<Is...>) {
            constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(
                ^^remove_cvref_t<Agg>, std::meta::access_context::current()));
            return std::invoke(std::forward<F>(f), std::forward<Agg>(agg).[:members[Is]:]...);
        }

        template <typename F, typename Agg>
        auto apply_aggregate(F&& f, Agg&& agg) {
            constexpr auto n = aggregate_member_count<Agg>();
            return apply_aggregate_impl(std::forward<F>(f), std::forward<Agg>(agg),
                                        std::make_index_sequence<n>{});
        }

        consteval std::meta::info data_member_at_index(std::meta::info struct_info, std::size_t index) {
            auto ctx = std::meta::access_context::unchecked();
            return std::meta::nonstatic_data_members_of(struct_info, ctx)[index];
        }

        template <std::size_t N>
        struct string_literal {
            char value[N];

            constexpr string_literal() : value{} {}

            constexpr string_literal(const char (&str)[N]) {
                std::copy_n(str, N, value);
            }

            constexpr std::string_view str() const {
                return std::string_view(value, N - 1);
            }
        };

        template <size_t SVSize>
        consteval auto string_view_to_literal(const char* sv_data) {
            string_literal<SVSize + 1> lit{};
            std::copy_n(sv_data, SVSize, lit.value);
            lit.value[SVSize] = '\0';
            return lit;
        }

        // ---------------------------------------------------------------------
        // parse result / context
        // ---------------------------------------------------------------------

        template <typename OutType = std::string_view>
        struct parse_result {
            using value_type = OutType;

            bool success;
            bool committed;
            OutType parsed;
            std::string_view remaining;
        };

        template <typename OutType>
        constexpr auto parse_success(OutType parsed, std::string_view remaining, bool committed = false) {
            return parse_result<OutType>{true, committed, std::move(parsed), remaining};
        }

        template <typename OutType>
        constexpr auto parse_failure(std::string_view input, bool committed = false) {
            return parse_result<OutType>{false, committed, OutType{}, input};
        }

        struct parser_hook {
            virtual ~parser_hook() = default;
            virtual void on_success(std::string_view, std::string_view) {}
            virtual void on_failure(std::string_view, std::string_view) {}
            virtual void on_enter(std::string_view, std::string_view) {}
            virtual void on_exit(std::string_view, std::string_view) {}
        };

        struct parse_error {
            std::string message;
            uintptr_t ptr_pos = 0;

            size_t diff(const char* base_ptr) const {
                return ptr_pos - reinterpret_cast<uintptr_t>(base_ptr);
            }
        };

        struct parse_context {
            parse_error error;
            parser_hook* hook = nullptr;

            void add_error(std::string_view message, const char* ptr_position) {
                if (error.ptr_pos <= reinterpret_cast<uintptr_t>(ptr_position)) {
                    // only record the farthest error
                    error.message = message;
                    error.ptr_pos = reinterpret_cast<uintptr_t>(ptr_position);
                }
            }

            void reset() {
                error = parse_error{};
            }

            void set_hook(parser_hook* new_hook) {
                hook = new_hook;
            }

            void hook_on_success(std::string_view name, std::string_view remaining) {
                if (hook) hook->on_success(name, remaining);
            }

            void hook_on_failure(std::string_view name, std::string_view remaining) {
                if (hook) hook->on_failure(name, remaining);
            }

            void hook_on_enter(std::string_view name, std::string_view input) {
                if (hook) hook->on_enter(name, input);
            }

            void hook_on_exit(std::string_view name, std::string_view input) {
                if (hook) hook->on_exit(name, input);
            }
        };

        template <typename T>
        concept parser_trait = requires(std::string_view input, parse_context& ctx) {
            T::parse(input, ctx);
        };

        // Does the parser produce a generated aggregate (a `seq` result)?
        template <typename P>
        consteval bool parser_yields_aggregate() {
            if constexpr (requires { P::yields_aggregate; }) {
                return P::yields_aggregate;
            } else {
                return false;
            }
        }

        inline constexpr bool consumed_since(std::string_view before, std::string_view after) {
            return before.size() != after.size();
        }

        // ---------------------------------------------------------------------
        // attributes / decorators
        // ---------------------------------------------------------------------

        enum class parser_attribute : int {
            none = 0,
            discard = 1,
        };

        inline constexpr parser_attribute operator|(parser_attribute a, parser_attribute b) {
            return static_cast<parser_attribute>(static_cast<int>(a) | static_cast<int>(b));
        }

        inline constexpr parser_attribute& operator|=(parser_attribute& a, parser_attribute b) {
            a = a | b;
            return a;
        }

        inline constexpr bool has_attribute(parser_attribute attrs, parser_attribute check) {
            return (static_cast<int>(attrs) & static_cast<int>(check)) != 0;
        }

        template <parser_trait BaseParser, parser_attribute Attr>
        struct parser_decorator {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                return BaseParser::parse(input, ctx);
            }
        };

        consteval auto split_parser_decorator(std::meta::info decorated_parser)
            -> std::pair<std::meta::info, parser_attribute> {
            auto args = std::meta::template_arguments_of(decorated_parser);
            assert(args.size() == 2 && "parser_decorator must have exactly 2 template arguments");

            auto base_parser = args[0];
            auto attr_constant = args[1];
            auto attr_value = std::meta::extract<parser_attribute>(attr_constant);

            return {base_parser, attr_value};
        }

        // ---------------------------------------------------------------------
        // primitives
        // ---------------------------------------------------------------------

        template <string_literal Str>
        struct symbol_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context& ctx) {
                constexpr auto s = Str.str();
                if (!input.starts_with(s)) {
                    ctx.add_error("Expected symbol: " + std::string(s), input.data());
                    return parse_failure<std::string_view>(input);
                }
                return parse_success<std::string_view>(input.substr(0, s.size()), input.substr(s.size()));
            }
        };

        struct alpha_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (input.empty() || !std::isalpha(static_cast<unsigned char>(input[0]))) {
                    ctx.add_error("Expected alphabetic character", input.data());
                    return parse_failure<std::string_view>(input);
                }
                return parse_success<std::string_view>(input.substr(0, 1), input.substr(1));
            }
        };

        struct numeric_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (input.empty() || !std::isdigit(static_cast<unsigned char>(input[0]))) {
                    ctx.add_error("Expected numeric character", input.data());
                    return parse_failure<std::string_view>(input);
                }
                return parse_success<std::string_view>(input.substr(0, 1), input.substr(1));
            }
        };

        struct alphanumeric_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (input.empty() || !std::isalnum(static_cast<unsigned char>(input[0]))) {
                    ctx.add_error("Expected alphanumeric character", input.data());
                    return parse_failure<std::string_view>(input);
                }
                return parse_success<std::string_view>(input.substr(0, 1), input.substr(1));
            }
        };

        struct whitespace_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context&) {
                if (input.empty() || !std::isspace(static_cast<unsigned char>(input[0]))) {
                    return parse_failure<std::string_view>(input);
                }
                return parse_success<std::string_view>(input.substr(0, 1), input.substr(1));
            }
        };

        // the fundamental primitive: consume one character satisfying Predicate
        template <auto Predicate>
        struct satisfy_parser {
            using value_type = char;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (input.empty() || !Predicate(input[0])) {
                    ctx.add_error("satisfy: predicate rejected input", input.data());
                    return parse_failure<char>(input);
                }
                return parse_success<char>(input[0], input.substr(1));
            }
        };

        template <char C>
        struct char_parser {
            using value_type = char;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (input.empty() || input[0] != C) {
                    ctx.add_error(std::string("Expected character: ") + C, input.data());
                    return parse_failure<char>(input);
                }
                return parse_success<char>(input[0], input.substr(1));
            }
        };

        struct any_char_parser {
            using value_type = char;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (input.empty()) {
                    ctx.add_error("any_char: unexpected end of input", input.data());
                    return parse_failure<char>(input);
                }
                return parse_success<char>(input[0], input.substr(1));
            }
        };

        struct eof_parser {
            using value_type = std::monostate;
            static auto parse(std::string_view input, parse_context& ctx) {
                if (!input.empty()) {
                    ctx.add_error("eof: expected end of input", input.data());
                    return parse_failure<std::monostate>(input);
                }
                return parse_success<std::monostate>(std::monostate{}, input);
            }
        };

        // ---------------------------------------------------------------------
        // combinators
        // ---------------------------------------------------------------------

        template <parser_trait BaseParser>
        struct recognize_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context& ctx) {
                auto r = BaseParser::parse(input, ctx);
                if (!r.success) {
                    return parse_failure<std::string_view>(input, r.committed);
                }
                return parse_success<std::string_view>(
                    input.substr(0, input.size() - r.remaining.size()), r.remaining, r.committed);
            }
        };

        template <parser_trait BaseParser>
        struct optional_parser {
            using value_type = std::optional<typename BaseParser::value_type>;
            static auto parse(std::string_view input, parse_context& ctx) {
                using ElementType = typename BaseParser::value_type;
                using Out = std::optional<ElementType>;
                auto r = BaseParser::parse(input, ctx);
                if (r.success) {
                    return parse_result<Out>{true, r.committed, Out(std::move(r.parsed)), r.remaining};
                }
                if (r.committed) {
                    return parse_result<Out>{false, true, Out{}, input};
                }
                return parse_result<Out>{true, false, Out{std::nullopt}, input};
            }
        };

        template <parser_trait BaseParser, string_literal Label>
        struct label_parser {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                auto r = BaseParser::parse(input, ctx);
                if (!r.success) {
                    ctx.add_error(Label.str(), input.data());
                    return parse_failure<typename BaseParser::value_type>(input, r.committed);
                }
                return r;
            }
        };

        template <parser_trait BaseParser, typename Predicate>
        struct guard_parser {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                auto r = BaseParser::parse(input, ctx);
                if (!r.success) {
                    return parse_failure<typename BaseParser::value_type>(input, r.committed);
                }
                if (!Predicate{}(r.parsed)) {
                    bool committed = r.committed || consumed_since(input, r.remaining);
                    ctx.add_error("Guard condition failed", input.data());
                    return parse_failure<typename BaseParser::value_type>(input, committed);
                }
                return r;
            }
        };

        template <parser_trait BaseParser>
        struct cut_parser {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                auto r = BaseParser::parse(input, ctx);
                r.committed = true;
                return r;
            }
        };

        template <string_literal Name, parser_trait BaseParser>
        struct hooked_parser {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                ctx.hook_on_enter(Name.str(), input);
                auto result = BaseParser::parse(input, ctx);
                if (result.success) {
                    ctx.hook_on_success(Name.str(), result.remaining);
                } else {
                    ctx.hook_on_failure(Name.str(), result.remaining);
                }
                ctx.hook_on_exit(Name.str(), result.remaining);
                return result;
            }
        };

        template <parser_trait BaseParser>
        struct many_parser {
            using value_type = std::vector<typename BaseParser::value_type>;
            static auto parse(std::string_view input, parse_context& ctx) {
                using ElementType = typename BaseParser::value_type;
                std::string_view remaining = input;
                std::vector<ElementType> results;
                bool cut = false;
                while (true) {
                    auto result = BaseParser::parse(remaining, ctx);
                    if (result.success) {
                        if (!consumed_since(remaining, result.remaining)) {
                            ctx.add_error("many: element parser succeeded without consuming input",
                                          remaining.data());
                            return parse_failure<value_type>(input, true);
                        }
                        if (result.committed) cut = true;
                        results.push_back(std::move(result.parsed));
                        remaining = result.remaining;
                        continue;
                    }
                    if (result.committed) {
                        return parse_failure<value_type>(input, true);
                    }
                    break;
                }
                return parse_success<value_type>(std::move(results), remaining, cut);
            }
        };

        template <parser_trait BaseParser>
        struct many1_parser {
            using value_type = std::vector<typename BaseParser::value_type>;
            static auto parse(std::string_view input, parse_context& ctx) {
                using ElementType = typename BaseParser::value_type;

                auto first = BaseParser::parse(input, ctx);
                if (!first.success) {
                    return parse_failure<value_type>(input, first.committed);
                }

                std::vector<ElementType> results;
                results.push_back(std::move(first.parsed));
                std::string_view remaining = first.remaining;
                bool cut = first.committed;

                while (true) {
                    auto result = BaseParser::parse(remaining, ctx);
                    if (result.success) {
                        if (!consumed_since(remaining, result.remaining)) {
                            ctx.add_error("many1: element parser succeeded without consuming input",
                                          remaining.data());
                            return parse_failure<value_type>(input, true);
                        }
                        if (result.committed) cut = true;
                        results.push_back(std::move(result.parsed));
                        remaining = result.remaining;
                        continue;
                    }
                    if (result.committed) return parse_failure<value_type>(input, true);
                    break;
                }

                return parse_success<value_type>(std::move(results), remaining, cut);
            }
        };

        template <parser_trait BaseParser, std::size_t N>
        struct count_parser {
            using value_type = std::vector<typename BaseParser::value_type>;
            static auto parse(std::string_view input, parse_context& ctx) {
                using ElementType = typename BaseParser::value_type;

                std::vector<ElementType> results;
                results.reserve(N);
                std::string_view remaining = input;
                bool cut = false;

                template for (constexpr auto i : std::define_static_array(make_index_array<N>())) {
                    (void)i;
                    auto result = BaseParser::parse(remaining, ctx);
                    if (result.committed) cut = true;
                    if (!result.success) {
                        return parse_failure<value_type>(
                            input, cut || consumed_since(input, remaining) || result.committed);
                    }
                    results.push_back(std::move(result.parsed));
                    remaining = result.remaining;
                }

                return parse_success<value_type>(std::move(results), remaining, cut);
            }
        };

        // AllowEmpty == true  -> sep_by  (zero or more)
        // AllowEmpty == false -> sep_by1 (one or more)
        template <parser_trait BaseParser, parser_trait SepParser, bool AllowEmpty>
        struct sep_by_parser {
            using value_type = std::vector<typename BaseParser::value_type>;
            static auto parse(std::string_view input, parse_context& ctx) {
                using ElementType = typename BaseParser::value_type;

                std::vector<ElementType> results;
                std::string_view remaining = input;
                bool cut = false;

                auto first = BaseParser::parse(remaining, ctx);
                if (first.success) {
                    cut = first.committed;
                    results.push_back(std::move(first.parsed));
                    remaining = first.remaining;
                } else {
                    if (first.committed) return parse_failure<value_type>(input, true);
                    if constexpr (!AllowEmpty) return parse_failure<value_type>(input, first.committed);
                    return parse_success<value_type>(std::move(results), remaining, false);
                }

                while (true) {
                    auto sep = SepParser::parse(remaining, ctx);
                    if (!sep.success) {
                        if (sep.committed) return parse_failure<value_type>(input, true);
                        break;
                    }

                    auto elem = BaseParser::parse(sep.remaining, ctx);
                    if (!elem.success) {
                        bool committed = cut || sep.committed || elem.committed ||
                                         consumed_since(remaining, sep.remaining) ||
                                         consumed_since(remaining, elem.remaining);
                        return parse_failure<value_type>(input, committed);
                    }
                    if (!consumed_since(remaining, elem.remaining)) {
                        ctx.add_error("sep_by: separator and element consumed no input",
                                      remaining.data());
                        return parse_failure<value_type>(input, true);
                    }

                    cut = cut || sep.committed || elem.committed;
                    results.push_back(std::move(elem.parsed));
                    remaining = elem.remaining;
                }

                return parse_success<value_type>(std::move(results), remaining, cut);
            }
        };

        // run BaseParser but do not consume input on success
        template <parser_trait BaseParser>
        struct look_ahead_parser {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                auto result = BaseParser::parse(input, ctx);
                if (!result.success) {
                    return parse_failure<typename BaseParser::value_type>(input, result.committed);
                }
                return parse_result<value_type>{true, result.committed, std::move(result.parsed), input};
            }
        };

        // succeed (without consuming) iff BaseParser fails
        template <parser_trait BaseParser>
        struct not_parser {
            using value_type = std::monostate;
            static auto parse(std::string_view input, parse_context& ctx) {
                auto result = BaseParser::parse(input, ctx);
                if (result.success) {
                    ctx.add_error("not_followed_by: unexpected match", input.data());
                    return parse_failure<std::monostate>(input);
                }
                return parse_success<std::monostate>(std::monostate{}, input);
            }
        };

        // turn a committed failure into an empty (backtrackable) one
        template <parser_trait BaseParser>
        struct attempt_parser {
            using value_type = typename BaseParser::value_type;
            static constexpr bool yields_aggregate = parser_yields_aggregate<BaseParser>();
            static auto parse(std::string_view input, parse_context& ctx) {
                auto result = BaseParser::parse(input, ctx);
                if (!result.success) result.committed = false;
                return result;
            }
        };

        // left-associative: p (op p)* folded left
        template <parser_trait Parser, parser_trait OpParser>
        struct chainl1_parser {
            using value_type = typename Parser::value_type;
            static auto parse(std::string_view input, parse_context& ctx) {
                using T = typename Parser::value_type;

                auto first = Parser::parse(input, ctx);
                if (!first.success) return parse_failure<T>(input, first.committed);

                T acc = std::move(first.parsed);
                std::string_view remaining = first.remaining;
                bool cut = first.committed;

                while (true) {
                    auto op = OpParser::parse(remaining, ctx);
                    if (!op.success) {
                        if (op.committed) return parse_failure<T>(input, true);
                        break;
                    }
                    auto rhs = Parser::parse(op.remaining, ctx);
                    if (!rhs.success) {
                        bool committed = cut || op.committed || rhs.committed ||
                                         consumed_since(remaining, op.remaining) ||
                                         consumed_since(remaining, rhs.remaining);
                        return parse_failure<T>(input, committed);
                    }
                    cut = cut || op.committed || rhs.committed;
                    acc = std::invoke(op.parsed, std::move(acc), std::move(rhs.parsed));
                    remaining = rhs.remaining;
                }

                return parse_success<T>(std::move(acc), remaining, cut);
            }
        };

        // right-associative: p (op p)* folded right
        template <parser_trait Parser, parser_trait OpParser>
        struct chainr1_parser {
            using value_type = typename Parser::value_type;
            static auto parse(std::string_view input, parse_context& ctx) {
                using T = typename Parser::value_type;
                using F = typename OpParser::value_type;

                auto first = Parser::parse(input, ctx);
                if (!first.success) return parse_failure<T>(input, first.committed);

                std::vector<T> values;
                std::vector<F> ops;
                values.push_back(std::move(first.parsed));
                std::string_view remaining = first.remaining;
                bool cut = first.committed;

                while (true) {
                    auto op = OpParser::parse(remaining, ctx);
                    if (!op.success) {
                        if (op.committed) return parse_failure<T>(input, true);
                        break;
                    }
                    auto rhs = Parser::parse(op.remaining, ctx);
                    if (!rhs.success) {
                        bool committed = cut || op.committed || rhs.committed ||
                                         consumed_since(remaining, op.remaining) ||
                                         consumed_since(remaining, rhs.remaining);
                        return parse_failure<T>(input, committed);
                    }
                    cut = cut || op.committed || rhs.committed;
                    ops.push_back(std::move(op.parsed));
                    values.push_back(std::move(rhs.parsed));
                    remaining = rhs.remaining;
                }

                T acc = std::move(values.back());
                for (std::size_t i = ops.size(); i-- > 0; ) {
                    acc = std::invoke(ops[i], std::move(values[i]), std::move(acc));
                }

                return parse_success<T>(std::move(acc), remaining, cut);
            }
        };

        template <string_literal Reason>
        struct invalid_parser {
            using value_type = std::string_view;
            static auto parse(std::string_view input, parse_context& ctx) {
                assert(false && "Invalid parser configuration");
                ctx.add_error("Invalid parser configuration: " + std::string(Reason.str()), input.data());
                return parse_failure<std::string_view>(input);
            }
        };

        template <parser_trait... SubParsers>
        consteval auto make_member_index_map() {
            constexpr auto attrs = std::array<std::meta::info, sizeof...(SubParsers)>{^^SubParsers...};
            std::array<int, sizeof...(SubParsers)> mapping{};
            int current_member = 0;
            for (size_t i = 0; i < attrs.size(); ++i) {
                auto [_, attr] = split_parser_decorator(attrs[i]);
                if (has_attribute(attr, parser_attribute::discard)) {
                    mapping[i] = -1;
                } else {
                    mapping[i] = current_member++;
                }
            }
            return mapping;
        }

        template <parser_trait... SubParsers>
        consteval auto construct_return_tuple_type() {
            constexpr auto mapping = make_member_index_map<SubParsers...>();
            using SubParsersTuple = std::tuple<SubParsers...>;

            std::vector<std::meta::info> types;
            template for (constexpr auto i : std::define_static_array(make_index_array<sizeof...(SubParsers)>())) {
                if constexpr (mapping[i] != -1) {
                    using SubParser = std::tuple_element_t<i, SubParsersTuple>;
                    using ParsedType = typename SubParser::value_type;
                    types.push_back(^^ParsedType);
                }
            }

            return std::meta::substitute(^^generated_tuple_t, std::define_static_array(types));
        }

        template <parser_trait BaseParser, typename Fmap>
        struct map_parser {
            static constexpr bool is_aggregate = parser_yields_aggregate<BaseParser>();

            static auto apply(auto&& val) {
                if constexpr (is_aggregate) {
                    return apply_aggregate(Fmap{}, std::forward<decltype(val)>(val));
                } else {
                    return std::invoke(Fmap{}, std::forward<decltype(val)>(val));
                }
            }

            using value_type = decltype(apply(std::declval<typename BaseParser::value_type>()));

            static auto parse(std::string_view input, parse_context& ctx) {
                using mapped_type = value_type;
                auto result = BaseParser::parse(input, ctx);
                if (!result.success) {
                    return parse_failure<mapped_type>(input, result.committed);
                }
                return parse_success<mapped_type>(apply(std::move(result.parsed)), result.remaining,
                                                  result.committed);
            }
        };

        template <parser_trait... SubParsers>
        struct choice_parser {
            using value_type = variant<typename SubParsers::value_type...>;
            static auto parse(std::string_view input, parse_context& ctx) {
                using ParserOutputs = value_type;
                using SubParserTuple = std::tuple<SubParsers...>;

                const auto original_input = input;
                template for (constexpr int i : std::define_static_array(make_index_array<sizeof...(SubParsers)>())) {
                    using SubParser = std::tuple_element_t<i, SubParserTuple>;

                    auto [succ, committed, parsed, remaining] = SubParser::parse(original_input, ctx);
                    if (succ) {
                        ParserOutputs parsed_value;
                        parsed_value.template emplace<i>(std::move(parsed));
                        return parse_result<ParserOutputs>{true, committed, std::move(parsed_value), remaining};
                    }

                    if (committed) {
                        // committed failure, stop trying
                        return parse_result<ParserOutputs>{false, true, ParserOutputs{}, original_input};
                    }
                }

                return parse_result<ParserOutputs>{false, false, ParserOutputs{}, input};
            }
        };

        template <parser_trait... SubParsers>
        struct sequential_parser {
            using value_type = [:construct_return_tuple_type<SubParsers...>():];
            static constexpr bool yields_aggregate = true;

            static auto parse(std::string_view input, parse_context& ctx) {
                using ReturnTupleType = value_type;
                using SubParsersTuple = std::tuple<SubParsers...>;

                ReturnTupleType result_tuple{};
                std::string_view parse_remaining = input;

                bool cut = false;
                constexpr auto member_index_map = make_member_index_map<SubParsers...>();
                template for (constexpr int i : std::define_static_array(make_index_array<sizeof...(SubParsers)>())) {
                    using SubParser = std::tuple_element_t<i, SubParsersTuple>;
                    auto r = SubParser::parse(parse_remaining, ctx);

                    if (r.committed) cut = true;

                    if (!r.success) {
                        bool committed = cut || consumed_since(input, parse_remaining);
                        return parse_result<ReturnTupleType>{false, committed, ReturnTupleType{}, input};
                    }

                    constexpr int target_idx = member_index_map[i];
                    if constexpr (target_idx != -1) {
                        result_tuple.[:data_member_at_index(^^ReturnTupleType, target_idx):] =
                            std::move(r.parsed);
                    }

                    parse_remaining = r.remaining;
                }

                return parse_result<ReturnTupleType>{true, cut, std::move(result_tuple), parse_remaining};
            }
        };

        // ---------------------------------------------------------------------
        // DSL value: a compile-time parser handle
        // ---------------------------------------------------------------------

        struct parser_wrapper {
            std::meta::info basic_parser;
            bool should_discard = false;
            bool is_optional = false;
            bool is_many = false;

            // discard the result of this parser
            consteval auto discard() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.should_discard = true;
                return pw;
            }

            // 0..1
            consteval auto optional() const -> parser_wrapper {
                parser_wrapper pw = *this;
                if (is_many) {
                    pw.basic_parser = ^^invalid_parser<"Cannot apply 'optional' to a many parser">;
                    return pw;
                }
                pw.is_optional = true;
                pw.basic_parser = std::meta::substitute(^^optional_parser, {pw.basic_parser});
                return pw;
            }

            template <string_literal Label>
            consteval auto label() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(
                    ^^label_parser, {pw.basic_parser, std::meta::reflect_constant(Label)});
                return pw;
            }

            template <typename Predicate>
            consteval auto guard(Predicate) const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(
                    ^^guard_parser, {pw.basic_parser, ^^Predicate});
                return pw;
            }

            // force a cut: any failure after this point is fatal
            consteval auto cut() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(^^cut_parser, {pw.basic_parser});
                return pw;
            }

            // 0..n
            consteval auto many() const -> parser_wrapper {
                parser_wrapper pw = *this;
                if (is_optional) {
                    pw.basic_parser = ^^invalid_parser<"Cannot apply 'many' to an optional parser">;
                    return pw;
                }
                pw.is_many = true;
                pw.basic_parser = std::meta::substitute(^^many_parser, {pw.basic_parser});
                return pw;
            }

            // 1..n
            consteval auto many1() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(^^many1_parser, {pw.basic_parser});
                return pw;
            }

            // exactly N
            template <std::size_t N>
            consteval auto count() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(
                    ^^count_parser, {pw.basic_parser, std::meta::reflect_constant(N)});
                return pw;
            }

            // succeed without consuming input
            consteval auto look_ahead() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(^^look_ahead_parser, {pw.basic_parser});
                return pw;
            }

            // succeed without consuming input iff this parser fails
            consteval auto not_followed_by() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(^^not_parser, {pw.basic_parser});
                return pw;
            }

            // make failures backtrackable (inverse of cut)
            consteval auto attempt() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(^^attempt_parser, {pw.basic_parser});
                return pw;
            }

            // recognize the raw input consumed by this parser (returns string_view)
            consteval auto recognize() const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.is_many = false;
                pw.is_optional = false;
                pw.basic_parser = std::meta::substitute(^^recognize_parser, {pw.basic_parser});
                return pw;
            }

            // map the parsed value (or, for a `seq`, unpack the generated aggregate)
            template <typename Fmap>
            consteval auto map(Fmap) const -> parser_wrapper {
                parser_wrapper pw = *this;
                pw.basic_parser = std::meta::substitute(
                    ^^map_parser, {pw.basic_parser, ^^Fmap});
                return pw;
            }

            consteval auto parser() const {
                return basic_parser;
            }
        };

        // ---------------------------------------------------------------------
        // recursion (Y-combinator)
        // ---------------------------------------------------------------------

        template <typename OutType, auto F>
        struct y_combinator {
            using value_type = OutType;
            static auto parse(std::string_view input, parse_context& ctx) -> parse_result<OutType> {
                struct recurse_helper {
                    using value_type [[maybe_unused]] = OutType;
                    static auto parse(std::string_view input, parse_context& ctx) -> parse_result<OutType> {
                        return y_combinator<OutType, F>::parse(input, ctx);
                    }
                };
                static_assert(std::is_same_v<typename recurse_helper::value_type, OutType>);

                static constexpr auto cached_parser = [] {
                    constexpr auto wrapper = F(parser_wrapper{^^recurse_helper});
                    return wrapper.basic_parser;
                }();

                using parser = [:cached_parser:];
                return parser::parse(input, ctx);
            }
        };

        // ---------------------------------------------------------------------
        // pretty names for hooks
        // ---------------------------------------------------------------------

        template <std::meta::info Parser>
        consteval auto pretty_name() {
            if constexpr (std::meta::has_template_arguments(Parser) &&
                          std::meta::template_of(Parser) == ^^symbol_parser) {
                constexpr auto args = std::define_static_array(std::meta::template_arguments_of(Parser));
                constexpr auto symbol_name_constant = args[0];
                constexpr auto symbol_name_size = std::meta::extract<size_t>(
                    std::meta::template_arguments_of(std::meta::type_of(symbol_name_constant))[0]);
                constexpr auto symbol_name =
                    std::meta::extract<string_literal<symbol_name_size>>(symbol_name_constant);
                constexpr auto symbol_str = "symbol_parser(\"" + std::string(symbol_name.str()) + "\")";
                return string_view_to_literal<symbol_str.size()>(symbol_str.data());
            } else if constexpr (std::meta::has_template_arguments(Parser) &&
                                 std::meta::template_of(Parser) == ^^satisfy_parser) {
                return string_literal("satisfy_parser");
            } else if constexpr (std::meta::has_template_arguments(Parser) &&
                                 std::meta::template_of(Parser) == ^^char_parser) {
                return string_literal("char_parser");
            } else if constexpr (Parser == ^^any_char_parser) return string_literal("any_char_parser");
            else if constexpr (Parser == ^^eof_parser) return string_literal("eof_parser");
            else if constexpr (Parser == ^^alpha_parser) return string_literal("alpha_parser");
            else if constexpr (Parser == ^^numeric_parser) return string_literal("numeric_parser");
            else if constexpr (Parser == ^^alphanumeric_parser) return string_literal("alphanumeric_parser");
            else if constexpr (Parser == ^^whitespace_parser) return string_literal("whitespace_parser");
            else return string_literal("unknown_parser");
        }
    } // namespace detail

    // -------------------------------------------------------------------------
    // public surface
    // -------------------------------------------------------------------------

    using detail::parser_hook;
    using detail::parse_context;
    using detail::parse_error;
    using detail::parser_wrapper;
    using detail::variant;

    template <typename V, typename F>
    decltype(auto) visit(V&& v, F&& f) {
        return std::forward<V>(v).visit(std::forward<F>(f));
    }

    // Apply a callable to every member of a `seq` result (generated aggregate).
    template <typename F, typename Agg>
    auto apply(F&& f, Agg&& agg) {
        return detail::apply_aggregate(std::forward<F>(f), std::forward<Agg>(agg));
    }

    namespace dsl {
        template <detail::string_literal Str>
        consteval auto symbol() {
            auto parser_type = std::meta::substitute(
                ^^detail::symbol_parser, {std::meta::reflect_constant(Str)});
            return detail::parser_wrapper{parser_type};
        }

        consteval auto alpha() {
            return detail::parser_wrapper{^^detail::alpha_parser};
        }

        consteval auto numeric() {
            return detail::parser_wrapper{^^detail::numeric_parser};
        }

        consteval auto alphanumeric() {
            return detail::parser_wrapper{^^detail::alphanumeric_parser};
        }

        consteval auto whitespace() {
            return detail::parser_wrapper{^^detail::whitespace_parser};
        }

        template <auto Predicate>
        consteval auto satisfy() {
            auto parser_type = std::meta::substitute(
                ^^detail::satisfy_parser, {std::meta::reflect_constant(Predicate)});
            return detail::parser_wrapper{parser_type};
        }

        template <char C>
        consteval auto char_() {
            auto parser_type = std::meta::substitute(
                ^^detail::char_parser, {std::meta::reflect_constant(C)});
            return detail::parser_wrapper{parser_type};
        }

        consteval auto any_char() {
            return detail::parser_wrapper{^^detail::any_char_parser};
        }

        consteval auto eof() {
            return detail::parser_wrapper{^^detail::eof_parser};
        }

        consteval auto seq(std::span<const detail::parser_wrapper> parsers) {
            std::vector<std::meta::info> parser_types;
            for (const auto& p : parsers) {
                detail::parser_attribute attr = detail::parser_attribute::none;
                if (p.should_discard) attr |= detail::parser_attribute::discard;

                parser_types.push_back(std::meta::substitute(
                    ^^detail::parser_decorator, {p.basic_parser, std::meta::reflect_constant(attr)}));
            }

            auto combined_parser = std::meta::substitute(^^detail::sequential_parser, parser_types);
            return detail::parser_wrapper{combined_parser};
        }

        consteval auto seq(std::initializer_list<detail::parser_wrapper> parsers) {
            return seq(std::span{parsers.begin(), parsers.size()});
        }

        template <size_t N>
        struct parser_list : detail::list_wrapper<detail::parser_wrapper, N> {
            using detail::list_wrapper<detail::parser_wrapper, N>::list_wrapper;
        };

        template <typename... Ps>
        parser_list(Ps...) -> parser_list<sizeof...(Ps)>;

        // if you are using y-combinators, this is not going to work
        template <parser_list Initializer>
        consteval auto seq() {
            std::vector<detail::parser_wrapper> parsers;
            for (size_t i = 0; i < Initializer.size(); ++i) {
                parsers.push_back(Initializer.data[i]);
            }
            return seq(std::span{parsers.data(), parsers.size()});
        }

        consteval auto choice(std::span<const detail::parser_wrapper> parsers) {
            std::vector<std::meta::info> parser_types;
            for (const auto& p : parsers) {
                detail::parser_attribute attr = detail::parser_attribute::none;
                if (p.should_discard) attr |= detail::parser_attribute::discard;

                parser_types.push_back(std::meta::substitute(
                    ^^detail::parser_decorator, {p.basic_parser, std::meta::reflect_constant(attr)}));
            }

            auto choice_parser = std::meta::substitute(^^detail::choice_parser, parser_types);
            return detail::parser_wrapper{choice_parser};
        }

        consteval auto choice(std::initializer_list<detail::parser_wrapper> parsers) {
            return choice(std::span{parsers.begin(), parsers.size()});
        }

        // if you are using y-combinators, this is not going to work
        template <parser_list Initializer>
        consteval auto choice() {
            std::vector<detail::parser_wrapper> parsers;
            for (size_t i = 0; i < Initializer.size(); ++i) {
                parsers.push_back(Initializer.data[i]);
            }
            return choice(std::span{parsers.data(), parsers.size()});
        }

        // keep only the inner parser's value (drops the delimiters)
        struct unwrap_fn {
            template <typename T>
            auto operator()(T&& value) const {
                return std::forward<T>(value);
            }
        };

        // open inner close, yielding inner's value
        consteval auto between(detail::parser_wrapper open, detail::parser_wrapper close,
                               detail::parser_wrapper inner) {
            return seq({open.discard(), inner, close.discard()}).map(unwrap_fn{});
        }

        consteval auto sep_by(detail::parser_wrapper element, detail::parser_wrapper sep) {
            auto parser_type = std::meta::substitute(
                ^^detail::sep_by_parser,
                {element.basic_parser, sep.basic_parser, std::meta::reflect_constant(true)});
            return detail::parser_wrapper{parser_type};
        }

        consteval auto sep_by1(detail::parser_wrapper element, detail::parser_wrapper sep) {
            auto parser_type = std::meta::substitute(
                ^^detail::sep_by_parser,
                {element.basic_parser, sep.basic_parser, std::meta::reflect_constant(false)});
            return detail::parser_wrapper{parser_type};
        }

        consteval auto chainl1(detail::parser_wrapper element, detail::parser_wrapper op) {
            auto parser_type = std::meta::substitute(
                ^^detail::chainl1_parser, {element.basic_parser, op.basic_parser});
            return detail::parser_wrapper{parser_type};
        }

        consteval auto chainr1(detail::parser_wrapper element, detail::parser_wrapper op) {
            auto parser_type = std::meta::substitute(
                ^^detail::chainr1_parser, {element.basic_parser, op.basic_parser});
            return detail::parser_wrapper{parser_type};
        }

        template <typename OutType, typename F>
        consteval auto fix(F&& fn) {
            auto parser_type = std::meta::substitute(
                ^^detail::y_combinator, {^^OutType, std::meta::reflect_constant(fn)});
            return detail::parser_wrapper{parser_type};
        }

        // Build a struct/class OutType from the members of a `seq` result,
        // in declaration order.
        template <typename OutType>
        struct into_t {
            auto operator()(auto&&... args) const {
                constexpr auto arg_size = sizeof...(args);
                static_assert(arg_size <= std::meta::nonstatic_data_members_of(
                                  ^^OutType, std::meta::access_context::current()).size(),
                              "into<OutType>: too many parsed values for the target type");

                std::tuple tpl{std::forward<decltype(args)>(args)...};
                OutType out{};
                template for (constexpr int i : std::define_static_array(detail::make_index_array<arg_size>())) {
                    constexpr auto member_info = detail::data_member_at_index(^^OutType, i);
                    out.[:member_info:] = std::get<i>(std::move(tpl));
                }
                return out;
            }
        };

        template <typename OutType>
        constexpr auto into = into_t<OutType>{};
    } // namespace dsl

    namespace preds {
        constexpr auto always_true = [](auto&&...) {
            return true;
        };

        constexpr auto always_false = [](auto&&...) {
            return false;
        };

        constexpr auto not_empty = [](auto&& val) {
            return !val.empty();
        };

        constexpr auto is_digit = [](char c) {
            return c >= '0' && c <= '9';
        };

        constexpr auto is_alpha = [](char c) {
            return std::isalpha(static_cast<unsigned char>(c)) != 0;
        };

        constexpr auto is_alphanumeric = [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0;
        };

        constexpr auto is_space = [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        };

        constexpr auto is_upper = [](char c) {
            return c >= 'A' && c <= 'Z';
        };

        constexpr auto is_lower = [](char c) {
            return c >= 'a' && c <= 'z';
        };

        template <char... Cs>
        constexpr auto one_of = [](char c) {
            return ((c == Cs) || ...);
        };
    }

    constexpr auto context() {
        return detail::parse_context{};
    }

    template <detail::parser_wrapper ParserWrapper>
    struct parser {
        using parser_type = [:ParserWrapper.parser():];

        static auto parse(std::string_view input, detail::parse_context& ctx) {
            return parser_type::parse(input, ctx);
        }
    };

    template <detail::parser_wrapper ParserWrapper>
    auto parse(std::string_view input, detail::parse_context& ctx) {
        return parser<ParserWrapper>::parse(input, ctx);
    }

    // -------------------------------------------------------------------------
    // hook application (trace generation)
    // -------------------------------------------------------------------------

    template <detail::parser_wrapper pw>
    consteval auto apply_hook() {
        constexpr auto parser_info = pw.parser();
        if constexpr (std::meta::has_template_arguments(parser_info)) {
            constexpr auto parser_template = std::meta::template_of(parser_info);
            if constexpr (parser_template == ^^detail::hooked_parser) {
                return pw;
            } else if constexpr (parser_template == ^^detail::sequential_parser ||
                                 parser_template == ^^detail::choice_parser) {
                static constexpr auto args = std::define_static_array(std::meta::template_arguments_of(parser_info));
                std::vector<std::meta::info> new_args;
                template for (constexpr auto arg : args) {
                    new_args.push_back(apply_hook<detail::parser_wrapper{arg}>().parser());
                }
                return detail::parser_wrapper{std::meta::substitute(parser_template, new_args)};
            } else if constexpr (parser_template == ^^detail::parser_decorator) {
                static constexpr auto args = std::define_static_array(std::meta::template_arguments_of(parser_info));
                constexpr detail::parser_wrapper inner_pw{args[0]};
                constexpr auto attr_info = args[1];
                constexpr auto hooked_inner = apply_hook<inner_pw>();
                return detail::parser_wrapper{
                    std::meta::substitute(parser_template, {hooked_inner.parser(), attr_info})};
            } else if constexpr (parser_template == ^^detail::map_parser ||
                                 parser_template == ^^detail::optional_parser ||
                                 parser_template == ^^detail::many_parser ||
                                 parser_template == ^^detail::many1_parser ||
                                 parser_template == ^^detail::count_parser ||
                                 parser_template == ^^detail::label_parser ||
                                 parser_template == ^^detail::guard_parser ||
                                 parser_template == ^^detail::cut_parser ||
                                 parser_template == ^^detail::attempt_parser ||
                                 parser_template == ^^detail::look_ahead_parser ||
                                 parser_template == ^^detail::not_parser ||
                                 parser_template == ^^detail::recognize_parser) {
                static constexpr auto args = std::define_static_array(std::meta::template_arguments_of(parser_info));
                constexpr detail::parser_wrapper inner_pw{args[0]};

                std::vector<std::meta::info> infos;
                infos.push_back(apply_hook<inner_pw>().parser());
                infos.insert(infos.end(), args.begin() + 1, args.end());

                constexpr auto name = std::meta::identifier_of(parser_template);
                constexpr auto name_size = name.size();
                constexpr auto name_lit = detail::string_view_to_literal<name_size>(name.data());

                return detail::parser_wrapper{std::meta::substitute(
                    ^^detail::hooked_parser,
                    {std::meta::reflect_constant(name_lit),
                     std::meta::substitute(parser_template, infos)})};
            } else if constexpr (parser_template == ^^detail::sep_by_parser ||
                                 parser_template == ^^detail::chainl1_parser ||
                                 parser_template == ^^detail::chainr1_parser) {
                // both template arguments are parsers (plus an optional flag)
                static constexpr auto args = std::define_static_array(std::meta::template_arguments_of(parser_info));
                constexpr detail::parser_wrapper a0{args[0]};
                constexpr detail::parser_wrapper a1{args[1]};

                std::vector<std::meta::info> infos;
                infos.push_back(apply_hook<a0>().parser());
                infos.push_back(apply_hook<a1>().parser());
                if constexpr (args.size() > 2) {
                    infos.push_back(args[2]);
                }

                constexpr auto name = std::meta::identifier_of(parser_template);
                constexpr auto name_size = name.size();
                constexpr auto name_lit = detail::string_view_to_literal<name_size>(name.data());

                return detail::parser_wrapper{std::meta::substitute(
                    ^^detail::hooked_parser,
                    {std::meta::reflect_constant(name_lit),
                     std::meta::substitute(parser_template, infos)})};
            } else if constexpr (parser_template == ^^detail::y_combinator) {
                // cannot peek into F; wrap the whole thing without recursing
                return detail::parser_wrapper{std::meta::substitute(
                    ^^detail::hooked_parser,
                    {std::meta::reflect_constant(detail::string_literal("y_combinator")), parser_info})};
            }
        }
        // other parser types, just wrap with hooked_parser
        constexpr auto name_lit = detail::pretty_name<parser_info>();
        return detail::parser_wrapper{std::meta::substitute(
            ^^detail::hooked_parser, {std::meta::reflect_constant(name_lit), parser_info})};
    }
} // namespace parser_combinators
