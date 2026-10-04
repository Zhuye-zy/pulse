// markdown_document.cpp — see markdown_document.h.
#include "markdown_document.h"
#include "../ipc/preview_protocol.h"
#include <windows.h>
#include "../../third_party/md4c/md4c.h"
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

namespace pulse::preview {
namespace {

enum : unsigned {
    kBold = 1, kItalic = 2, kCode = 4, kStrike = 8, kLink = 16, kImage = 32,
    kUnderline = 64, kMath = 128, kDisplayMath = 256
};

void AppendEscaped(std::wstring& out, const wchar_t* text, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const wchar_t c = text[i];
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c != L'\r') out += c;
    }
}

void AppendTarget(std::wstring& out, const std::wstring& target) {
    std::wstring encoded;
    for (wchar_t c : target) {
        if (c == L'%') encoded += L"%25";
        else if (c == L',') encoded += L"%2C";
        else if (c == L';') encoded += L"%3B";
        else encoded += c;
    }
    AppendEscaped(out, encoded.data(), encoded.size());
}

std::wstring AttributeText(const MD_ATTRIBUTE& attribute) {
    return attribute.text && attribute.size ? std::wstring(attribute.text, attribute.size) : std::wstring();
}

void AppendCodePoint(std::wstring& out, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out += L'\xFFFD'; return; }
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out += static_cast<wchar_t>(0xD800 + (cp >> 10));
        out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
    } else {
        out += static_cast<wchar_t>(cp);
    }
}

void AppendEntity(std::wstring& out, const wchar_t* text, size_t size) {
    const std::wstring e(text, size);
    if (e.size() > 3 && e[1] == L'#') {
        const bool hex = e[2] == L'x' || e[2] == L'X';
        const unsigned long cp = wcstoul(e.c_str() + (hex ? 3 : 2), nullptr, hex ? 16 : 10);
        AppendCodePoint(out, cp);
        return;
    }
    static const struct { const wchar_t* name; wchar_t ch; } kNamed[] = {
        {L"&amp;", L'&'}, {L"&lt;", L'<'}, {L"&gt;", L'>'}, {L"&quot;", L'"'}, {L"&apos;", L'\''},
        {L"&nbsp;", L'\xA0'}, {L"&copy;", L'\xA9'}, {L"&reg;", L'\xAE'}, {L"&trade;", L'\x2122'},
        {L"&hellip;", L'\x2026'}, {L"&mdash;", L'\x2014'}, {L"&ndash;", L'\x2013'},
        {L"&laquo;", L'\xAB'}, {L"&raquo;", L'\xBB'}, {L"&times;", L'\xD7'}, {L"&middot;", L'\xB7'},
    };
    for (const auto& n : kNamed) if (e == n.name) { out += n.ch; return; }
    out += e;
}

struct BackslashMath {
    size_t start, end;
    unsigned flags;
};

struct MathDiscovery {
    const std::wstring& source;
    std::vector<BackslashMath> ranges;
    size_t pending = std::wstring::npos, total = 0;
    wchar_t close = 0;
    int excluded = 0;

    bool HasQuoteContinuation(size_t start, size_t end) const {
        for (size_t i = start; i < end; ++i) {
            if (source[i] != L'\n' && source[i] != L'\r') continue;
            while (i + 1 < end && (source[i + 1] == L' ' || source[i + 1] == L'\t')) ++i;
            if (i + 1 < end && source[i + 1] == L'>') return true;
        }
        return false;
    }

