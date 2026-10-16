#include "program.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <iomanip>
#include <limits>
#include <memory>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <tuple>

namespace flows {
namespace {

// The frontend elaborates a static signal graph; class bodies are not Python.
enum class Kind { identifier, number, string, dot, colon, equals, plus, minus, star, slash,
                  left, right, comma, lt, le, gt, ge, eq, ne, newline, indent, dedent, end };
struct Token {
    Kind kind;
    std::string text;
    SourceSpan span;
};
struct Failure { Diagnostic diagnostic; };

[[noreturn]] void fail(SourceSpan span, std::string message) {
    throw Failure{{span, std::move(message)}};
}
bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}
    std::vector<Token> run() {
        while (offset_ < source_.size()) {
            if (at_line_start_ && parentheses_ == 0) read_indent();
            if (offset_ == source_.size()) break;
            char c = source_[offset_];
            if (c == ' ') { advance(); continue; }
            if (c == '\t') fail(here(), "Use spaces instead of tabs for indentation.");
            if (c == '#') {
                while (offset_ < source_.size() && source_[offset_] != '\n' && source_[offset_] != '\r') advance();
                continue;
            }
            if (c == '\n' || c == '\r') {
                SourceSpan span = here();
                if (c == '\r') {
                    advance();
                    if (offset_ < source_.size() && source_[offset_] == '\n') advance();
                } else advance();
                ++line_;
                column_ = 1;
                at_line_start_ = true;
                if (parentheses_ == 0 && !line_empty_) emit(Kind::newline, span);
                line_empty_ = true;
                continue;
            }
            at_line_start_ = false;
            line_empty_ = false;
            SourceSpan start = here();
            if (c == '"' || c == '\'') { read_string(start, c); continue; }
            if (letter(c)) {
                advance();
                if(c>='A'&&c<='G'&&offset_+1<source_.size()&&source_[offset_]=='#'&&digit(source_[offset_+1]))advance();
                while (offset_ < source_.size() && (letter(source_[offset_]) || digit(source_[offset_]))) advance();
                emit(Kind::identifier, start);
                continue;
            }
            if (digit(c) || (c == '.' && offset_ + 1 < source_.size() && digit(source_[offset_ + 1]))) {
                read_number(start);
                continue;
            }
            advance();
            switch (c) {
                case '.': emit(Kind::dot, start); break;
                case ':': emit(Kind::colon, start); break;
                case '=': if (offset_ < source_.size() && source_[offset_] == '=') { advance(); emit(Kind::eq, start); } else emit(Kind::equals, start); break;
                case '<': if (offset_ < source_.size() && source_[offset_] == '=') { advance(); emit(Kind::le, start); } else emit(Kind::lt, start); break;
                case '>': if (offset_ < source_.size() && source_[offset_] == '=') { advance(); emit(Kind::ge, start); } else emit(Kind::gt, start); break;
                case '!': if (offset_ < source_.size() && source_[offset_] == '=') { advance(); emit(Kind::ne, start); } else fail(start, "Expected '!='."); break;
                case '+': emit(Kind::plus, start); break;
                case '-': emit(Kind::minus, start); break;
                case '*': emit(Kind::star, start); break;
                case '/': emit(Kind::slash, start); break;
                case ',': emit(Kind::comma, start); break;
                case '(': ++parentheses_; emit(Kind::left, start); break;
                case ')':
                    if (parentheses_ == 0) fail(start, "Unexpected ')'.");
                    --parentheses_;
                    emit(Kind::right, start);
                    break;
                default: fail(start, "Unsupported character in Flows source.");
            }
        }
        if (parentheses_ != 0) fail(here(), "Unclosed '('.");
        if (!tokens_.empty() && tokens_.back().kind != Kind::newline) emit(Kind::newline, here());
        while (indents_.size() > 1) { indents_.pop_back(); emit(Kind::dedent, here()); }
        emit(Kind::end, here());
        return std::move(tokens_);
    }
private:
    SourceSpan here() const { return {offset_, 0, line_, column_}; }
    void advance() { ++offset_; ++column_; }
    void emit(Kind kind, SourceSpan start) {
        start.length = offset_ - start.offset;
        if (kind == Kind::identifier && start.length > 256) fail(start, "Identifier exceeds 256 bytes.");
        if (tokens_.size() >= 131072) fail(start, "Source exceeds 128 Ki tokens.");
        tokens_.push_back({kind, std::string(source_.substr(start.offset, start.length)), start});
    }
    void read_indent() {
        SourceSpan start = here();
        std::size_t spaces = 0;
        while (offset_ < source_.size() && source_[offset_] == ' ') { advance(); ++spaces; }
        if (offset_ == source_.size() || source_[offset_] == '#' || source_[offset_] == '\n' || source_[offset_] == '\r') return;
        if (source_[offset_] == '\t') fail(here(), "Use spaces instead of tabs for indentation.");
        if (spaces > indents_.back()) {
            indents_.push_back(spaces);
            emit(Kind::indent, start);
        } else if (spaces < indents_.back()) {
            while (spaces < indents_.back()) { indents_.pop_back(); emit(Kind::dedent, start); }
            if (spaces != indents_.back()) fail(start, "Indentation does not match an outer block.");
        }
        at_line_start_ = false;
    }
    void read_number(SourceSpan start) {
        while (offset_ < source_.size() && digit(source_[offset_])) advance();
        if (offset_ < source_.size() && source_[offset_] == '.') {
            advance();
            while (offset_ < source_.size() && digit(source_[offset_])) advance();
        }
        if (offset_ < source_.size() && (source_[offset_] == 'e' || source_[offset_] == 'E')) {
            advance();
            if (offset_ < source_.size() && (source_[offset_] == '+' || source_[offset_] == '-')) advance();
            if (offset_ == source_.size() || !digit(source_[offset_])) fail(start, "Expected digits in numeric exponent.");
            while (offset_ < source_.size() && digit(source_[offset_])) advance();
        }
        while (offset_ < source_.size() && letter(source_[offset_])) advance();
        emit(Kind::number, start);
    }
    void read_string(SourceSpan start, char quote) {
        advance();
        std::string value;
        while (offset_ < source_.size() && source_[offset_] != quote) {
            unsigned char c = source_[offset_];
            if (c < 0x20 || c == 0x7f) fail(here(), "Sample paths cannot contain control characters or line breaks.");
            advance();
            if (c == '\\') {
                if (offset_ == source_.size()) fail(start, "Unclosed string literal.");
                c = source_[offset_];
                if (c != '\\' && c != '"' && c != '\'') fail(here(), "String escapes support only quotes and backslash.");
                advance();
            }
            value.push_back(static_cast<char>(c));
            if (value.size() > 4096) fail(start, "Sample path exceeds 4096 bytes.");
        }
        if (offset_ == source_.size()) fail(start, "Unclosed string literal.");
        advance();
        emit(Kind::string, start);
        tokens_.back().text = std::move(value);
    }
    std::string_view source_;
    std::size_t offset_ = 0, line_ = 1, column_ = 1;
    std::size_t parentheses_ = 0;
    bool at_line_start_ = true, line_empty_ = true;
    std::vector<std::size_t> indents_{0};
    std::vector<Token> tokens_;
};

SourceSpan joined(SourceSpan first, SourceSpan last) {
    first.length = last.offset + last.length - first.offset;
    return first;
}
Rate combined(Rate left, Rate right) {
    return static_cast<Rate>(std::max(static_cast<int>(left), static_cast<int>(right)));
}
struct Quantity { double value; Unit unit; };

// Floating-point from_chars is unavailable before macOS 26. An explicit C
// locale gives strtod's correctly rounded conversion without changing the
// process locale (which may be shared with a plugin host).
struct NumericLocale {
#if defined(_WIN32)
    _locale_t value = _create_locale(LC_NUMERIC, "C");
    ~NumericLocale() { if (value) _free_locale(value); }
#else
    locale_t value = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    ~NumericLocale() { if (value) freelocale(value); }
#endif
};

Quantity quantity(const Token& token) {
    static const NumericLocale locale;
    if (!locale.value) fail(token.span, "Unable to initialize the numeric parser locale.");
    char* end = nullptr;
#if defined(_WIN32)
    double value = _strtod_l(token.text.c_str(), &end, locale.value);
#else
    double value = strtod_l(token.text.c_str(), &end, locale.value);
#endif
    const std::string_view text = token.text;
    const auto length = static_cast<std::size_t>(end - token.text.c_str());
    const auto mantissa = text.substr(0, text.find_first_of("eE"));
    const bool nonzero = std::any_of(mantissa.begin(), mantissa.end(), [](char c) { return c >= '1' && c <= '9'; });
    // ERANGE also describes valid subnormals. Reject only overflow and a
    // nonzero decimal that actually rounds to zero; zero with any exponent is
    // still exactly zero.
    if (length == 0 || !std::isfinite(value) || (value == 0.0 && nonzero))
        fail(token.span, "Numeric literal must be finite and representable.");
    const std::string_view suffix = text.substr(length);
    Unit unit = Unit::scalar;
    if (suffix == "hz") unit = Unit::hertz;
    else if (suffix == "khz") { unit = Unit::hertz; value *= 1000.0; }
    else if (suffix == "db") unit = Unit::decibels;
    else if (suffix == "ms") { unit = Unit::seconds; value /= 1000.0; }
    else if (suffix == "s") unit = Unit::seconds;
    else if (!suffix.empty()) fail(token.span, "Unsupported unit '" + std::string(suffix) + "'; supported units are hz, khz, db, ms and s.");
    if (!std::isfinite(value) || (value == 0.0 && nonzero)) fail(token.span, "Unit conversion exceeds the finite numeric range.");
    return {value, unit};
}

enum class Builtin { sine, saw, adsr, filter, lfo, distortion, sampler, phasor, custom };
struct Expr { std::uint32_t id; bool analysis = false; bool literal = false; SourceSpan span; };
struct Knob {
    std::optional<Quantity> value, minimum, maximum, step, smoothing;
    SourceSpan value_span;
    bool logarithmic = false, has_scale = false, live = true;
};
struct ResourceRef { std::string path; SourceSpan span; };
struct Port {
    std::string name;
    Unit unit;
    std::optional<Quantity> value;
    SourceSpan span;
    bool sample = false;
    std::optional<ResourceRef> resource;
};
struct ClassDef {
    std::string name;
    Builtin base = Builtin::custom;
    std::vector<Port> ports;
    std::map<std::string, Knob> knobs;
    std::size_t body_begin = 0, body_end = 0;
};
struct Instance { Builtin kind; std::string class_name; std::uint32_t state = 0; bool called = false; };
struct Argument { std::optional<Expr> value; std::string symbol; SourceSpan span, value_span; bool boolean_literal = false; std::optional<ResourceRef> resource; };
struct Build {
    Program program;
    std::vector<std::optional<double>> initial;
    std::vector<std::optional<std::uint32_t>> zero;
    std::map<std::string, Builtin> imports;
    std::map<std::string, ClassDef> classes;
    std::map<std::string, ResourceRef> resource_aliases;
    std::vector<std::pair<std::size_t,std::uint32_t>> sampler_mode_choices;
    std::vector<std::string> expansion;
    std::optional<std::uint32_t> gate_closed;
    std::size_t expanded_classes = 0, statements = 0;
    std::string standard;
    std::set<ValueId> defaults, registered_steps, registered_scales;
};

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string source_path)
        : tokens_(std::make_shared<const std::vector<Token>>(std::move(tokens))), build_(&owned_) { build_->program.source_path = std::move(source_path); }
    Program run() {
        skip_newlines();
        bool instrument = false;
        while (!at(Kind::end)) {
            statement();
            if (instrument) fail(peek().span, "Only one Inst.sigstream block is allowed; move definitions before it.");
            if (word("import")) parse_import();
            else if (word("from")) parse_from();
            else if (word("class")) parse_class();
            else if (word("Inst")) { parse_instrument(); instrument = true; }
            else {
                Token name = expect(Kind::identifier, "Expected an import, class, event source or Inst.sigstream.");
                declare(name); expect(Kind::equals, "Top-level assignments must define a key event source or sample path.");
                if (is_resource()) build_->resource_aliases[name.text] = resource_value();
                else policies_[name.text] = parse_policy(name.text);
                line_end();
            }
            skip_newlines();
        }
        if (!instrument) fail(peek().span, "Expected one Inst.sigstream block.");
        reconcile_sampler_lifetimes();
        reconcile_choices();
        return std::move(build_->program);
    }
