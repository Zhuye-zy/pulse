#include "math_macro.h"

#include <algorithm>
#include <cwctype>
#include <deque>
#include <unordered_map>
#include <utility>

namespace pulse::ui {
namespace {
using Error = MathParseErrorCode;
enum class TokenKind { Character, Command, Space, Open, Close };
struct Token {
    TokenKind kind = TokenKind::Character;
    std::wstring text;
    MathSourceOrigin origin;
    unsigned depth = 0;
};
bool Letter(wchar_t ch) { return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'); }
bool White(wchar_t ch) { return iswspace(ch) != 0; }
bool Character(const Token& token, wchar_t value) {
    return token.kind == TokenKind::Character && token.text.size() == 1 && token.text[0] == value;
}
struct Macro {
    unsigned parameters = 0;
    bool optional = false;
    std::vector<Token> default_value;
    std::vector<Token> body;
};
enum class ScopeKind { Root, Group, Environment, Substack, Cell, Fence };
struct Scope {
    ScopeKind kind = ScopeKind::Root;
    std::unordered_map<std::wstring, Macro> macros;
};

class Expander {
public:
    Expander(std::wstring_view source, bool (*is_builtin)(std::wstring_view))
        : source_(source), is_builtin_(is_builtin) {}
    MathMacroExpansion Run() {
        if (source_.size() > 4096) {
            Fail(Error::SourceLimit, 4096);
            return std::move(result_);
        }
        Lex();
        scopes_.push_back({});
        while (Good() && !pending_.empty()) {
            Token token = Take();
            if (token.kind == TokenKind::Command) {
                if (token.text == L"\\newcommand" || token.text == L"\\renewcommand" ||
                    token.text == L"\\providecommand") {
                    Define(token);
                    continue;
                }
                if (const Macro* macro = Find(token.text)) {
                    // Expansion can define macros and rehash a scope later, so no
                    // pointer into the scope table survives the current call.
                    Invoke(token, *macro);
                    continue;
                }
            }
            if (awaiting_substack_ && token.kind != TokenKind::Open && token.kind != TokenKind::Space) {
                Fail(Error::InvalidArgument, token.origin.start);
                break;
            }
            if (token.kind == TokenKind::Command) {
                if (token.text == L"\\begin") {
                    Push(ScopeKind::Environment, token.origin.start);
                    Push(ScopeKind::Cell, token.origin.start);
                } else if (token.text == L"\\end") {
                    Pop(ScopeKind::Cell, token.origin.start);
                    Pop(ScopeKind::Environment, token.origin.start);
                } else if (token.text == L"\\left")
                    Push(ScopeKind::Fence, token.origin.start);
                else if (token.text == L"\\right")
                    Pop(ScopeKind::Fence, token.origin.start);
                else if (token.text == L"\\\\")
                    NextCell(true);
                else if (token.text == L"\\substack")
                    awaiting_substack_ = true;
            } else if (token.kind == TokenKind::Open) {
                if (awaiting_substack_) {
                    Push(ScopeKind::Substack, token.origin.start);
                    Push(ScopeKind::Cell, token.origin.start);
                    awaiting_substack_ = false;
                } else
                    Push(ScopeKind::Group, token.origin.start);
            } else if (token.kind == TokenKind::Close) {
                if (scopes_.size() > 2 && scopes_.back().kind == ScopeKind::Cell &&
                    scopes_[scopes_.size() - 2].kind == ScopeKind::Substack) {
                    Pop(ScopeKind::Cell, token.origin.start);
                    Pop(ScopeKind::Substack, token.origin.start);
                } else
                    Pop(ScopeKind::Group, token.origin.start);
            } else if (Character(token, L'&'))
                NextCell(false);
            Emit(token);
        }
        if (Good() && scopes_.size() != 1)
            Fail(Error::UnexpectedEnd, source_.size());
        return std::move(result_);
    }

private:
    std::wstring_view source_;
    bool (*is_builtin_)(std::wstring_view);
    MathMacroExpansion result_;
    std::deque<Token> pending_;
    std::vector<Scope> scopes_;
    size_t pending_units_ = 0;
    unsigned definitions_ = 0, calls_ = 0;
    bool last_control_word_ = false;
    bool awaiting_substack_ = false;

    bool Good() const { return result_.error.code == Error::None; }
    void Fail(Error code, size_t offset) {
        if (Good())
            result_.error = {code, std::min(offset, source_.size())};
    }
    void Lex() {
        for (size_t i = 0; i < source_.size();) {
            const size_t start = i;
            Token token;
            const wchar_t ch = source_[i++];
            if (ch == L'\\') {
                token.kind = TokenKind::Command;
                if (i < source_.size() && iswalpha(source_[i])) {
                    while (i < source_.size() && iswalpha(source_[i]))
                        ++i;
                } else if (i < source_.size())
                    ++i;
            } else if (ch == L'{')
                token.kind = TokenKind::Open;
            else if (ch == L'}')
                token.kind = TokenKind::Close;
            else if (White(ch))
                token.kind = TokenKind::Space;
            else if (ch >= 0xd800 && ch <= 0xdbff && i < source_.size() &&
                     source_[i] >= 0xdc00 && source_[i] <= 0xdfff)
                ++i;
            token.text = source_.substr(start, i - start);
            token.origin = {start, i - start};
            pending_units_ += token.text.size();
            const bool control_word = token.kind == TokenKind::Command && token.text.size() > 1 &&
                                      iswalpha(token.text.back());
            pending_.push_back(std::move(token));
            if (control_word)
                while (i < source_.size() && White(source_[i]))
                    ++i;
        }
    }
    Token Take() {
        Token token = std::move(pending_.front());
        pending_.pop_front();
        pending_units_ -= token.text.size();
        return token;
    }
    void Spaces() {
        while (!pending_.empty() && pending_.front().kind == TokenKind::Space)
            Take();
    }
    const Macro* Find(const std::wstring& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->macros.find(name);
            if (found != scope->macros.end())
                return &found->second;
        }
        return nullptr;
    }
    void Push(ScopeKind kind, size_t offset) {
        if (!Good())
            return;
        const auto depth = std::count_if(scopes_.begin(), scopes_.end(),
                                        [](const Scope& scope) { return scope.kind != ScopeKind::Cell; });
        if (kind != ScopeKind::Cell && depth >= 33) {
            Fail(Error::DepthLimit, offset);
            return;
        }
        scopes_.push_back({kind, {}});
    }
    void NextCell(bool row) {
        // Alignment cells are local TeX groups. Only direct separators reset
        // them; separators inside an explicit group or nested fence do not.
        if (Good() && scopes_.back().kind == ScopeKind::Cell &&
            (row || scopes_[scopes_.size() - 2].kind != ScopeKind::Substack))
            scopes_.back().macros.clear();
    }
    void Pop(ScopeKind kind, size_t offset) {
        if (scopes_.size() == 1 || scopes_.back().kind != kind)
            Fail(Error::UnexpectedToken, offset);
        else
            scopes_.pop_back();
    }
    std::vector<Token> Group(size_t offset, MathSourceOrigin* consumed = nullptr) {
        Spaces();
        std::vector<Token> tokens;
        if (pending_.empty() || pending_.front().kind != TokenKind::Open) {
            Fail(Error::InvalidArgument, offset);
            return tokens;
        }
        Token open = Take();
        unsigned depth = 1;
        while (Good() && !pending_.empty()) {
            Token token = Take();
            if (token.kind == TokenKind::Open) {
                if (++depth > 32)
                    Fail(Error::DepthLimit, token.origin.start);
            } else if (token.kind == TokenKind::Close && --depth == 0) {
                if (consumed) {
                    const size_t start = std::min(open.origin.start, token.origin.start);
                    const size_t end = std::max(open.origin.start + open.origin.length,
                                               token.origin.start + token.origin.length);
                    *consumed = {start, end - start};
                }
                return tokens;
            }
            tokens.push_back(std::move(token));
        }
        Fail(Error::UnexpectedEnd, offset);
        return tokens;
    }
    std::vector<Token> Optional(size_t offset, MathSourceOrigin* consumed = nullptr) {
        std::vector<Token> tokens;
        Token open = Take(); // Caller observed '[' after discarding delimiter spaces.
        unsigned groups = 0;
        while (Good() && !pending_.empty()) {
            Token token = Take();
            if (token.kind == TokenKind::Open) {
                if (++groups > 32)
                    Fail(Error::DepthLimit, token.origin.start);
            } else if (token.kind == TokenKind::Close) {
                if (groups == 0) {
                    Fail(Error::InvalidArgument, token.origin.start);
                    break;
                }
                --groups;
            } else if (groups == 0 && Character(token, L']')) {
                if (consumed) {
                    const size_t start = std::min(open.origin.start, token.origin.start);
                    const size_t end = std::max(open.origin.start + open.origin.length,
                                               token.origin.start + token.origin.length);
                    *consumed = {start, end - start};
                }
                return tokens;
            }
            tokens.push_back(std::move(token));
        }
        Fail(Error::UnexpectedEnd, offset);
        return tokens;
    }
    bool Parameters(const std::vector<Token>& tokens, unsigned parameters, size_t offset) {
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!Character(tokens[i], L'#'))
                continue;
            if (++i == tokens.size() || tokens[i].kind != TokenKind::Character || tokens[i].text.size() != 1 ||
                tokens[i].text[0] < L'1' || tokens[i].text[0] > L'9' ||
                static_cast<unsigned>(tokens[i].text[0] - L'0') > parameters) {
                Fail(Error::InvalidArgument, offset);
                return false;
            }
        }
        return true;
    }
    void Define(const Token& instruction) {
        if (++definitions_ > 64) {
            Fail(Error::NodeLimit, instruction.origin.start);
            return;
        }
        Spaces();
        std::vector<Token> name;
        if (!pending_.empty() && pending_.front().kind == TokenKind::Open)
            name = Group(instruction.origin.start);
        else if (!pending_.empty())
            name.push_back(Take());
        name.erase(std::remove_if(name.begin(), name.end(), [](const Token& t) { return t.kind == TokenKind::Space; }),
                   name.end());
        if (name.size() != 1 || name.front().kind != TokenKind::Command || name.front().text.size() < 2 ||
            !std::all_of(name.front().text.begin() + 1, name.front().text.end(), Letter)) {
            Fail(Error::InvalidArgument, instruction.origin.start);
            return;
        }
        Macro macro;
        Spaces();
        if (!pending_.empty() && Character(pending_.front(), L'[')) {
            auto count = Optional(instruction.origin.start);
            count.erase(std::remove_if(count.begin(), count.end(), [](const Token& t) { return t.kind == TokenKind::Space; }),
                        count.end());
            if (count.size() != 1 || count.front().kind != TokenKind::Character || count.front().text.size() != 1 ||
                count.front().text[0] < L'0' || count.front().text[0] > L'9') {
                Fail(Error::InvalidArgument, instruction.origin.start);
                return;
            }
            macro.parameters = static_cast<unsigned>(count.front().text[0] - L'0');
            Spaces();
            if (!pending_.empty() && Character(pending_.front(), L'[')) {
                macro.optional = true;
                macro.default_value = Optional(instruction.origin.start);
                if (macro.parameters == 0 || !Parameters(macro.default_value, 0, instruction.origin.start)) {
                    Fail(Error::InvalidArgument, instruction.origin.start);
                    return;
                }
            }
        }
        macro.body = Group(instruction.origin.start);
        if (!Good() || !Parameters(macro.body, macro.parameters, instruction.origin.start))
            return;
        const std::wstring& key = name.front().text;
        const bool builtin = is_builtin_(std::wstring_view(key).substr(1));
        const bool exists = builtin || Find(key) != nullptr;
        if (instruction.text == L"\\providecommand" && exists)
            return;
        if (builtin || (instruction.text == L"\\newcommand" && exists) ||
            (instruction.text == L"\\renewcommand" && !exists)) {
            Fail(Error::InvalidArgument, instruction.origin.start);
            return;
        }
        scopes_.back().macros[key] = std::move(macro);
    }
    void Invoke(const Token& invocation, const Macro& macro) {
        if (++calls_ > 256) {
            Fail(Error::NodeLimit, invocation.origin.start);
            return;
        }
        if (invocation.depth >= 32) {
            Fail(Error::DepthLimit, invocation.origin.start);
            return;
        }
        MathSourceOrigin call = invocation.origin;
        const auto include = [&](MathSourceOrigin origin) {
            const size_t start = std::min(call.start, origin.start);
            const size_t end = std::max(call.start + call.length, origin.start + origin.length);
            call = {start, end - start};
        };
        std::vector<std::vector<Token>> arguments(macro.parameters);
        for (unsigned i = 0; i < macro.parameters && Good(); ++i) {
            Spaces();
            MathSourceOrigin consumed = call;
            if (i == 0 && macro.optional) {
                if (!pending_.empty() && Character(pending_.front(), L'[')) {
                    arguments[i] = Optional(invocation.origin.start, &consumed);
                    include(consumed);
                } else {
                    arguments[i] = macro.default_value;
                    for (auto& token : arguments[i])
                        token.origin = call;
                }
            } else if (!pending_.empty() && pending_.front().kind == TokenKind::Open) {
                arguments[i] = Group(invocation.origin.start, &consumed);
                include(consumed);
            } else if (!pending_.empty() && pending_.front().kind != TokenKind::Close) {
                arguments[i].push_back(Take());
                include(arguments[i].back().origin);
            } else
                Fail(Error::InvalidArgument, invocation.origin.start);
        }
        std::vector<Token> replacement;
        size_t units = 0;
        const auto append = [&](Token token) {
            token.depth = std::max(token.depth, invocation.depth + 1);
            units += token.text.size();
            if (units + pending_units_ + result_.source.size() > 4096)
                Fail(Error::SourceLimit, invocation.origin.start);
            else
                replacement.push_back(std::move(token));
        };
        for (size_t i = 0; Good() && i < macro.body.size(); ++i) {
            if (Character(macro.body[i], L'#')) {
                const unsigned parameter = static_cast<unsigned>(macro.body[++i].text[0] - L'1');
                for (const auto& token : arguments[parameter]) {
                    append(token);
                    if (!Good())
                        break;
                }
            } else {
                Token token = macro.body[i];
                token.origin = call;
                append(std::move(token));
            }
        }
        if (!Good())
            return;
        for (auto token = replacement.rbegin(); token != replacement.rend(); ++token)
            pending_.push_front(std::move(*token));
        pending_units_ += units;
    }
    void Emit(const Token& token) {
        // A space terminates a control word without emitting a TeX math atom.
        // Ordinary character adjacency (including expanded text) stays intact.
        const bool separator = last_control_word_ && !token.text.empty() && iswalpha(token.text.front());
        if (result_.source.size() + token.text.size() + (separator ? 1 : 0) > 4096) {
            Fail(Error::SourceLimit, token.origin.start);
            return;
        }
        if (separator) {
            result_.source.push_back(L' ');
            result_.origins.push_back(token.origin);
        }
        result_.source += token.text;
        result_.origins.insert(result_.origins.end(), token.text.size(), token.origin);
        last_control_word_ = token.kind == TokenKind::Command && token.text.size() > 1 && iswalpha(token.text.back());
    }
};
} // namespace

MathMacroExpansion ExpandMathMacros(std::wstring_view source, bool (*is_builtin)(std::wstring_view)) {
    return Expander(source, is_builtin).Run();
}

} // namespace pulse::ui