    static int Block(MD_BLOCKTYPE, void*, void* user) {
        static_cast<MathDiscovery*>(user)->pending = std::wstring::npos;
        return 0;
    }
    static bool Excluded(MD_SPANTYPE type) {
        return type == MD_SPAN_CODE || type == MD_SPAN_LATEXMATH ||
               type == MD_SPAN_LATEXMATH_DISPLAY || type == MD_SPAN_IMG;
    }
    static int Enter(MD_SPANTYPE type, void*, void* user) {
        auto& d = *static_cast<MathDiscovery*>(user);
        if (type == MD_SPAN_A || type == MD_SPAN_WIKILINK) d.pending = std::wstring::npos;
        if (Excluded(type)) { ++d.excluded; d.pending = std::wstring::npos; }
        return 0;
    }
    static int Leave(MD_SPANTYPE type, void*, void* user) {
        auto& d = *static_cast<MathDiscovery*>(user);
        if (type == MD_SPAN_A || type == MD_SPAN_WIKILINK) d.pending = std::wstring::npos;
        if (Excluded(type)) { --d.excluded; d.pending = std::wstring::npos; }
        return 0;
    }
    static int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
        auto& d = *static_cast<MathDiscovery*>(user);
        if (type == MD_TEXT_HTML || type == MD_TEXT_CODE) d.pending = std::wstring::npos;
        if (type != MD_TEXT_NORMAL || d.excluded || d.ranges.size() >= 256) return 0;
        const uintptr_t address = reinterpret_cast<uintptr_t>(text);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(d.source.data());
        if (address < begin || address >= begin + d.source.size() * sizeof(wchar_t)) return 0;
        const size_t first = (address - begin) / sizeof(wchar_t);
        const size_t end = std::min(first + size, d.source.size());
        for (size_t i = first; i < end; ++i) {
            const wchar_t ch = d.source[i];
            if (!i || d.source[i - 1] != L'\\' ||
                (ch != L'(' && ch != L')' && ch != L'[' && ch != L']')) continue;
            size_t slash = i;
            while (slash && d.source[slash - 1] == L'\\') --slash;
            if ((i - slash) % 2 == 0) continue;
            if (ch == L'(' || ch == L'[') {
                d.pending = i - 1;
                d.close = ch == L'(' ? L')' : L']';
            } else if (d.pending != std::wstring::npos && ch == d.close) {
                const size_t length = i + 1 - d.pending;
                if (length <= 4096 && d.total + length <= 65536 && d.ranges.size() < 256 &&
                    !d.HasQuoteContinuation(d.pending, i + 1)) {
                    d.ranges.push_back({d.pending, i + 1, ch == L')' ? kMath : kDisplayMath});
                    d.total += length;
                }
                d.pending = std::wstring::npos;
            }
        }
        return 0;
    }
};

// Discover only delimiters md4c actually exposes as prose. The equal-length
// masked second parse protects formula syntax while retaining Markdown context.
std::vector<BackslashMath> PrepareBackslashMath(const std::wstring& source, std::wstring& masked) {
    if (source.find(L"\\(") == std::wstring::npos && source.find(L"\\[") == std::wstring::npos) return {};
    MathDiscovery d{source};
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = MathDiscovery::Block;
    parser.leave_block = MathDiscovery::Block;
    parser.enter_span = MathDiscovery::Enter;
    parser.leave_span = MathDiscovery::Leave;
    parser.text = MathDiscovery::Text;
    if (md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, &d) != 0 || d.ranges.empty()) return {};
    masked = source;
    for (const auto& range : d.ranges) {
        for (size_t i = range.start; i < range.end; ++i)
            if (masked[i] != L'\r' && masked[i] != L'\n' && masked[i] != L' ' && masked[i] != L'\t') masked[i] = L'x';
        // Unicode punctuation keeps emphasis flanking without creating a link
        // destination after a preceding ']' (ASCII parentheses would do that).
        masked[range.start] = masked[range.start + 1] = L'\xFF08';
        masked[range.end - 2] = masked[range.end - 1] = L'\xFF09';
    }
    return std::move(d.ranges);
}

struct Builder {
    struct Run { size_t start, length; unsigned flags; std::wstring target; };
    struct OpenSpan { size_t start; unsigned flags; std::wstring target; };
    struct List { bool ordered; unsigned next; };

    std::wstring out;
    const std::wstring* source = nullptr;
    const std::wstring* original = nullptr;
    std::vector<BackslashMath> backslash_math;
    size_t math_index = 0, skip_math_until = 0;
    size_t limit = 0;
    bool full = false;
    // Current leaf block.
    bool open = false;
    wchar_t kind = L'p';
    std::wstring arg, marker, text;
    std::vector<Run> runs;
    std::vector<OpenSpan> spans;
    // Context.
    int quote = 0;
    std::vector<List> lists;
    std::wstring pending_marker;