private:
    Parser(std::shared_ptr<const std::vector<Token>> tokens, Build& build, std::size_t begin, std::string prefix)
        : tokens_(std::move(tokens)), build_(&build), position_(begin), prefix_(std::move(prefix)) {}
    const Token& peek(std::size_t ahead = 0) const { return (*tokens_)[std::min(position_ + ahead, tokens_->size() - 1)]; }
    bool at(Kind kind) const { return peek().kind == kind; }
    bool word(std::string_view text) const { return at(Kind::identifier) && peek().text == text; }
    Token take() { return (*tokens_)[position_++]; }
    bool accept(Kind kind) { if (!at(kind)) return false; take(); return true; }
    Token expect(Kind kind, const std::string& message) { if (!at(kind)) fail(peek().span, message); return take(); }
    Token expect_word(std::string_view text) { if (!word(text)) fail(peek().span, "Expected '" + std::string(text) + "'."); return take(); }
    void skip_newlines() { while (accept(Kind::newline)) {} }
    void line_end() { expect(Kind::newline, "Expected end of statement."); }
    std::string qualified(const std::string& name) const { return prefix_ + name; }
    std::string choice_owner() const { return qualified(binding_context_ == "$return" ? "$output" : binding_context_); }
    std::size_t add_choice(std::string binding, std::string name, std::string selected, std::string owner,
                           SourceSpan span, std::vector<ChoiceOption> options, bool implicit = false,
                           std::string prefix = {}, std::string suffix = {}) {
        if (program().choices.size() >= 16384) fail(span,"Program exceeds 16384 discrete choices.");
        program().choices.push_back({std::move(binding),std::move(name),std::move(selected),std::move(owner),span,implicit,
                                     std::move(prefix),std::move(suffix),std::move(options)});
        return program().choices.size()-1;
    }
    static void disable_option(ChoiceDescriptor& choice, const std::string& value, std::string reason) {
        for (auto& option : choice.options) if (option.value == value) { option.enabled = false; option.reason = std::move(reason); }
    }
    void reconcile_sampler_lifetimes() {
        if (program().voice_policy.mode != VoiceMode::poly || !program().completion_root) return;
        for (const auto& [index,state] : build_->sampler_mode_choices) {
            // Looping removes this sampler's EOF witness. Offer the choice
            // only when the complete output still has another lifetime proof.
            std::vector<bool> proven;
            for (const auto& expression : program().completion) {
                bool value = !(expression.op == CompletionOp::sample_done && expression.state_index == state);
                if (expression.op == CompletionOp::all || expression.op == CompletionOp::any) {
                    value = expression.op == CompletionOp::all;
                    for (auto input : expression.inputs)
                        value = expression.op == CompletionOp::all ? value && proven[input] : value || proven[input];
                }
                proven.push_back(value);
            }
            if (!proven[*program().completion_root])
                disable_option(program().choices[index], "loop", "Add an output gate or envelope before looping this poly sampler.");
        }
    }
    void reconcile_choices() {
        // A class body can elaborate to several owners. Editing its single
        // source span must be valid for every instance using that source.
        using Key = std::tuple<std::size_t,std::size_t,std::string,std::string,std::string>;
        std::map<Key,std::vector<std::size_t>> shared;
        for (std::size_t i=0;i<program().choices.size();++i) {
            const auto& choice=program().choices[i];
            shared[{choice.span.offset,choice.span.length,choice.name,choice.prefix,choice.suffix}].push_back(i);
        }
        for (const auto& [key,indices] : shared) {
            std::map<std::string,std::string> blocked;
            for (auto index : indices) for (const auto& option : program().choices[index].options)
                if (!option.enabled) blocked.try_emplace(option.value,option.reason);
            for (auto index : indices) for (const auto& [value,reason] : blocked) disable_option(program().choices[index],value,reason);
        }
    }
    void declare(const Token& name) {
        static const std::set<std::string> reserved{"Inst","import","from","as","return","class","if","else","def","probe","power","spec","spectrum","mel_spec","true","false"};
        if (reserved.contains(name.text) || occupied_.contains(name.text) || build_->imports.contains(name.text) || build_->classes.contains(name.text) || name.text == build_->standard)
            fail(name.span, "Name '" + name.text + "' is already defined or reserved.");
        occupied_.insert(name.text);
    }
    void statement() {
        if (++build_->statements > 8192) fail(peek().span, "Expanded program exceeds 8192 statements.");
    }
    void named_budget(SourceSpan span) {
        if (program().named_values.size() + program().named_analysis.size() >= 8192) fail(span, "Expanded program exceeds 8192 named signals.");
    }
    void analysis_budget(SourceSpan span) {
        if (program().analysis.size() >= 16384) fail(span, "Program exceeds 16384 analysis nodes.");
    }
    Program& program() { return build_->program; }
    bool is_resource() const {
        return at(Kind::string) || (at(Kind::identifier) &&
            (resources_.contains(peek().text) || (!values_.contains(peek().text) && build_->resource_aliases.contains(peek().text))));
    }
    ResourceRef resource_value() {
        if (at(Kind::identifier)) {
            auto name = take();
            if (resources_.contains(name.text)) return resources_.at(name.text);
            if (build_->resource_aliases.contains(name.text)) return build_->resource_aliases.at(name.text);
            fail(name.span, "Expected a constant sample path.");
        }
        const auto value = expect(Kind::string, "Expected a quoted sample path.");
        if (value.text.empty() || value.text.front() == '/' || value.text.find_first_of("\\:") != std::string::npos)
            fail(value.span, "Sample paths must be relative to the asset root and use '/' separators.");
        std::size_t start = 0;
        while (start <= value.text.size()) {
            auto end = value.text.find('/', start);
            if (end == std::string::npos) end = value.text.size();
            const auto component = value.text.substr(start, end - start);
            if (component.empty() || component == "." || component == "..")
                fail(value.span, "Sample paths cannot contain empty, '.' or '..' components.");
            start = end + 1;
        }
        return {value.text, value.span};
    }
    std::uint32_t sample_resource(const ResourceRef& resource) {
        for (std::size_t i = 0; i < program().samples.size(); ++i)
            if (program().samples[i].path == resource.path) return static_cast<std::uint32_t>(i);
        if (program().samples.size() >= 64) fail(resource.span, "Program exceeds 64 sample resources.");
        program().samples.push_back({resource.path, resource.span});
        return static_cast<std::uint32_t>(program().samples.size() - 1);
    }
    const Node& node(std::uint32_t id) const { return build_->program.nodes[id]; }
    Unit unit(Expr value) const { return value.analysis ? build_->program.analysis[value.id].unit : node(value.id).unit; }
    std::uint32_t completion(CompletionOp op, std::uint32_t state = 0, std::vector<std::uint32_t> inputs = {}) {
        auto id = static_cast<std::uint32_t>(program().completion.size());
        program().completion.push_back({op, state, std::move(inputs)}); return id;
    }
    std::uint32_t gate_completion() {
        if (!build_->gate_closed) build_->gate_closed = completion(CompletionOp::gate_closed);
        return *build_->gate_closed;
    }
    Expr append(Node next, std::optional<double> initial = std::nullopt, bool literal = false) {
        if (program().nodes.size() >= 16384) fail(next.span, "Program exceeds 16384 DSP nodes.");
        if (initial && (!std::isfinite(*initial) || std::abs(*initial) > std::numeric_limits<float>::max())) fail(next.span, "Expression produces a non-finite or unrepresentable audio value.");
        if (next.op == Op::add || next.op == Op::subtract || next.op == Op::multiply || next.op == Op::divide)
            next.channels = std::max(node(next.inputs[0]).channels, node(next.inputs[1]).channels);
        else if (next.op == Op::select)
            next.channels = std::max(node(next.inputs[1]).channels, node(next.inputs[2]).channels);
        else if (next.op == Op::filter_lowpass || next.op == Op::filter_highpass || next.op == Op::distortion || next.op == Op::math_sin || next.op == Op::math_cos)
            next.channels = node(next.inputs[0]).channels;
        if (next.channels == 2) initial.reset();
        std::optional<std::uint32_t> zero;
        if (next.op == Op::key_gate) zero = gate_completion();
        auto both = [&](CompletionOp combine) {
            std::vector<std::uint32_t> children;
            for (auto input : next.inputs) {
                if (build_->zero[input]) children.push_back(*build_->zero[input]);
                else if (combine == CompletionOp::all) return std::optional<std::uint32_t>{};
            }
            if (children.empty()) return std::optional<std::uint32_t>{};
            if (children.size() == 1) return std::optional<std::uint32_t>{children[0]};
            return std::optional<std::uint32_t>{completion(combine, 0, std::move(children))};
        };
        if (next.op == Op::multiply) zero = both(CompletionOp::any);
        else if (next.op == Op::add || next.op == Op::subtract || next.op == Op::stereo) zero = both(CompletionOp::all);
        else if ((next.op == Op::left || next.op == Op::right) && node(next.inputs[0]).op == Op::stereo)
            zero = build_->zero[node(next.inputs[0]).inputs[next.op == Op::left ? 0 : 1]];
        else if (next.op == Op::divide || next.op == Op::left || next.op == Op::right || next.op == Op::mid || next.op == Op::side || next.op == Op::math_sin) zero = build_->zero[next.inputs[0]];
        else if (next.op == Op::sine || next.op == Op::saw || next.op == Op::unison_sine || next.op == Op::unison_saw) zero = build_->zero[next.inputs[1]];
        else if (next.op == Op::adsr && build_->zero[next.inputs[0]]) zero = completion(CompletionOp::envelope_idle, next.state_index);
        else if ((next.op == Op::filter_lowpass || next.op == Op::filter_highpass) && build_->zero[next.inputs[0]])
            zero = completion(CompletionOp::all, 0, {*build_->zero[next.inputs[0]], completion(CompletionOp::filter_settled, next.state_index)});
        else if (next.op == Op::distortion && build_->zero[next.inputs[0]])
            zero = completion(CompletionOp::all, 0, {*build_->zero[next.inputs[0]], completion(CompletionOp::distortion_settled, next.state_index)});
        else if (next.op == Op::sampler)
            zero = (next.flags & 1u) ? build_->zero[next.inputs[7]] :
                std::optional<std::uint32_t>{completion(CompletionOp::sample_done, next.state_index)};
        else if (next.op == Op::select && build_->zero[next.inputs[1]] && build_->zero[next.inputs[2]])
            zero = completion(CompletionOp::all, 0, {*build_->zero[next.inputs[1]], *build_->zero[next.inputs[2]]});
        const auto id = static_cast<ValueId>(program().nodes.size());
        const auto span = next.span;
        program().nodes.push_back(std::move(next)); build_->initial.push_back(initial); build_->zero.push_back(zero);
        return {id, false, literal, span};
    }
    Expr constant(double value, Unit type, SourceSpan span, bool authored = false) {
        Expr result = append({Op::parameter, {}, value, type, Rate::constant, span, 0}, value, authored);
        if (authored) {
            ParameterDescriptor descriptor;
            descriptor.value = result.id; descriptor.binding_id = qualified(binding_context_) + ".literal" + std::to_string(literal_index_++);
            descriptor.unit = type; descriptor.span = span; descriptor.default_value = value; descriptor.name = binding_context_;
            program().parameters.push_back(std::move(descriptor));
        }
        return result;
    }
    Expr analysis_constant(double value, Unit type, SourceSpan span) {
        analysis_budget(span);
        auto id = static_cast<std::uint32_t>(program().analysis.size());
        program().analysis.push_back({AnalysisOp::constant, {}, invalid_value, value, AnalysisShape::scalar, type, span});
        return {id, true, true, span};
    }
    Expr as_analysis(Expr value) {
        if (value.analysis) return value;
        fail(value.span, "Use probe(signal) before combining a DSP signal with analysis results.");
    }
    Expr require_dsp(Expr value) const { if (value.analysis) fail(value.span, "Analysis results cannot feed the realtime DSP graph."); return value; }
    void require_unit(Expr value, Unit type, const std::string& context) const {
        require_dsp(value); if (unit(value) != type) fail(value.span, context + " requires " + std::string(unit_name(type)) + ".");
    }
    void require_mono(Expr value, const std::string& context) const {
        require_dsp(value);
        if (node(value.id).channels != 1) fail(value.span, context + " requires a mono signal; select left(), right(), mid() or side().");
    }
    void structural_parameter(Expr value) {
        std::vector<ValueId> dependencies{value.id};
        std::set<ValueId> seen;
        while (!dependencies.empty()) {
            const auto id = dependencies.back(); dependencies.pop_back();
            if (!seen.insert(id).second) continue;
            for (auto& parameter : program().parameters) if (parameter.value == id) parameter.live = false;
            for (auto input : node(id).inputs) dependencies.push_back(input);
        }
    }
    Expr conversion(Expr input, Op op, Unit type) {
        std::optional<double> initial;
        if (build_->initial[input.id]) initial = op == Op::db_to_linear ? std::pow(10.0, *build_->initial[input.id] / 20.0) : 440.0 * std::exp2((*build_->initial[input.id] - 69.0) / 12.0);
        return append({op, {input.id}, 0, type, node(input.id).rate, input.span, 0}, initial);
    }
    void register_range(Expr value, const std::string& name, double minimum, double maximum, double step, bool logarithmic = false) {
        if (!value.analysis && build_->initial[value.id] && (*build_->initial[value.id] < minimum || *build_->initial[value.id] > maximum)) fail(value.span, name + " is outside its supported range.");
        if (value.analysis || !value.literal) return;
        for (auto& parameter : program().parameters) if (parameter.value == value.id) {
            parameter.name = name;
            if (parameter.has_range) { parameter.min = std::max(parameter.min, minimum); parameter.max = std::min(parameter.max, maximum); }
            else { parameter.min = minimum; parameter.max = maximum; }
            if (parameter.min > parameter.max) fail(value.span, "The shared parameter has incompatible ranges.");
            parameter.has_range = true;
            if (!build_->registered_steps.contains(value.id)) parameter.step = step;
            if (!build_->registered_scales.contains(value.id)) {
                parameter.logarithmic = logarithmic;
                parameter.control_scale = logarithmic ? "log" : "linear";
                parameter.control_offset = 0;
                if (!logarithmic && parameter.unit == Unit::seconds && minimum >= 0) {
                    parameter.control_scale = "offset_log"; parameter.control_offset = .001;
                } else if (!logarithmic && parameter.unit == Unit::hertz && minimum >= 0) {
                    parameter.control_scale = "offset_log"; parameter.control_offset = name == "rate" ? .05 : 20;
                } else if (parameter.unit == Unit::decibels && (name == "level" || name == "output")) {
                    parameter.control_scale = "decibels";
                } else if (parameter.unit == Unit::scalar && (name == "level" || name == "output") && minimum >= 0) {
                    parameter.control_scale = "gain";
                } else if (name == "rate" && parameter.unit == Unit::scalar && minimum > 0) {
                    parameter.control_scale = "log";
                }
            }
            if (parameter.has_default && (parameter.default_value < parameter.min || parameter.default_value > parameter.max))
                fail(value.span, "Parameter default is outside its supported range.");
            if(name=="level"||name=="cutoff"||name=="resonance"||name=="S"||name=="detune"||name=="spread"||name=="pan"||name=="drive"||name=="mix"||name=="output"||name=="rate")parameter.smoothing_seconds=.005;
            return;
        }
    }
    // The outermost port owns a literal's reset behavior. Its class default
    // outranks its port fallback, which in turn outranks inner primitives.
    void declare_default(Expr value, const Quantity& fallback, bool registered = false) {
        if (value.analysis || !value.literal) return;
        if (!build_->defaults.insert(value.id).second) return;
        for (auto& parameter : program().parameters) if (parameter.value == value.id) {
            double converted = fallback.value;
            if (unit(value) != fallback.unit) {
                if (unit(value) == Unit::decibels && fallback.unit == Unit::scalar) {
                    if (fallback.value < 0) fail(value.span, "Parameter default is outside its supported range.");
                    if (fallback.value == 0) return; // Silence has no finite dB spelling.
                    converted = 20.0 * std::log10(fallback.value);
                } else if (unit(value) == Unit::scalar && fallback.unit == Unit::decibels)
                    converted = std::pow(10.0, fallback.value / 20.0);
                else { if (registered) fail(value.span, "Parameter default has incompatible units."); return; }
            }
            if (!std::isfinite(converted)) fail(value.span, "Parameter default is not finite in the authored unit.");
            if (parameter.has_range && (converted < parameter.min || converted > parameter.max))
                fail(value.span, "Parameter default is outside its supported range.");
            parameter.default_value = converted;
            parameter.has_default = true;
            return;
        }
    }
    void register_knob(Expr value, const std::string& name, const Knob& knob) {
        if (knob.minimum && knob.maximum) {
            if (knob.minimum->unit != unit(value) || knob.maximum->unit != unit(value)) fail(value.span, "Registered parameter range has incompatible units.");
            register_range(value, name, knob.minimum->value, knob.maximum->value, knob.step ? knob.step->value : 0.01, knob.logarithmic);
        }
        if (value.analysis || !value.literal) return;
        if (knob.value) declare_default(value, *knob.value, true);
        for (auto& descriptor : program().parameters) if (descriptor.value == value.id) {
            descriptor.live = knob.live; descriptor.name = name;
            if (knob.smoothing) descriptor.smoothing_seconds = knob.smoothing->value;
            if (knob.step) {
                if (knob.step->unit != unit(value)) fail(value.span, "Registered step has incompatible units.");
                if (build_->registered_steps.insert(value.id).second) descriptor.step = knob.step->value;
            }
            if (knob.has_scale && build_->registered_scales.insert(value.id).second) {
                descriptor.logarithmic = knob.logarithmic;
                descriptor.control_scale = knob.logarithmic ? "log" : "linear";
                descriptor.control_offset = 0;
            }
        }
    }
    void parse_import() {
        take(); expect_word("standard"); expect_word("as"); Token alias = expect(Kind::identifier, "Expected standard alias.");
        if (!build_->standard.empty()) fail(alias.span, "standard has already been imported.");
        declare(alias); build_->standard = alias.text; line_end();
    }
    void parse_from() {
        take(); expect_word("lib"); expect(Kind::dot, "Expected library module.");
        Token module = expect(Kind::identifier, "Expected osc, env, filt, mod, fx, sample or dsp library."); expect_word("import");
        do {
            Token imported = expect(Kind::identifier, "Expected imported class."); Builtin kind;
            if (module.text == "osc" && imported.text == "ClassicSine") kind = Builtin::sine;
            else if (module.text == "osc" && imported.text == "ClassicSaw") kind = Builtin::saw;
            else if (module.text == "env" && imported.text == "ClassicADSR") kind = Builtin::adsr;
            else if (module.text == "filt" && imported.text == "ClassicFilt") kind = Builtin::filter;
            else if (module.text == "mod" && imported.text == "ClassicLFO") kind = Builtin::lfo;
            else if (module.text == "fx" && imported.text == "ClassicDistortion") kind = Builtin::distortion;
            else if (module.text == "sample" && imported.text == "ClassicSampler") kind = Builtin::sampler;
            else if (module.text == "dsp" && imported.text == "Phasor") kind = Builtin::phasor;
            else fail(imported.span, "Unknown builtin import lib." + module.text + "." + imported.text + ".");
            Token alias = imported; if (word("as")) { take(); alias = expect(Kind::identifier, "Expected import alias."); }
            declare(alias); build_->imports[alias.text] = kind;
        } while (accept(Kind::comma));
        line_end();
    }
    Quantity signed_quantity() {
        double sign = 1.0; if (accept(Kind::minus)) sign = -1.0; else accept(Kind::plus);
        if (at(Kind::identifier)) if (auto pitch = note_pitch(peek().text)) { take(); return {*pitch * sign, Unit::pitch}; }
        Token literal = expect(Kind::number, "Expected a constant numeric quantity."); auto result = quantity(literal); result.value *= sign; return result;
    }
    VoicePolicy parse_policy(const std::string& binding) {
        if (build_->standard.empty()) fail(peek().span, "Import standard before using keys.");
        expect_word(build_->standard); expect(Kind::dot,"Expected .event.keys."); expect_word("event"); expect(Kind::dot,"Expected .keys."); expect_word("keys"); expect(Kind::dot,"Expected key policy.");
        Token mode = expect(Kind::identifier, "Expected poly or mono."); VoicePolicy policy;
        if (mode.text == "mono") policy.mode = VoiceMode::mono;
        else if (mode.text != "poly") fail(mode.span, "Key policy must be poly or mono.");
        expect(Kind::left,"Expected '(' after key policy."); std::set<std::string> seen;
        std::map<std::string,Token> explicit_choices;
        while (!at(Kind::right)) {
            Token name = expect(Kind::identifier,"Expected policy argument.");
            if (!seen.insert(name.text).second) fail(name.span,"Duplicate key policy argument.");
            expect(Kind::colon,"Expected ':' after policy argument.");
            if (policy.mode == VoiceMode::poly) fail(name.span,"keys.poly() takes no arguments.");
            if (name.text == "priority") explicit_choices.emplace(name.text,expect_word("last"));
            else if (name.text == "retrigger") {
                Token value = expect(Kind::identifier,"Expected retrigger mode."); explicit_choices.emplace(name.text,value);
                if (value.text == "each_press") policy.retrigger = Retrigger::each_press;
                else if (value.text == "gate_rise") policy.retrigger = Retrigger::gate_rise;
                else fail(value.span,"retrigger must be each_press or gate_rise.");
            } else if (name.text == "glide") {
                auto value = signed_quantity();
                if (value.unit != Unit::seconds || value.value < 0 || value.value > 30) fail(name.span,"glide requires a time from 0ms to 30s.");
                policy.glide_seconds = value.value;
            } else fail(name.span,"Unknown key policy argument.");
            if (!accept(Kind::comma)) break;
        }
        const bool trailing_comma=position_>0&&(*tokens_)[position_-1].kind==Kind::comma;
        const Token close=expect(Kind::right,"Expected ')' after key policy.");
        const std::string id="$policy."+binding;
        auto& choices=policy_choices_[binding];
        choices.push_back(add_choice(id+".mode","mode",mode.text,"",mode.span,{{"poly"},{"mono"}}));
        if (!seen.empty()) disable_option(program().choices[choices.back()],"poly","Remove mono arguments before switching to poly.");
        if (policy.mode==VoiceMode::mono) {
            auto field=[&](const std::string& name,const std::string& selected,std::vector<ChoiceOption> options) {
                auto found=explicit_choices.find(name);
                if (found!=explicit_choices.end()) choices.push_back(add_choice(id+"."+name,name,selected,"",found->second.span,std::move(options)));
                else { auto span=close.span;span.length=0;choices.push_back(add_choice(id+"."+name,name,selected,"",span,std::move(options),true,(seen.empty()||trailing_comma?"":", ")+name+": ")); }
            };
            field("priority","last",{{"last"}});
            field("retrigger",policy.retrigger==Retrigger::each_press?"each_press":"gate_rise",{{"each_press"},{"gate_rise"}});
        }
        return policy;
    }
    Unit port_type() {
        Token type = expect(Kind::identifier,"Expected port type.");
        if (type.text == "scalar" || type.text == "audio" || type.text == "control") return Unit::scalar;
        if (type.text == "pitch") return Unit::pitch;
        if (type.text == "hz" || type.text == "hertz") return Unit::hertz;
        if (type.text == "db" || type.text == "decibels") return Unit::decibels;
        if (type.text == "seconds" || type.text == "time") return Unit::seconds;
        if (type.text == "bool" || type.text == "boolean") return Unit::boolean;
        fail(type.span,"Unknown signal port type.");
    }
    void parse_class() {
        take(); Token name = expect(Kind::identifier,"Expected class name."); declare(name); ClassDef definition; definition.name = name.text;
        if (accept(Kind::left)) {
            Token base = expect(Kind::identifier,"Expected builtin base class.");
            if (!build_->imports.contains(base.text)) fail(base.span,"Class inheritance supports an imported builtin primitive.");
            definition.base = build_->imports.at(base.text); expect(Kind::right,"Expected ')' after base class.");
        }
        expect(Kind::colon,"Expected ':' after class."); line_end(); expect(Kind::indent,"Expected class body.");
        bool has_signal = false, has_init = false;
        while (!at(Kind::dedent) && !at(Kind::end)) {
            if (accept(Kind::newline)) continue;
            expect_word("def"); Token method = expect(Kind::identifier,"Expected init or signal method.");
            if (method.text == "init") {
                if (has_init) fail(method.span,"Duplicate init method."); has_init = true;
                if (accept(Kind::left)) expect(Kind::right,"init takes no arguments.");
                expect(Kind::colon,"Expected ':' after init."); line_end(); expect(Kind::indent,"Expected init metadata body.");
                while (!at(Kind::dedent)) {
                    if (accept(Kind::newline)) continue;
                    expect_word("self"); expect(Kind::dot,"Expected .register_knob."); expect_word("register_knob"); expect(Kind::left,"Expected knob registration arguments.");
                    Token target = expect(Kind::identifier,"Expected knob name.");
                    while (accept(Kind::dot)) target = expect(Kind::identifier,"Expected knob path component.");
                    Knob knob; std::set<std::string> fields; std::optional<Token> scale_choice, update_choice;
                    while (accept(Kind::comma) && !at(Kind::right)) {
                        Token field = expect(Kind::identifier,"Expected knob metadata field.");
                        if (!fields.insert(field.text).second) fail(field.span,"Duplicate knob metadata field.");
                        expect(Kind::equals,"Knob metadata uses name=value.");
                        if (field.text == "default") { knob.value_span=peek().span;knob.value = signed_quantity();knob.value_span=joined(knob.value_span,(*tokens_)[position_-1].span); }
                        else if (field.text == "range") {
                            expect(Kind::left,"Expected (minimum, maximum)."); knob.minimum = signed_quantity(); expect(Kind::comma,"Expected range comma."); knob.maximum = signed_quantity(); expect(Kind::right,"Expected ')' after range.");
                            if (knob.minimum->unit != knob.maximum->unit || knob.minimum->value >= knob.maximum->value) fail(field.span,"Invalid parameter range.");
                        } else if (field.text == "step") { knob.step = signed_quantity(); if (knob.step->value <= 0) fail(field.span,"Parameter step must be positive."); }
                        else if (field.text == "smoothing") {
                            knob.smoothing = signed_quantity();
                            if (knob.smoothing->unit != Unit::seconds || knob.smoothing->value < 0 || knob.smoothing->value > 1)
                                fail(field.span,"Knob smoothing requires a time from 0ms to 1s.");
                        }
                        else if (field.text == "scale") { Token scale = expect(Kind::identifier,"Expected linear or log."); if (scale.text != "linear" && scale.text != "log") fail(scale.span,"Scale must be linear or log."); knob.logarithmic = scale.text == "log"; knob.has_scale = true; scale_choice=scale; }
                        else if (field.text == "update") { Token update = expect(Kind::identifier,"Expected live or rebuild."); if (update.text != "live" && update.text != "rebuild") fail(update.span,"Update must be live or rebuild."); knob.live = update.text == "live"; update_choice=update; }
                        else fail(field.span,"Unknown knob metadata field.");
                    }
                    if (knob.logarithmic && (!knob.minimum || knob.minimum->value <= 0)) fail(target.span,"Log range requires a positive minimum.");
                    const std::string choice_id=definition.name+".knob."+target.text+".";
                    if (scale_choice) {
                        const auto index=add_choice(choice_id+"scale","scale",scale_choice->text,"",scale_choice->span,{{"linear"},{"log"}});
                        if (!knob.minimum || knob.minimum->value<=0) disable_option(program().choices[index],"log","Log scale requires a registered range with a positive minimum.");
                    }
                    if (update_choice) add_choice(choice_id+"update","update",update_choice->text,"",update_choice->span,{{"live"},{"rebuild"}});
                    if (!definition.knobs.emplace(target.text, knob).second) fail(target.span,"Duplicate registered knob.");
                    expect(Kind::right,"Expected ')' after registration."); line_end();
                }
                take();
            } else if (method.text == "signal") {
                if (has_signal || definition.base != Builtin::custom) fail(method.span,"A composition has one signal method; builtin subclasses only customize metadata."); has_signal = true;
                expect(Kind::left,"Expected typed signal ports."); std::set<std::string> ports;
                while (!at(Kind::right)) {
                    Token port = expect(Kind::identifier,"Expected port name."); if (!ports.insert(port.text).second) fail(port.span,"Duplicate signal port."); expect(Kind::colon,"Signal ports require types.");
                    const bool sample = word("sample");
                    const auto type = sample ? (take(), Unit::scalar) : port_type();
                    Port item{port.text,type,std::nullopt,port.span,sample,std::nullopt};
                    if (accept(Kind::equals)) {
                        if (sample) { item.resource = resource_value(); item.span = item.resource->span; }
                        else { item.span = peek().span; item.value = signed_quantity(); item.span = joined(item.span,(*tokens_)[position_-1].span); if (item.value->unit != item.unit) fail(item.span,"Port default has incompatible units."); }
                    }
                    definition.ports.push_back(item); if (!accept(Kind::comma)) break;
                }
                expect(Kind::right,"Expected ')' after signal ports."); expect(Kind::colon,"Expected signal body."); line_end(); expect(Kind::indent,"Expected signal block."); definition.body_begin = position_;
                std::size_t depth = 1;
                while (depth && !at(Kind::end)) { if (at(Kind::indent)) ++depth; else if (at(Kind::dedent)) --depth; if (depth) take(); }
                if (depth) fail(method.span,"Unclosed signal block."); definition.body_end = position_; take();
            } else fail(method.span,"Classes support only signal and metadata init methods.");
        }
        expect(Kind::dedent,"Expected class end.");
        if (definition.base == Builtin::custom && !has_signal) fail(name.span,"A composition class requires a typed signal method.");
        std::set<std::string> known;
        if(definition.base==Builtin::custom) { for(const auto& port:definition.ports)if (!port.sample) known.insert(port.name); }
        else if(definition.base==Builtin::sine||definition.base==Builtin::saw)known={"pitch","level","unison","octave","semitune","detune","spread","pan","phase","random"};
        else if(definition.base==Builtin::phasor)known={"freq"};
        else if(definition.base==Builtin::adsr)known={"gate","trigger","A","D","S","R"};
        else if(definition.base==Builtin::lfo)known={"rate","sync","trigger","phase","delay","fade","bipolar"};
        else if(definition.base==Builtin::distortion)known={"drive","mix","output"};
        else if(definition.base==Builtin::sampler)known={"pitch","root","trigger","gate","rate","start","end","level","reverse"};
        else known={"cutoff","resonance"};
        for(const auto& [target,knob]:definition.knobs){
            if(!known.contains(target))fail(name.span,"Registered knob '"+target+"' is not a class port or builtin parameter.");
            if(knob.value&&knob.minimum&&(knob.value->unit!=knob.minimum->unit||knob.value->value<knob.minimum->value||knob.value->value>knob.maximum->value))fail(knob.value_span,"Registered default is outside its typed range.");
        }
        build_->classes.emplace(name.text,std::move(definition));
    }
    void parse_instrument() {
        take(); expect(Kind::dot,"Expected Inst.sigstream."); expect_word("sigstream"); expect(Kind::left,"Expected sigstream source.");
        std::string policy_binding="$keys";
        if (at(Kind::identifier) && policies_.contains(peek().text)) {policy_binding=take().text;program().voice_policy=policies_.at(policy_binding);} else program().voice_policy=parse_policy(policy_binding);
        expect(Kind::right,"Expected ')' after source."); expect_word("as"); Token key = expect(Kind::identifier,"Expected key name."); declare(key); key_alias_ = key.text;
        for (auto index : policy_choices_[policy_binding]) program().choices[index].owner=key_alias_+".pitch";
        expect(Kind::colon,"Expected ':' before sigstream."); line_end(); expect(Kind::indent,"Expected an indented sigstream block.");
        // Event fields are potential taps even when the first patch does not use them.
        for (const std::string field : {"pitch","gate","velocity","trigger"}) key_value(field,key.span);
        Expr result = block(); expect(Kind::dedent,"Expected sigstream end."); require_unit(result,Unit::scalar,"Instrument output");
        program().output = result.id; program().completion_root = build_->zero[result.id];
        if (!program().completion_root) for (auto index : policy_choices_[policy_binding])
            if (program().choices[index].name=="mode") disable_option(program().choices[index],"poly","Poly requires each output path to end through key.gate, an envelope or finite one-shot playback.");
        if (program().voice_policy.mode == VoiceMode::poly && !program().completion_root) fail(result.span,"Poly output lifetime is unproven: each output path needs key.gate, a key-controlled ADSR or finite one-shot playback.");
        named_budget(result.span); program().named_values.push_back({"$output",result.id,result.span});
    }
    Expr block() {
        std::optional<Expr> result;
        while (!at(Kind::dedent) && !at(Kind::end)) {
            if (accept(Kind::newline)) continue;
            if (result) fail(peek().span,"Statements after return are not supported.");
            if (word("return")) {
                statement();
                Token start = take(); binding_context_ = "$return"; literal_index_ = 0; analysis_context_ = false;
                result = require_dsp(expression()); result->span = joined(start.span,(*tokens_)[position_-1].span); line_end();
            } else if (word("if")) conditional();
            else assignment();
        }
        if (!result) fail(peek().span,"Signal block requires a return statement."); return *result;
    }
    void conditional() {
        statement();
        Token start = take(); if (branch_depth_ >= 32) fail(start.span,"Conditional nesting exceeds 32.");
        analysis_context_ = false; Expr condition = require_dsp(expression());
        require_mono(condition,"if condition");
        if (unit(condition) != Unit::boolean && unit(condition) != Unit::scalar) fail(condition.span,"if requires a scalar or boolean condition.");
        expect(Kind::colon,"Expected ':' after if."); line_end(); expect(Kind::indent,"Expected if block.");
        auto baseline = values_; auto names = occupied_; ++branch_depth_;
        const auto previous_branch=branch_scope_;const auto branch="if"+std::to_string(conditional_index_++);
        branch_scope_=branch+".then.";
        while (!at(Kind::dedent)) { if (accept(Kind::newline)) continue; if (word("if") || word("return")) fail(peek().span,"Conditional blocks contain pure signal assignments."); assignment(); }
        take(); auto yes = values_; values_ = baseline; occupied_ = names;branch_scope_=branch+".else.";
        expect_word("else"); expect(Kind::colon,"Expected ':' after else."); line_end(); expect(Kind::indent,"Expected else block.");
        while (!at(Kind::dedent)) { if (accept(Kind::newline)) continue; if (word("if") || word("return")) fail(peek().span,"Conditional blocks contain pure signal assignments."); assignment(); }
        take(); auto no = values_; values_ = baseline; occupied_ = names; --branch_depth_;branch_scope_=previous_branch;
        std::set<std::string> assigned;
        for (const auto& [name,value] : yes) if (!baseline.contains(name)) assigned.insert(name);
        for (const auto& [name,value] : no) if (!baseline.contains(name)) assigned.insert(name);
        if (assigned.empty()) fail(start.span,"if/else must assign a signal in both branches.");
        for (const auto& name : assigned) {
            if (!yes.contains(name) || !no.contains(name)) fail(start.span,"Both if/else branches must assign '" + name + "'.");
            Expr a = require_dsp(yes.at(name)), b = require_dsp(no.at(name)); if (unit(a) != unit(b)) fail(start.span,"if/else branches have incompatible units.");
            Expr merged = append({Op::select,{condition.id,a.id,b.id},0,unit(a),combined(node(condition.id).rate,combined(node(a.id).rate,node(b.id).rate)),start.span,0});
            values_[name] = merged; occupied_.insert(name); named_budget(start.span); program().named_values.push_back({qualified(name),merged.id,start.span});
        }
    }
    bool analysis_statement() const {
        for (std::size_t i = position_; i < tokens_->size() && (*tokens_)[i].kind != Kind::newline; ++i) {
            const auto& token = (*tokens_)[i];
            if (token.kind != Kind::identifier) continue;
            if (token.text == "probe" || token.text == "power" || token.text == "spec" || token.text == "spectrum" || token.text == "mel_spec") return true;
            auto value = values_.find(token.text); if (value != values_.end() && value->second.analysis) return true;
        }
        return false;
    }
    void assignment() {
        statement();
        Token name = expect(Kind::identifier,"Expected assignment, if or return."); expect(Kind::equals,"Expected '=' after signal name."); declare(name);
        binding_context_ = branch_scope_ + name.text; literal_index_ = 0;
        if (is_resource()) {
            if (branch_depth_) fail(peek().span,"Sample paths must be outside conditional blocks.");
            resources_[name.text] = resource_value();
        } else if (at(Kind::identifier) && (build_->imports.contains(peek().text) || build_->classes.contains(peek().text))) {
            if (branch_depth_) fail(peek().span,"State creation must be outside conditional blocks.");
            Token type = take(); expect(Kind::dot,"Expected .make()."); expect_word("make"); expect(Kind::left,"Expected '(' after make."); expect(Kind::right,"make() takes no arguments.");
            Builtin kind = build_->imports.contains(type.text) ? build_->imports.at(type.text) : build_->classes.at(type.text).base;
            std::uint32_t state = 0;
            if (kind != Builtin::custom) {
                if (program().states.size() >= 4096) fail(name.span,"Program exceeds 4096 states per voice.");
                state = static_cast<std::uint32_t>(program().states.size());
                StateKind state_kind = kind == Builtin::adsr ? StateKind::envelope : kind == Builtin::filter ? StateKind::filter :
                    kind == Builtin::lfo ? StateKind::lfo : kind == Builtin::distortion ? StateKind::distortion : kind == Builtin::sampler ? StateKind::sampler : StateKind::oscillator;
                program().states.push_back({qualified(name.text),name.span,state_kind});
            }
            instances_[name.text] = {kind,build_->classes.contains(type.text) ? type.text : "",state,false};
        } else {
            analysis_context_ = analysis_statement(); Expr value = expression(); analysis_context_ = false; values_[name.text] = value;
            named_budget(name.span);
            if (value.analysis) program().named_analysis.push_back({qualified(name.text),value.id,name.span});
            else program().named_values.push_back({qualified(name.text) + (branch_depth_ ? ".branch" + std::to_string(name.span.offset) : ""),value.id,name.span});
        }
        line_end();
    }
    static int precedence(Kind kind) {
        if (kind == Kind::lt || kind == Kind::le || kind == Kind::gt || kind == Kind::ge || kind == Kind::eq || kind == Kind::ne) return 1;
        if (kind == Kind::plus || kind == Kind::minus) return 2;
        if (kind == Kind::star || kind == Kind::slash) return 3;
        return 0;
    }
    Expr expression(int minimum = 1) {
        if (++expression_depth_ > 256) fail(peek().span,"Expression nesting exceeds 256.");
        Expr left = primary();
        while (precedence(peek().kind) >= minimum) { Token op = take(); Expr right = expression(precedence(op.kind)+1); left = binary(op,left,right); }
        --expression_depth_; return left;
    }
    Expr key_value(const std::string& field, SourceSpan span) {
        const std::string name = key_alias_ + "." + field;
        if (values_.contains(name)) return values_.at(name);
        Op op; Unit type = Unit::scalar; Rate rate = Rate::event_held;
        if (field == "pitch") { op = Op::key_pitch; type = Unit::pitch; if (program().voice_policy.glide_seconds > 0) rate = Rate::sample_varying; }
        else if (field == "gate") op = Op::key_gate;
        else if (field == "velocity") op = Op::key_velocity;
        else if (field == "trigger") { op = Op::key_trigger; rate = Rate::sample_varying; }
        else fail(span,"Unsupported key field '" + field + "'.");
        Expr value = append({op,{},0,type,rate,span,0}); values_[name] = value; program().named_values.push_back({name,value.id,span}); return value;
    }
    std::optional<double> note_pitch(const std::string& text) const {
        if (text.size() < 2 || text[0] < 'A' || text[0] > 'G') return std::nullopt;
        static constexpr int semitones[]{9,11,0,2,4,5,7}; std::size_t i = 1; int accidental = 0;
        if (text[i] == 's' || text[i] == '#') { accidental = 1; ++i; } else if (text[i] == 'b') { accidental = -1; ++i; }
        if (i >= text.size()) return std::nullopt;
        int octave = 0; for (;i < text.size();++i) { if (!digit(text[i])) return std::nullopt; octave = octave*10 + text[i]-'0'; if (octave > 9) return std::nullopt; }
        double pitch = (octave+1)*12 + semitones[text[0]-'A'] + accidental; if (pitch > 127) return std::nullopt; return pitch;
    }
    Expr primary() {
        if (at(Kind::minus) || at(Kind::plus)) {
            Token sign = take();
            if (at(Kind::number)) {
                Token number = take(); auto parsed = quantity(number); parsed.value *= sign.kind == Kind::minus ? -1.0 : 1.0; auto span = joined(sign.span,number.span);
                return analysis_context_ ? analysis_constant(parsed.value,parsed.unit,span) : constant(parsed.value,parsed.unit,span,true);
            }
            if (++expression_depth_ > 256) fail(sign.span,"Expression nesting exceeds 256."); Expr value = primary(); --expression_depth_;
            if (sign.kind == Kind::plus) return value;
            Expr negative = value.analysis ? analysis_constant(-1,Unit::scalar,sign.span) : constant(-1,Unit::scalar,sign.span);
            return binary({Kind::star,"*",sign.span},negative,value);
        }
        if (at(Kind::number)) { Token number = take(); auto parsed = quantity(number); return analysis_context_ ? analysis_constant(parsed.value,parsed.unit,number.span) : constant(parsed.value,parsed.unit,number.span,true); }
        if (accept(Kind::left)) { Expr value = expression(); expect(Kind::right,"Expected ')' after expression."); return value; }
        Token name = expect(Kind::identifier,"Expected a numeric literal, signal or expression.");
        if (!key_alias_.empty() && name.text == key_alias_) { expect(Kind::dot,"Expected key field."); Token field = expect(Kind::identifier,"Expected key field."); Expr value = key_value(field.text,joined(name.span,field.span)); value.span = joined(name.span,field.span); return value; }
        if (name.text == "true" || name.text == "false") return constant(name.text == "true" ? 1:0,Unit::boolean,name.span);
        if (auto pitch = note_pitch(name.text)) return analysis_context_ ? analysis_constant(*pitch,Unit::pitch,name.span) : constant(*pitch,Unit::pitch,name.span,true);
        if (name.text == "probe" || name.text == "power" || name.text == "spec" || name.text == "spectrum" || name.text == "mel_spec") return analysis_call(name);
        if (at(Kind::left) && (name.text == "sin" || name.text == "cos" || name.text == "pitch_hz" || name.text == "db_gain")) return math_call(name);
        if (at(Kind::left) && (name.text == "stereo" || name.text == "left" || name.text == "right" || name.text == "mid" || name.text == "side")) return channel_call(name);
        if (instances_.contains(name.text)) return instance_call(name,instances_.at(name.text));
        if (at(Kind::left)) fail(name.span,"Unknown callable '" + name.text + "'.");
        if (!values_.contains(name.text)) fail(name.span,"Unknown signal '" + name.text + "' or use before definition.");
        Expr value = values_.at(name.text); value.span = name.span; return value;
    }
    Expr math_call(const Token& name) {
        expect(Kind::left,"Expected math function argument.");
        Expr input = require_dsp(expression());
        expect(Kind::right,"Math functions accept exactly one signal.");
        const auto span = joined(name.span,(*tokens_)[position_-1].span);
        if (name.text == "pitch_hz" || name.text == "db_gain") {
            require_mono(input,name.text);
            require_unit(input,name.text == "pitch_hz" ? Unit::pitch : Unit::decibels,name.text);
            auto value = conversion(input,name.text == "pitch_hz" ? Op::pitch_to_hz : Op::db_to_linear,
                                    name.text == "pitch_hz" ? Unit::hertz : Unit::scalar);
            value.span = span; return value;
        }
        require_unit(input,Unit::scalar,name.text + " (radians)");
        std::optional<double> initial;
        if (build_->initial[input.id]) initial = name.text == "sin" ? std::sin(*build_->initial[input.id]) : std::cos(*build_->initial[input.id]);
        return append({name.text == "sin" ? Op::math_sin : Op::math_cos,{input.id},0,Unit::scalar,node(input.id).rate,span},initial);
    }
    Expr channel_call(const Token& name) {
        expect(Kind::left,"Expected channel function arguments.");
        Expr first = require_dsp(expression()); require_unit(first,Unit::scalar,name.text);
        const auto rate = node(first.id).rate;
        if (name.text == "stereo") {
            require_mono(first,"stereo left input"); expect(Kind::comma,"stereo requires left and right mono inputs.");
            Expr second = require_dsp(expression()); require_unit(second,Unit::scalar,"stereo"); require_mono(second,"stereo right input");
            expect(Kind::right,"stereo accepts exactly two mono inputs.");
            return append({Op::stereo,{first.id,second.id},0,Unit::scalar,combined(rate,node(second.id).rate),name.span,0,0,2});
        }
        expect(Kind::right,"Channel selection accepts exactly one signal.");
        const Op operation = name.text == "left" ? Op::left : name.text == "right" ? Op::right : name.text == "mid" ? Op::mid : Op::side;
        return append({operation,{first.id},0,Unit::scalar,rate,name.span});
    }
    Expr analysis_call(const Token& name) {
        expect(Kind::left,"Expected analysis argument.");
        if (name.text == "probe") {
            Token target=expect(Kind::identifier,"probe requires an existing signal name.");
            Expr source;
            if(!key_alias_.empty()&&target.text==key_alias_){expect(Kind::dot,"Expected key signal field.");Token field=expect(Kind::identifier,"Expected key signal field.");source=key_value(field.text,joined(target.span,field.span));}
            else {auto found=values_.find(target.text);if(found==values_.end())fail(target.span,"probe requires an existing DSP signal.");source=found->second;}
            if (source.analysis) fail(source.span,"probe requires an existing DSP signal.");
            expect(Kind::right,"probe accepts one existing signal, without inline DSP operations.");
            analysis_budget(name.span); auto id = static_cast<std::uint32_t>(program().analysis.size()); program().analysis.push_back({AnalysisOp::probe,{},source.id,0,AnalysisShape::waveform,unit(source),joined(name.span,(*tokens_)[position_-1].span)});
            return {id,true,false,name.span};
        }
        Expr input = as_analysis(expression()); expect(Kind::right,"Expected ')' after analysis input.");
        const auto shape = program().analysis[input.id].shape;
        if (shape != AnalysisShape::waveform) fail(input.span,"power/spectrum/mel_spec require a waveform probe.");
        AnalysisOp op = name.text == "power" ? AnalysisOp::power : name.text == "mel_spec" ? AnalysisOp::mel_spectrum : AnalysisOp::spectrum;
        AnalysisShape output = op == AnalysisOp::power ? AnalysisShape::scalar : op == AnalysisOp::mel_spectrum ? AnalysisShape::mel_spectrum : AnalysisShape::spectrum;
        analysis_budget(name.span); auto id = static_cast<std::uint32_t>(program().analysis.size()); program().analysis.push_back({op,{input.id},invalid_value,0,output,unit(input),name.span}); return {id,true,false,name.span};
    }
    Expr binary(const Token& token, Expr left, Expr right) {
        Unit a = unit(left), b = unit(right), result = a; Op op;
        const bool comparison = precedence(token.kind) == 1;
        if (comparison) {
            if (left.analysis || right.analysis) fail(token.span,"Analysis comparisons are not implemented.");
            require_mono(left,"Comparison"); require_mono(right,"Comparison");
            if (a != b) fail(token.span,"Comparison requires matching units."); result = Unit::boolean;
            switch (token.kind) { case Kind::lt:op=Op::less;break;case Kind::le:op=Op::less_equal;break;case Kind::gt:op=Op::greater;break;case Kind::ge:op=Op::greater_equal;break;case Kind::eq:op=Op::equal;break;default:op=Op::not_equal;break; }
        } else if (token.kind == Kind::plus || token.kind == Kind::minus) {
            op = token.kind == Kind::plus ? Op::add : Op::subtract;
            if (a == Unit::pitch && b == Unit::scalar) result = Unit::pitch;
            else if (a == Unit::scalar && b == Unit::pitch && token.kind == Kind::plus) result = Unit::pitch;
            else if (a == Unit::pitch && b == Unit::pitch && token.kind == Kind::minus) result = Unit::scalar;
            else if (a == b && a != Unit::pitch && a != Unit::boolean) result = a;
            else fail(token.span,"Cannot add/subtract incompatible units.");
        } else if (token.kind == Kind::star) {
            op = Op::multiply;
            if (a == Unit::scalar) result = b; else if (b == Unit::scalar) result = a; else fail(token.span,"Multiplication requires a scalar operand.");
        } else {
            op = Op::divide;
            if (b == Unit::scalar) result = a; else if (a == b) result = Unit::scalar; else fail(token.span,"Division requires a scalar divisor or matching units.");
        }
        if (left.analysis || right.analysis) {
            left=as_analysis(left); right=as_analysis(right); auto x=program().analysis[left.id].shape,y=program().analysis[right.id].shape;
            if (x != y && x != AnalysisShape::scalar && y != AnalysisShape::scalar) fail(token.span,"Analysis operands require compatible shapes.");
            auto shape = x == AnalysisShape::scalar ? y : x;
            AnalysisOp operation = op==Op::add?AnalysisOp::add:op==Op::subtract?AnalysisOp::subtract:op==Op::multiply?AnalysisOp::multiply:AnalysisOp::divide;
            analysis_budget(token.span); auto id=static_cast<std::uint32_t>(program().analysis.size()); program().analysis.push_back({operation,{left.id,right.id},invalid_value,0,shape,result,token.span}); return {id,true,false,token.span};
        }
        if (op == Op::divide && (!build_->initial[right.id] || *build_->initial[right.id] == 0)) fail(token.span,"Division requires a nonzero constant-rate divisor.");
        if (op == Op::divide) {
            // The current live-update API cannot atomically re-prove arbitrary
            // denominator expressions. Recompile their literals before changing
            // them, so a drag cannot introduce a realtime division by zero.
            std::vector<ValueId> dependencies{right.id};
            std::set<ValueId> seen;
            while (!dependencies.empty()) {
                const auto id=dependencies.back();dependencies.pop_back();
                if(!seen.insert(id).second)continue;
                for(auto& parameter:program().parameters)if(parameter.value==id)parameter.live=false;
                for(auto input:node(id).inputs)dependencies.push_back(input);
            }
        }
        std::optional<double> initial;
        if (build_->initial[left.id] && build_->initial[right.id]) {
            const double x=*build_->initial[left.id], y=*build_->initial[right.id];
            switch(op) { case Op::add:initial=x+y;break;case Op::subtract:initial=x-y;break;case Op::multiply:initial=x*y;break;case Op::divide:initial=x/y;break;case Op::less:initial=x<y;break;case Op::less_equal:initial=x<=y;break;case Op::greater:initial=x>y;break;case Op::greater_equal:initial=x>=y;break;case Op::equal:initial=x==y;break;case Op::not_equal:initial=x!=y;break;default:break; }
        }
        return append({op,{left.id,right.id},0,result,combined(node(left.id).rate,node(right.id).rate),token.span,0},initial);
    }
    std::map<std::string,Argument> arguments(Builtin kind, SourceSpan& insertion, std::string& separator) {
        std::map<std::string,Argument> result; std::size_t positional=0;
        expect(Kind::left,"Expected instance call arguments.");
        while (!at(Kind::right)) {
            std::string name; SourceSpan span=peek().span;
            if (at(Kind::identifier) && peek(1).kind==Kind::colon) { name=take().text;take(); }
            else name="$"+std::to_string(positional++);
            if (result.contains(name)) fail(span,"Duplicate instance parameter '"+name+"'.");
            Argument argument; argument.span=span; const auto value_begin=position_; argument.value_span=peek().span;
            const bool symbolic = (name=="mode"&&(kind==Builtin::filter||kind==Builtin::lfo||kind==Builtin::distortion||kind==Builtin::sampler)) ||
                (name=="retrigger_from"&&kind==Builtin::adsr) || (name=="shape"&&kind==Builtin::lfo);
            if (symbolic) argument.symbol=expect(Kind::identifier,"Expected symbolic parameter value.").text;
            else if (is_resource()) argument.resource=resource_value();
            else argument.value=require_dsp(expression());
            argument.value_span=joined(argument.value_span,(*tokens_)[position_-1].span);
            argument.boolean_literal=position_==value_begin+1&&((*tokens_)[value_begin].text=="true"||(*tokens_)[value_begin].text=="false");
            result[name]=argument; if (!accept(Kind::comma)) break;
        }
        separator=result.empty()||(*tokens_)[position_-1].kind==Kind::comma?"":", ";
        insertion=expect(Kind::right,"Expected ',' or ')' after instance argument.").span; insertion.length=0; return result;
    }
    Expr instance_call(const Token& name, Instance& instance) {
        if (branch_depth_) fail(name.span,"Stateful calls must be outside pure conditional blocks.");
        if (instance.called) fail(name.span,"Stateful instance '"+name.text+"' can be called only once; reuse its signal result.");
        instance.called=true; SourceSpan insertion;std::string separator; auto args=arguments(instance.kind,insertion,separator);
        const ClassDef* definition = instance.class_name.empty()?nullptr:&build_->classes.at(instance.class_name);
        if (instance.kind==Builtin::custom) return composition(name,*definition,args);
        std::set<std::string> allowed;
        if (instance.kind==Builtin::sine || instance.kind==Builtin::saw) allowed={"pitch","level","unison","octave","semitune","detune","spread","pan","phase","random"};
        else if (instance.kind==Builtin::phasor) allowed={"freq"};
        else if (instance.kind==Builtin::adsr) allowed={"gate","trigger","A","D","S","R","retrigger_from"};
        else if (instance.kind==Builtin::lfo) allowed={"rate","sync","shape","phase","mode","trigger","bipolar","delay","fade"};
        else if (instance.kind==Builtin::distortion) allowed={"$0","input","drive","mix","output","mode"};
        else if (instance.kind==Builtin::sampler) allowed={"file","pitch","root","trigger","gate","mode","rate","start","end","level","reverse"};
        else allowed={"$0","input","cutoff","resonance","mode"};
        for (const auto& [key,arg]:args) if (!allowed.contains(key)) fail(arg.span,"Unknown instance parameter '"+key+"'.");
        auto get=[&](const std::string& key,std::optional<Quantity> fallback=std::nullopt) {
            Expr value;
            if (args.contains(key)) { if (!args.at(key).value) fail(args.at(key).span,"Expected numeric/signal parameter."); value=*args.at(key).value; }
            else if (definition && definition->knobs.contains(key) && definition->knobs.at(key).value) { const auto& knob=definition->knobs.at(key);auto q=*knob.value;value=constant(q.value,q.unit,knob.value_span,true); }
            else if (fallback) value=constant(fallback->value,fallback->unit,name.span);
            else { fail(name.span,"Instance requires '"+key+"'."); }
            if (key != "$0" && key != "input") require_mono(value,key);
            if (definition && definition->knobs.contains(key)) register_knob(value,key,definition->knobs.at(key));
            if (fallback) declare_default(value,*fallback);
            return value;
        };
        auto supplied = [&](const std::string& key) {
            return args.contains(key) || (definition && definition->knobs.contains(key) && definition->knobs.at(key).value);
        };
        auto choice = [&](const std::string& key,const std::string& selected,std::vector<ChoiceOption> options) {
            const auto found=args.find(key);
            const bool implicit=found==args.end();
            return add_choice(choice_owner()+".choice."+name.text+"."+key,key,selected,choice_owner(),
                implicit?insertion:found->second.value_span,std::move(options),implicit,implicit?separator+key+": ":"");
        };
        auto scalar = [&](const std::string& key, double fallback, double minimum, double maximum, double step) {
            Expr value=get(key,Quantity{fallback,Unit::scalar}); require_unit(value,Unit::scalar,key); register_range(value,key,minimum,maximum,step); return value;
        };
        if (instance.kind==Builtin::phasor) {
            Expr frequency = get("freq"); require_unit(frequency,Unit::hertz,"freq");
            register_range(frequency,"freq",0,384000,1,true);
            return append({Op::phasor,{frequency.id},0,Unit::scalar,Rate::sample_varying,
                           joined(name.span,(*tokens_)[position_-1].span),instance.state});
        }
        if (instance.kind==Builtin::sampler) {
            if (!args.contains("file") || !args.at("file").resource) fail(name.span,"Sampler requires file: with a constant sample path.");
            const auto resource = sample_resource(*args.at("file").resource);
            const std::string mode=args.contains("mode")?args.at("mode").symbol:"one_shot";
            if (mode!="one_shot" && mode!="loop") fail(args.at("mode").span,"Sampler mode must be one_shot or loop.");
            if (mode=="one_shot" && supplied("gate")) fail(name.span,"Sampler gate is only used in loop mode; shape one_shot output with an envelope or key.gate.");
            const auto mode_choice=choice("mode",mode,{{"one_shot"},{"loop"}});
            if (mode=="one_shot") build_->sampler_mode_choices.emplace_back(mode_choice,instance.state);
            if (supplied("gate")) disable_option(program().choices[mode_choice],"one_shot","Remove gate before switching to one_shot.");
            Expr trigger=get("trigger"); require_unit(trigger,Unit::scalar,"trigger");
            Expr pitch=get("pitch",Quantity{48,Unit::pitch}),root=get("root",Quantity{48,Unit::pitch});
            require_unit(pitch,Unit::pitch,"pitch"); require_unit(root,Unit::pitch,"root");
            register_range(pitch,"pitch",0,127,1); register_range(root,"root",0,127,1);
            Expr speed=scalar("rate",1,1.0/64.0,64,.01);
            Expr start=scalar("start",0,0,1,.001),end=scalar("end",1,0,1,.001);
            if (build_->initial[start.id] && build_->initial[end.id] && *build_->initial[start.id]>=*build_->initial[end.id])
                fail(start.span,"Sampler start must be less than end.");
            Expr gain=get("level",Quantity{1,Unit::scalar});
            if (unit(gain)==Unit::decibels) { register_range(gain,"level",-120,24,.1); gain=conversion(gain,Op::db_to_linear,Unit::scalar); }
            else { require_unit(gain,Unit::scalar,"level"); register_range(gain,"level",0,16,.00001); }
            Expr gate=get("gate",Quantity{1,Unit::scalar}); require_unit(gate,Unit::scalar,"gate");
            Expr reverse=get("reverse",Quantity{0,Unit::scalar});
            if ((unit(reverse)!=Unit::boolean && unit(reverse)!=Unit::scalar) || !build_->initial[reverse.id] ||
                (*build_->initial[reverse.id]!=0 && *build_->initial[reverse.id]!=1))
                fail(reverse.span,"reverse requires a constant true or false.");
            structural_parameter(reverse);
            if (!args.contains("reverse") || args.at("reverse").boolean_literal) {
                const auto index=choice("reverse",*build_->initial[reverse.id]!=0?"true":"false",{{"false"},{"true"}});
                if (definition&&definition->knobs.contains("reverse")&&definition->knobs.at("reverse").minimum) {
                    disable_option(program().choices[index],"false","This class registers a scalar reverse range; edit that registration before using boolean literals.");
                    disable_option(program().choices[index],"true","This class registers a scalar reverse range; edit that registration before using boolean literals.");
                }
            }
            const std::uint32_t flags=(mode=="loop"?1u:0u)|(*build_->initial[reverse.id]!=0?2u:0u);
            return append({Op::sampler,{trigger.id,pitch.id,root.id,speed.id,start.id,end.id,gain.id,gate.id},0,Unit::scalar,Rate::sample_varying,
                joined(name.span,(*tokens_)[position_-1].span),instance.state,flags,2,resource});
        }
        if (instance.kind==Builtin::sine || instance.kind==Builtin::saw) {
            std::uint32_t count=1;
            if (supplied("unison")) {
                Expr unison=get("unison",Quantity{1,Unit::scalar}); require_unit(unison,Unit::scalar,"unison");
                if (!build_->initial[unison.id] || std::floor(*build_->initial[unison.id]) != *build_->initial[unison.id] || *build_->initial[unison.id] < 1 || *build_->initial[unison.id] > 8)
                    fail(unison.span,"unison requires a constant integer from 1 to 8.");
                register_range(unison,"unison",1,8,1); structural_parameter(unison);
                for (auto& parameter : program().parameters) if (parameter.value == unison.id) parameter.discrete = true;
                count=static_cast<std::uint32_t>(*build_->initial[unison.id]);
            }
            const bool expanded=count>1 || supplied("detune") || supplied("spread") || supplied("pan") || supplied("phase") || supplied("random");
            Expr pitch=get("pitch");
            if (unit(pitch)!=Unit::pitch && unit(pitch)!=Unit::hertz) fail(pitch.span,"pitch: requires key.pitch or a quantity in hz / khz.");
            if (supplied("octave") || supplied("semitune")) {
                if (unit(pitch)!=Unit::pitch) fail(pitch.span,"octave/semitune require pitch input, not fixed hertz.");
                if (supplied("octave")) { Expr octave=get("octave",Quantity{0,Unit::scalar});require_unit(octave,Unit::scalar,"octave");register_range(octave,"octave",-8,8,1); Expr offset=binary({Kind::star,"*",name.span},octave,constant(12,Unit::scalar,name.span));pitch=binary({Kind::plus,"+",name.span},pitch,offset); }
                if (supplied("semitune")) { Expr shift=get("semitune",Quantity{0,Unit::scalar});require_unit(shift,Unit::scalar,"semitune");register_range(shift,"semitune",-96,96,1); pitch=binary({Kind::plus,"+",name.span},pitch,shift); }
            }
            if (unit(pitch)==Unit::pitch) pitch=conversion(pitch,Op::pitch_to_hz,Unit::hertz);
            if (build_->initial[pitch.id] && *build_->initial[pitch.id]<0) fail(pitch.span,"Oscillator frequency must be non-negative.");
            register_range(pitch,"frequency",0,100000,.01);
            Expr gain=get("level",Quantity{1,Unit::scalar});
            if (unit(gain)==Unit::decibels) {register_range(gain,"level",-120,24,0.1);gain=conversion(gain,Op::db_to_linear,Unit::scalar);}
            else {require_unit(gain,Unit::scalar,"level:");register_range(gain,"level",0,16,.00001);}
            if (!expanded) return append({instance.kind==Builtin::sine?Op::sine:Op::saw,{pitch.id,gain.id},0,Unit::scalar,Rate::sample_varying,joined(name.span,(*tokens_)[position_-1].span),instance.state});
            Expr detune=scalar("detune",10,0,100,0.1),spread=scalar("spread",1,0,1,.01),pan=scalar("pan",0,-1,1,.01);
            Expr phase=scalar("phase",0,0,1,.01),random=scalar("random",1,0,1,.01);
            if (!build_->initial[phase.id] || !build_->initial[random.id]) fail(name.span,"Oscillator phase/random require constant note-start values.");
            structural_parameter(phase); structural_parameter(random);
            return append({instance.kind==Builtin::sine?Op::unison_sine:Op::unison_saw,{pitch.id,gain.id,detune.id,spread.id,pan.id,phase.id,random.id},0,Unit::scalar,Rate::sample_varying,joined(name.span,(*tokens_)[position_-1].span),instance.state,count,2});
        }
        if (instance.kind==Builtin::lfo) {
            const bool sync=supplied("sync");
            if (sync && supplied("rate")) fail(name.span,"LFO rate and sync are mutually exclusive.");
            Expr speed = sync ? get("sync") : get("rate",Quantity{1,Unit::hertz});
            require_unit(speed,sync?Unit::scalar:Unit::hertz,sync?"sync":"rate");
            register_range(speed,sync?"sync":"rate",sync?1.0/1024.0:0.0,sync?64.0:100.0,sync?1.0/64.0:.01);
            const std::string shape=args.contains("shape")?args.at("shape").symbol:"sine";
            std::uint32_t flags=0;
            if(shape=="triangle")flags=1;else if(shape=="saw")flags=2;else if(shape=="square")flags=3;else if(shape=="sample_hold")flags=4;
            else if(shape!="sine")fail(args.at("shape").span,"LFO shape must be sine, triangle, saw, square or sample_hold.");
            const std::string mode=args.contains("mode")?args.at("mode").symbol:"free";
            if(mode=="retrigger")flags|=1u<<3;else if(mode=="one_shot")flags|=2u<<3;else if(mode=="transport")flags|=3u<<3;
            else if(mode!="free")fail(args.at("mode").span,"LFO mode must be free, retrigger, one_shot or transport.");
            choice("shape",shape,{{"sine"},{"triangle"},{"saw"},{"square"},{"sample_hold"}});
            const auto mode_choice=choice("mode",mode,{{"free"},{"retrigger"},{"one_shot"},{"transport"}});
            if (!args.contains("trigger")) {
                disable_option(program().choices[mode_choice],"retrigger","Provide an explicit trigger signal first.");
                disable_option(program().choices[mode_choice],"one_shot","Provide an explicit trigger signal first.");
            }
            if (!sync) disable_option(program().choices[mode_choice],"transport","Use sync instead of rate before selecting transport mode.");
            if(mode=="transport"&&!sync)fail(name.span,"Transport LFO mode requires sync.");
            if((mode=="retrigger"||mode=="one_shot")&&!args.contains("trigger"))fail(name.span,"Retrigger and one_shot LFO modes require an explicit trigger signal.");
            Expr bipolar=get("bipolar",Quantity{0,Unit::scalar});
            if((unit(bipolar)!=Unit::boolean&&unit(bipolar)!=Unit::scalar)||!build_->initial[bipolar.id]||(*build_->initial[bipolar.id]!=0&&*build_->initial[bipolar.id]!=1))fail(bipolar.span,"bipolar requires a constant true or false.");
            structural_parameter(bipolar); if(*build_->initial[bipolar.id]!=0)flags|=1u<<5; if(sync)flags|=1u<<6;
            if (!args.contains("bipolar") || args.at("bipolar").boolean_literal) {
                const auto index=choice("bipolar",*build_->initial[bipolar.id]!=0?"true":"false",{{"false"},{"true"}});
                if (definition&&definition->knobs.contains("bipolar")&&definition->knobs.at("bipolar").minimum) {
                    disable_option(program().choices[index],"false","This class registers a scalar bipolar range; edit that registration before using boolean literals.");
                    disable_option(program().choices[index],"true","This class registers a scalar bipolar range; edit that registration before using boolean literals.");
                }
            }
            Expr trigger=get("trigger",Quantity{0,Unit::scalar});require_unit(trigger,Unit::scalar,"trigger");
            Expr phase=scalar("phase",0,0,1,.01),delay=get("delay",Quantity{0,Unit::seconds}),fade=get("fade",Quantity{0,Unit::seconds});
            for(auto [value,label]:std::vector<std::pair<Expr,std::string>>{{delay,"delay"},{fade,"fade"}}){require_unit(value,Unit::seconds,label);register_range(value,label,0,30,.00001);}
            return append({Op::lfo,{speed.id,trigger.id,phase.id,delay.id,fade.id},0,Unit::scalar,Rate::sample_varying,name.span,instance.state,flags});
        }
        if (instance.kind==Builtin::distortion) {
            if(args.contains("$0")&&args.contains("input"))fail(name.span,"Distortion input supplied twice.");
            Expr input=get(args.contains("$0")?"$0":"input"); require_unit(input,Unit::scalar,"Distortion input");
            auto level=[&](const std::string& key,double fallback,double lower,double upper) {
                Expr value=get(key,Quantity{fallback,Unit::decibels});
                if(unit(value)==Unit::decibels){register_range(value,key,lower,upper,.1);return conversion(value,Op::db_to_linear,Unit::scalar);}
                require_unit(value,Unit::scalar,key);register_range(value,key,0,key=="drive"?256:16,key=="drive"?.01:.00001);return value;
            };
            Expr drive=level("drive",12,-24,48),mix=scalar("mix",1,0,1,.01),output=level("output",0,-120,24);
            const std::string mode=args.contains("mode")?args.at("mode").symbol:"softclip";
            std::uint32_t flags=0;if(mode=="hardclip")flags=1;else if(mode=="fold")flags=2;else if(mode!="softclip")fail(args.at("mode").span,"Distortion mode must be softclip, hardclip or fold.");
            choice("mode",mode,{{"softclip"},{"hardclip"},{"fold"}});
            return append({Op::distortion,{input.id,drive.id,mix.id,output.id},0,Unit::scalar,Rate::sample_varying,name.span,instance.state,flags});
        }
        if (instance.kind==Builtin::adsr) {
            Expr gate=get("gate"),trigger=get("trigger",Quantity{0,Unit::scalar});require_unit(gate,Unit::scalar,"gate");require_unit(trigger,Unit::scalar,"trigger");
            Expr attack=get("A",Quantity{.01,Unit::seconds}),decay=get("D",Quantity{.1,Unit::seconds}),sustain=get("S",Quantity{.6,Unit::scalar}),release=get("R",Quantity{.2,Unit::seconds});
            for (auto [value,label]:std::vector<std::pair<Expr,std::string>>{{attack,"A"},{decay,"D"},{release,"R"}}) {require_unit(value,Unit::seconds,label);register_range(value,label,0,30,.00001);}
            require_unit(sustain,Unit::scalar,"S");register_range(sustain,"S",0,1,.01);
            std::uint32_t flags=0;
            if (args.contains("retrigger_from")) {const auto& mode=args.at("retrigger_from");if(mode.symbol=="zero")flags=retrigger_from_zero;else if(mode.symbol!="current")fail(mode.span,"retrigger_from must be current or zero.");}
            choice("retrigger_from",flags?"zero":"current",{{"current"},{"zero"}});
            return append({Op::adsr,{gate.id,trigger.id,attack.id,decay.id,sustain.id,release.id},0,Unit::scalar,Rate::sample_varying,name.span,instance.state,flags});
        }
        if(args.contains("$0")&&args.contains("input"))fail(name.span,"Filter input supplied twice.");
        Expr input=get(args.contains("$0")?"$0":"input"),cutoff=get("cutoff",Quantity{800,Unit::hertz}),resonance=get("resonance",Quantity{.1,Unit::scalar});
        require_unit(input,Unit::scalar,"Filter input");require_unit(cutoff,Unit::hertz,"cutoff");require_unit(resonance,Unit::scalar,"resonance");register_range(cutoff,"cutoff",20,20000,1,true);register_range(resonance,"resonance",0,.99,.01);
        Op operation=Op::filter_lowpass;
        if(args.contains("mode")){const auto& mode=args.at("mode");if(mode.symbol=="highpass")operation=Op::filter_highpass;else if(mode.symbol!="lowpass")fail(mode.span,"Filter mode must be lowpass or highpass.");}
        choice("mode",operation==Op::filter_highpass?"highpass":"lowpass",{{"lowpass"},{"highpass"}});
        return append({operation,{input.id,cutoff.id,resonance.id},0,Unit::scalar,Rate::sample_varying,name.span,instance.state});
    }
    Expr composition(const Token& name,const ClassDef& definition,const std::map<std::string,Argument>& args) {
        if (++build_->expanded_classes > 2048) fail(name.span,"Program exceeds 2048 class expansions.");
        if(build_->expansion.size()>=16||std::find(build_->expansion.begin(),build_->expansion.end(),definition.name)!=build_->expansion.end())fail(name.span,"Recursive class composition is not allowed.");
        for(const auto&[argument,value]:args)if(std::none_of(definition.ports.begin(),definition.ports.end(),[&](const Port&port){return port.name==argument;}))fail(value.span,"Unknown class port '"+argument+"'.");
        build_->expansion.push_back(definition.name);
        Parser child(tokens_,*build_,definition.body_begin,qualified(name.text)+".");
        for(const auto& port:definition.ports){
            if (port.sample) {
                if (args.contains(port.name)) {
                    if (!args.at(port.name).resource) fail(args.at(port.name).span,"Sample ports require a constant sample path.");
                    child.resources_[port.name] = *args.at(port.name).resource;
                } else if (port.resource) child.resources_[port.name] = *port.resource;
                else fail(name.span,"Missing class port '"+port.name+"'.");
                child.occupied_.insert(port.name);
                continue;
            }
            Expr value;
            if(args.contains(port.name)){if(!args.at(port.name).value)fail(args.at(port.name).span,"Class ports require signal values.");value=*args.at(port.name).value;}
            else if(definition.knobs.contains(port.name)&&definition.knobs.at(port.name).value){const auto& knob=definition.knobs.at(port.name);child.binding_context_=port.name+".default";child.literal_index_=0;value=child.constant(knob.value->value,knob.value->unit,knob.value_span,true);}
            else if(port.value){child.binding_context_=port.name+".default";child.literal_index_=0;value=child.constant(port.value->value,port.value->unit,port.span,true);}
            else fail(name.span,"Missing class port '"+port.name+"'.");
            require_unit(value,port.unit,port.name);if(definition.knobs.contains(port.name))register_knob(value,port.name,definition.knobs.at(port.name));
            if (port.value) declare_default(value,*port.value);
            // A required outer port has no omitted-argument value to restore.
            if (value.literal) build_->defaults.insert(value.id);
            child.values_[port.name]=value;child.occupied_.insert(port.name);
        }
        Expr result=child.block();if(child.position_!=definition.body_end)fail(name.span,"Invalid class signal body.");
        const auto returned=qualified(name.text)+".$output";
        for (auto& choice : program().choices) if (choice.owner==returned) choice.owner=choice_owner();
        build_->expansion.pop_back();return result;
    }
    std::shared_ptr<const std::vector<Token>> tokens_;
    Build owned_; Build* build_;
    std::size_t position_=0,expression_depth_=0,literal_index_=0,conditional_index_=0;
    std::string prefix_,key_alias_,binding_context_="$literal",branch_scope_;
    std::map<std::string,Expr> values_;
    std::map<std::string,ResourceRef> resources_;
    std::map<std::string,Instance> instances_;
    std::map<std::string,VoicePolicy> policies_;
    std::map<std::string,std::vector<std::size_t>> policy_choices_;
    std::set<std::string> occupied_;
    unsigned branch_depth_=0;bool analysis_context_=false;
};