    void Record(const std::wstring& line) {
        if (full) return;
        if (out.size() + line.size() + 1 > limit) { full = true; return; }
        out += line;
        out += L'\n';
    }
    int base_indent = 0;  // notebook cells sit right of the In/Out gutter
    std::wstring Context() const {
        return std::to_wstring(quote) + L'\t' + std::to_wstring(lists.size() + static_cast<size_t>(base_indent));
    }
    void Open(wchar_t k, std::wstring a) {
        Flush();
        open = true;
        kind = k;
        arg = std::move(a);
        marker.swap(pending_marker);
        pending_marker.clear();
        text.clear();
        runs.clear();
        spans.clear();
    }
    void Flush() {
        if (!open) return;
        open = false;
        if (kind == L'c' || kind == L'x')
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
        if (kind == L'p' && runs.size() == 1 && runs.front().flags == kDisplayMath) {
            const Run& run = runs.front();
            const size_t first = text.find_first_not_of(L" \t\r\n");
            const size_t last = text.find_last_not_of(L" \t\r\n");
            if (first == run.start && last == run.start + run.length - 1) kind = L'm';
        }
        std::wstring line = L"B\t";
        line += kind;
        line += L'\t';
        AppendEscaped(line, arg.data(), arg.size());
        line += L'\t' + Context() + L'\t' + marker + L'\t';
        AppendEscaped(line, text.data(), text.size());
        line += L'\t';
        for (const Run& run : runs) {
            line += std::to_wstring(run.start) + L',' + std::to_wstring(run.length) + L',' +
                    std::to_wstring(run.flags);
            if (!run.target.empty()) { line += L','; AppendTarget(line, run.target); }
            line += L';';
        }
        Record(line);
    }
    void EnsureOpen() { if (!open) Open(L'p', L""); }  // tight list items carry no P block
};

int EnterBlock(MD_BLOCKTYPE type, void* detail, void* user) {
    Builder& b = *static_cast<Builder*>(user);
    switch (type) {
    case MD_BLOCK_QUOTE: b.Flush(); ++b.quote; break;
    case MD_BLOCK_UL: b.Flush(); b.lists.push_back({false, 1}); break;
    case MD_BLOCK_OL:
        b.Flush();
        b.lists.push_back({true, static_cast<const MD_BLOCK_OL_DETAIL*>(detail)->start});
        break;
    case MD_BLOCK_LI: {
        b.Flush();
        const auto* li = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
        if (li->is_task) b.pending_marker = li->task_mark == L' ' ? L"t0" : L"t1";
        else if (!b.lists.empty() && b.lists.back().ordered) b.pending_marker = L"o" + std::to_wstring(b.lists.back().next++);
        else b.pending_marker = L"u";
        break;
    }
    case MD_BLOCK_HR: b.Open(L'r', L""); b.Flush(); break;
    case MD_BLOCK_H:
        b.Open(L'h', std::to_wstring(static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level));
        break;
    case MD_BLOCK_CODE: b.Open(L'c', AttributeText(static_cast<const MD_BLOCK_CODE_DETAIL*>(detail)->lang)); break;
    case MD_BLOCK_HTML: b.Open(L'x', L""); break;
    case MD_BLOCK_P: b.Open(L'p', L""); break;
    case MD_BLOCK_TABLE:
        b.Flush();
        b.Record(L"T\t" + std::to_wstring(static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail)->col_count) +
                 L'\t' + b.Context());
        break;
    case MD_BLOCK_TR: b.Flush(); b.Record(L"R"); break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        const MD_ALIGN align = static_cast<const MD_BLOCK_TD_DETAIL*>(detail)->align;
        b.Open(L't', align == MD_ALIGN_LEFT ? L"l" : align == MD_ALIGN_CENTER ? L"c"
                   : align == MD_ALIGN_RIGHT ? L"r" : L"-");
        b.marker = type == MD_BLOCK_TH ? L"h" : L"";
        break;
    }
    default: break;
    }
    return 0;
}