std::string json_string(std::string_view text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                else out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}
void write_span(std::ostream& out, SourceSpan span) {
    out << "{\"byte_offset\": " << span.offset << ", \"byte_length\": " << span.length
        << ", \"line\": " << span.line << ", \"byte_column\": " << span.column << '}';
}
} // namespace

CompileResult compile(std::string_view source, std::string source_path) {
    try {
        if (source.size() > 1024 * 1024) fail({}, "Source limit is 1 MiB.");
        return {Parser(Lexer(source).run(), std::move(source_path)).run(), {}};
    } catch (const Failure& failure) {
        return {std::nullopt, {failure.diagnostic}};
    }
}

std::string_view op_name(Op op) {
    switch (op) {
        case Op::parameter: return "parameter";
        case Op::key_pitch: return "key_pitch";
        case Op::key_gate: return "key_gate";
        case Op::key_velocity: return "key_velocity";
        case Op::pitch_to_hz: return "pitch_to_hz";
        case Op::db_to_linear: return "db_to_linear";
        case Op::sine: return "sine";
        case Op::add: return "add";
        case Op::subtract: return "subtract";
        case Op::multiply: return "multiply";
        case Op::divide: return "divide";
        case Op::key_trigger: return "key_trigger";
        case Op::saw: return "saw";
        case Op::adsr: return "adsr";
        case Op::filter_lowpass: return "filter_lowpass";
        case Op::filter_highpass: return "filter_highpass";
        case Op::less: return "less";
        case Op::less_equal: return "less_equal";
        case Op::greater: return "greater";
        case Op::greater_equal: return "greater_equal";
        case Op::equal: return "equal";
        case Op::not_equal: return "not_equal";
        case Op::select: return "select";
        case Op::unison_sine: return "unison_sine";
        case Op::unison_saw: return "unison_saw";
        case Op::lfo: return "lfo";
        case Op::distortion: return "distortion";
        case Op::stereo: return "stereo";
        case Op::left: return "left";
        case Op::right: return "right";
        case Op::mid: return "mid";
        case Op::side: return "side";
        case Op::sampler: return "sampler";
        case Op::phasor: return "phasor";
        case Op::math_sin: return "sin";
        case Op::math_cos: return "cos";
    }
    return "unknown";
}
std::string_view unit_name(Unit unit) {
    switch (unit) {
        case Unit::scalar: return "scalar";
        case Unit::pitch: return "pitch";
        case Unit::hertz: return "hertz";
        case Unit::decibels: return "decibels";
        case Unit::seconds: return "seconds";
        case Unit::boolean: return "boolean";
    }
    return "unknown";
}
std::string_view rate_name(Rate rate) {
    switch (rate) {
        case Rate::constant: return "constant";
        case Rate::event_held: return "event_held";
        case Rate::sample_varying: return "sample_varying";
    }
    return "unknown";
}

namespace {
std::string_view state_name(StateKind kind) {
    switch(kind) {case StateKind::oscillator:return "oscillator";case StateKind::envelope:return "envelope";case StateKind::filter:return "filter";case StateKind::lfo:return "lfo";case StateKind::distortion:return "distortion";case StateKind::sampler:return "sampler";}
    return "unknown";
}
std::string_view completion_name(CompletionOp op) {
    switch(op) {case CompletionOp::gate_closed:return "gate_closed";case CompletionOp::envelope_idle:return "envelope_idle";case CompletionOp::filter_settled:return "filter_settled";case CompletionOp::all:return "all";case CompletionOp::any:return "any";case CompletionOp::distortion_settled:return "distortion_settled";case CompletionOp::sample_done:return "sample_done";}
    return "unknown";
}
std::string_view analysis_name(AnalysisOp op) {
    switch(op) {case AnalysisOp::probe:return "probe";case AnalysisOp::constant:return "constant";case AnalysisOp::add:return "add";case AnalysisOp::subtract:return "subtract";case AnalysisOp::multiply:return "multiply";case AnalysisOp::divide:return "divide";case AnalysisOp::power:return "power";case AnalysisOp::spectrum:return "spectrum";case AnalysisOp::mel_spectrum:return "mel_spectrum";}
    return "unknown";
}
std::string_view shape_name(AnalysisShape shape) {
    switch(shape) {case AnalysisShape::waveform:return "waveform";case AnalysisShape::scalar:return "scalar";case AnalysisShape::spectrum:return "spectrum";case AnalysisShape::mel_spectrum:return "mel_spectrum";}
    return "unknown";
}
void ids(std::ostream& out,const std::vector<std::uint32_t>& values) {
    out << '[';for(std::size_t i=0;i<values.size();++i){if(i)out<<", ";out<<values[i];}out<<']';
}
}