int LeaveBlock(MD_BLOCKTYPE type, void*, void* user) {
    Builder& b = *static_cast<Builder*>(user);
    switch (type) {
    case MD_BLOCK_QUOTE: b.Flush(); if (b.quote > 0) --b.quote; break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL: b.Flush(); if (!b.lists.empty()) b.lists.pop_back(); b.pending_marker.clear(); break;
    case MD_BLOCK_LI: b.Flush(); b.pending_marker.clear(); break;
    case MD_BLOCK_TABLE: b.Flush(); b.Record(L"E"); break;
    case MD_BLOCK_H: case MD_BLOCK_CODE: case MD_BLOCK_HTML: case MD_BLOCK_P:
    case MD_BLOCK_TH: case MD_BLOCK_TD: b.Flush(); break;
    default: break;
    }
    return 0;
}

int EnterSpan(MD_SPANTYPE type, void* detail, void* user) {
    Builder& b = *static_cast<Builder*>(user);
    b.EnsureOpen();
    Builder::OpenSpan span{b.text.size(), 0, {}};
    switch (type) {
    case MD_SPAN_EM: span.flags = kItalic; break;
    case MD_SPAN_STRONG: span.flags = kBold; break;
    case MD_SPAN_A:
        span.flags = kLink;
        span.target = AttributeText(static_cast<const MD_SPAN_A_DETAIL*>(detail)->href);
        break;
    case MD_SPAN_IMG:
        span.flags = kImage;
        span.target = AttributeText(static_cast<const MD_SPAN_IMG_DETAIL*>(detail)->src);
        break;
    case MD_SPAN_CODE: span.flags = kCode; break;
    case MD_SPAN_LATEXMATH: span.flags = kMath; b.text += L'$'; break;
    case MD_SPAN_LATEXMATH_DISPLAY: span.flags = kDisplayMath; b.text += L"$$"; break;
    case MD_SPAN_DEL: span.flags = kStrike; break;
    case MD_SPAN_U: span.flags = kUnderline; break;
    case MD_SPAN_WIKILINK:
        span.flags = kLink;
        span.target = AttributeText(static_cast<const MD_SPAN_WIKILINK_DETAIL*>(detail)->target);
        break;
    default: break;
    }
    b.spans.push_back(std::move(span));
    return 0;
}

int LeaveSpan(MD_SPANTYPE, void*, void* user) {
    Builder& b = *static_cast<Builder*>(user);
    if (b.spans.empty()) return 0;
    Builder::OpenSpan span = std::move(b.spans.back());
    b.spans.pop_back();
    if (span.flags & kMath) b.text += L'$';
    if (span.flags & kDisplayMath) b.text += L"$$";
    if ((span.flags & kImage) && b.text.size() == span.start) b.text += L"image";
    if (span.flags && b.text.size() > span.start)
        b.runs.push_back({span.start, b.text.size() - span.start, span.flags, std::move(span.target)});
    return 0;
}

int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user) {
    Builder& b = *static_cast<Builder*>(user);
    b.EnsureOpen();
    if (!b.backslash_math.empty()) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(text);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(b.source->data());
        if (address >= begin && address < begin + b.source->size() * sizeof(wchar_t)) {
            size_t offset = (address - begin) / sizeof(wchar_t);
            const size_t end = offset + size;
            if (b.skip_math_until) {
                offset = std::min(end, std::max(offset, b.skip_math_until));
                if (end >= b.skip_math_until) b.skip_math_until = 0;
            }
            if (type == MD_TEXT_NORMAL) {
                while (b.math_index < b.backslash_math.size() && b.backslash_math[b.math_index].start < end) {
                    const auto& range = b.backslash_math[b.math_index++];
                    if (range.start < offset) continue;
                    b.text.append(b.source->data() + offset, range.start - offset);
                    const size_t start = b.text.size();
                    b.text.append(*b.original, range.start, range.end - range.start);
                    b.runs.push_back({start, range.end - range.start, range.flags, {}});
                    offset = std::min(end, range.end);
                    if (range.end > end) b.skip_math_until = range.end;
                }
            }
            text = b.source->data() + offset;
            size = static_cast<MD_SIZE>(end - offset);
            if (!size) return 0;
        } else if (b.skip_math_until) return 0;
    }
    switch (type) {
    case MD_TEXT_LATEXMATH: {
        // md4c substitutes a static space for a math span's line break.
        // Keep the LaTeX line break for copy/search and unsupported-formula fallback.
        const uintptr_t address = reinterpret_cast<uintptr_t>(text);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(b.source->data());
        const uintptr_t end = begin + b.source->size() * sizeof(wchar_t);
        if (size == 1 && text[0] == L' ' && (address < begin || address >= end))
            b.text += L'\n';
        else
            b.text.append(text, size);
        break;
    }
    case MD_TEXT_NULLCHAR: b.text += L'\xFFFD'; break;
    case MD_TEXT_BR: b.text += L'\n'; break;
    case MD_TEXT_SOFTBR:
        // CJK prose wraps lines without spaces between them.
        if (b.text.empty() || b.text.back() < 0x2E80) b.text += L' ';
        break;
    case MD_TEXT_ENTITY: AppendEntity(b.text, text, size); break;
    default: b.text.append(text, size); break;
    }
    return 0;
}

} // namespace

bool AppendMarkdownBlocks(const std::wstring& markdown, std::wstring& payload, size_t limit,
                          int base_indent) {
    Builder b;
    std::wstring masked;
    b.backslash_math = PrepareBackslashMath(markdown, masked);
    b.source = masked.empty() ? &markdown : &masked;
    b.original = &markdown;
    b.out.swap(payload);
    b.limit = limit;
    b.base_indent = base_indent;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = EnterBlock;
    parser.leave_block = LeaveBlock;
    parser.enter_span = EnterSpan;
    parser.leave_span = LeaveSpan;
    parser.text = Text;
    const bool parsed = md_parse(b.source->data(), static_cast<MD_SIZE>(b.source->size()), &parser, &b) == 0;
    b.Flush();
    payload.swap(b.out);
    return parsed && !b.full;
}

bool AppendMarkdownBlock(std::wstring& payload, size_t limit, wchar_t kind, const std::wstring& arg,
                         int indent, const std::wstring& marker, const std::wstring& text,
                         const std::wstring& image_target) {
    std::wstring line = L"B\t";
    line += kind;
    line += L'\t';
    AppendEscaped(line, arg.data(), arg.size());
    line += L"\t0\t" + std::to_wstring(indent) + L'\t' + marker + L'\t';
    AppendEscaped(line, text.data(), text.size());
    line += L'\t';
    if (!image_target.empty()) {
        line += L"0," + std::to_wstring(text.size()) + L",32,";
        AppendTarget(line, image_target);
        line += L';';
    }
    if (payload.size() + line.size() + 1 > limit) return false;
    payload += line;
    payload += L'\n';
    return true;
}

bool MakeMarkdownDocument(const std::wstring& source, std::wstring& payload) {
    Builder b;
    std::wstring masked;
    b.original = &source;
    const size_t source_cost = source.size() * 2 + 16;
    if (source_cost + 1024 >= ipc::kPreviewMaxArchiveChars) return false;
    b.backslash_math = PrepareBackslashMath(source, masked);
    b.source = masked.empty() ? &source : &masked;
    b.limit = ipc::kPreviewMaxArchiveChars - source_cost - 64;
    b.out = L"PULSEMD\t1\n";
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = EnterBlock;
    parser.leave_block = LeaveBlock;
    parser.enter_span = EnterSpan;
    parser.leave_span = LeaveSpan;
    parser.text = Text;
    if (md_parse(b.source->data(), static_cast<MD_SIZE>(b.source->size()), &parser, &b) != 0) return false;
    b.Flush();
    b.out += L"S\t";
    AppendEscaped(b.out, source.data(), source.size());
    b.out += L'\n';
    payload.swap(b.out);
    return true;
}

} // namespace pulse::preview