std::string program_json(const Program& program) {
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<double>::max_digits10)<<std::boolalpha;
    out<<"{\n  \"schema_version\": 2,\n  \"status\": \"compiled\",\n  \"language_version\": \"1.0\",\n  \"source_path\": "<<json_string(program.source_path)
       <<",\n  \"voice_policy\": {\"mode\": "<<json_string(program.voice_policy.mode==VoiceMode::mono?"mono":"poly")
       <<", \"priority\": \"last\", \"retrigger\": "<<json_string(program.voice_policy.retrigger==Retrigger::each_press?"each_press":"gate_rise")
       <<", \"glide_seconds\": "<<program.voice_policy.glide_seconds<<"},\n  \"nodes\": [\n";
    for(std::size_t i=0;i<program.nodes.size();++i){
        const auto& node=program.nodes[i];out<<"    {\"value_id\": "<<i<<", \"op\": "<<json_string(op_name(node.op))<<", \"unit\": "<<json_string(unit_name(node.unit))<<", \"rate\": "<<json_string(rate_name(node.rate))<<", \"inputs\": ";ids(out,node.inputs);
        if(node.op==Op::parameter)out<<", \"initial_value\": "<<node.initial_value;
        if(node.op==Op::sine||node.op==Op::saw||node.op==Op::unison_sine||node.op==Op::unison_saw||node.op==Op::lfo||node.op==Op::distortion||node.op==Op::adsr||node.op==Op::filter_lowpass||node.op==Op::filter_highpass||node.op==Op::sampler||node.op==Op::phasor)out<<", \"state_index\": "<<node.state_index;
        if(node.op==Op::sampler)out<<", \"resource_index\": "<<node.resource_index;
        out<<", \"channels\": "<<node.channels<<", \"flags\": "<<node.flags<<", \"source_span\": ";write_span(out,node.span);out<<'}'<<(i+1==program.nodes.size()?"\n":",\n");
    }
    out<<"  ],\n  \"states\": [\n";
    for(std::size_t i=0;i<program.states.size();++i){const auto& state=program.states[i];out<<"    {\"state_index\": "<<i<<", \"name\": "<<json_string(state.name)<<", \"scope\": \"per_voice\", \"kind\": "<<json_string(state_name(state.kind))<<", \"source_span\": ";write_span(out,state.span);out<<'}'<<(i+1==program.states.size()?"\n":",\n");}
    out<<"  ],\n  \"named_values\": [\n";
    for(std::size_t i=0;i<program.named_values.size();++i){const auto& value=program.named_values[i];out<<"    {\"name\": "<<json_string(value.name)<<", \"value\": "<<value.value<<", \"source_span\": ";write_span(out,value.span);out<<'}'<<(i+1==program.named_values.size()?"\n":",\n");}
    out<<"  ],\n  \"parameters\": [\n";
    for(std::size_t i=0;i<program.parameters.size();++i){const auto& value=program.parameters[i];out<<"    {\"value\": "<<value.value<<", \"binding_id\": "<<json_string(value.binding_id)<<", \"name\": "<<json_string(value.name)<<", \"unit\": "<<json_string(unit_name(value.unit))<<", \"min\": "<<value.min<<", \"max\": "<<value.max<<", \"step\": "<<value.step<<", \"logarithmic\": "<<value.logarithmic<<", \"control_scale\": "<<json_string(value.control_scale)<<", \"control_offset\": "<<value.control_offset<<", \"smoothing_seconds\": "<<value.smoothing_seconds<<", \"live\": "<<value.live<<", \"has_range\": "<<value.has_range<<", \"default_value\": "<<value.default_value<<", \"has_default\": "<<value.has_default<<", \"source_span\": ";write_span(out,value.span);out<<'}'<<(i+1==program.parameters.size()?"\n":",\n");}
    out<<"  ],\n  \"choices\": [\n";
    for(std::size_t i=0;i<program.choices.size();++i){
        const auto& choice=program.choices[i];
        out<<"    {\"binding_id\": "<<json_string(choice.binding_id)<<", \"name\": "<<json_string(choice.name)<<", \"selected\": "<<json_string(choice.selected)
           <<", \"owner\": "<<json_string(choice.owner)<<", \"implicit\": "<<choice.implicit<<", \"prefix\": "<<json_string(choice.prefix)<<", \"suffix\": "<<json_string(choice.suffix)<<", \"options\": [";
        for(std::size_t j=0;j<choice.options.size();++j){const auto& option=choice.options[j];if(j)out<<", ";out<<"{\"value\": "<<json_string(option.value)<<", \"enabled\": "<<option.enabled<<", \"reason\": "<<json_string(option.reason)<<'}';}
        out<<"], \"source_span\": ";write_span(out,choice.span);out<<'}'<<(i+1==program.choices.size()?"\n":",\n");
    }
    out<<"  ],\n  \"completion\": [\n";
    for(std::size_t i=0;i<program.completion.size();++i){const auto& value=program.completion[i];out<<"    {\"id\": "<<i<<", \"op\": "<<json_string(completion_name(value.op))<<", \"state_index\": "<<value.state_index<<", \"inputs\": ";ids(out,value.inputs);out<<'}'<<(i+1==program.completion.size()?"\n":",\n");}
    out<<"  ],\n  \"completion_root\": ";if(program.completion_root)out<<*program.completion_root;else out<<"null";
    out<<",\n  \"analysis\": [\n";
    for(std::size_t i=0;i<program.analysis.size();++i){const auto& value=program.analysis[i];out<<"    {\"id\": "<<i<<", \"op\": "<<json_string(analysis_name(value.op))<<", \"shape\": "<<json_string(shape_name(value.shape))<<", \"unit\": "<<json_string(unit_name(value.unit))<<", \"inputs\": ";ids(out,value.inputs);if(value.signal!=invalid_value)out<<", \"signal\": "<<value.signal;if(value.op==AnalysisOp::constant)out<<", \"initial_value\": "<<value.initial_value;out<<", \"source_span\": ";write_span(out,value.span);out<<'}'<<(i+1==program.analysis.size()?"\n":",\n");}
    out<<"  ],\n  \"named_analysis\": [\n";
    for(std::size_t i=0;i<program.named_analysis.size();++i){const auto& value=program.named_analysis[i];out<<"    {\"name\": "<<json_string(value.name)<<", \"value\": "<<value.value<<", \"source_span\": ";write_span(out,value.span);out<<'}'<<(i+1==program.named_analysis.size()?"\n":",\n");}
    out<<"  ],\n  \"samples\": [\n";
    for(std::size_t i=0;i<program.samples.size();++i){const auto& value=program.samples[i];out<<"    {\"resource_index\": "<<i<<", \"path\": "<<json_string(value.path)<<", \"source_span\": ";write_span(out,value.span);out<<'}'<<(i+1==program.samples.size()?"\n":",\n");}
    out<<"  ],\n  \"output\": "<<program.output<<"\n}\n";return out.str();
}

std::string dsp_structure_key(const Program& program) {
    Program structural=program;structural.source_path.clear();structural.named_values.clear();structural.named_analysis.clear();structural.analysis.clear();structural.choices.clear();
    for(auto& node:structural.nodes)node.span={};
    for(auto& state:structural.states)state.span={};
    for(auto& sample:structural.samples)sample.span={};
    for(auto& parameter:structural.parameters){parameter.span={};if(parameter.live){parameter.default_value=0;parameter.has_default=false;if(parameter.value<structural.nodes.size())structural.nodes[parameter.value].initial_value=0;}}
    return "flows-native-dsp-v1:"+program_json(structural);
}
} // namespace flows
