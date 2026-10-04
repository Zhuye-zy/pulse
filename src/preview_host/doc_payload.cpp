// doc_payload.cpp — see doc_payload.h.
#include "doc_payload.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <system_error>

namespace pulse::preview {

namespace {

constexpr size_t kMaxImageBytes = 24u * 1024u * 1024u;   // one picture
constexpr size_t kMaxStoredBytes = 96u * 1024u * 1024u;  // one document
constexpr size_t kMaxStoredCount = 400;

void AppendTarget(std::wstring& out, const std::wstring& target) {
    std::wstring encoded;
    for (wchar_t c : target) {
        if (c == L'%') encoded += L"%25";
        else if (c == L',') encoded += L"%2C";
        else if (c == L';') encoded += L"%3B";
        else encoded += c;
    }
    AppendPayloadField(out, encoded);
}

uint64_t Fnv1a(const unsigned char* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

void AppendCodePoint(std::wstring& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out += L'\xFFFD'; return; }
    if (cp >= 0x10000) {
        cp -= 0x10000;
        out += static_cast<wchar_t>(0xD800 + (cp >> 10));
        out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
    } else {
        out += static_cast<wchar_t>(cp);
    }
}

}  // namespace

void AppendPayloadField(std::wstring& out, std::wstring_view text) {
    for (wchar_t c : text) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c != L'\r') out += c;
    }
}

// ---- DocPayload -------------------------------------------------------------

DocPayload::DocPayload(size_t limit) : limit_(limit) { out_ = L"PULSEMD\t1\n"; }

void DocPayload::Record(const std::wstring& line) {
    if (full_) return;
    if (out_.size() + line.size() + 1 > limit_) { full_ = true; return; }
    out_ += line;
    out_ += L'\n';
}

void DocPayload::Begin(wchar_t kind, std::wstring arg, int quote, int indent, std::wstring marker) {
    if (open_) End();
    open_ = true;
    kind_ = kind;
    arg_ = std::move(arg);
    quote_ = quote;
    indent_ = indent;
    marker_ = std::move(marker);
    text_.clear();
    runs_.clear();
}

void DocPayload::Text(std::wstring_view text, unsigned flags, const std::wstring& target) {
    if (!open_ || text.empty() || text_truncated_) return;
    constexpr size_t kMaxBlockChars = 200000;
    size_t count = (std::min)(text.size(), kMaxBlockChars - text_.size());
    if (count < text.size()) {
        text_truncated_ = true;
        if (count && text[count - 1] >= 0xD800 && text[count - 1] <= 0xDBFF) --count;
        if (!count && !text_.empty() && text_.back() >= 0xD800 && text_.back() <= 0xDBFF)
            text_.pop_back();
    }
    text = text.substr(0, count);
    if (text.empty()) return;
    const size_t start = text_.size();
    text_.append(text);
    if (!flags) return;
    if (!runs_.empty()) {
        Run& last = runs_.back();
        if (last.flags == flags && last.target == target && last.start + last.length == start) {
            last.length += text.size();
            return;
        }
    }
    runs_.push_back({start, text.size(), flags, target});
}

void DocPayload::End(bool keep_empty) {
    if (!open_) return;
    open_ = false;
    if (kind_ != L'c') {
        // Trailing spaces left by collapsed whitespace.
        size_t end = text_.size();
        while (end > 0 && (text_[end - 1] == L' ' || text_[end - 1] == L'\n')) --end;
        text_.resize(end);
        for (Run& run : runs_) {
            if (run.start >= end) run.length = 0;
            else run.length = (std::min)(run.length, end - run.start);
        }
    } else {
        while (!text_.empty() && (text_.back() == L'\n' || text_.back() == L'\r')) text_.pop_back();
    }
    if (text_.empty() && !keep_empty && kind_ != L'r') return;
    std::wstring line = L"B\t";
    line += kind_;
    line += L'\t';
    AppendPayloadField(line, arg_);
    line += L'\t' + std::to_wstring(quote_) + L'\t' + std::to_wstring(indent_) + L'\t' + marker_ + L'\t';
    AppendPayloadField(line, text_);
    line += L'\t';
    for (const Run& run : runs_) {
        if (!run.length) continue;
        line += std::to_wstring(run.start) + L',' + std::to_wstring(run.length) + L',' + std::to_wstring(run.flags);
        if (!run.target.empty()) { line += L','; AppendTarget(line, run.target); }
        line += L';';
    }
    const bool was_full = full_;
    Record(line);
    if (!was_full && !full_) ++blocks_;
}

void DocPayload::Image(const std::wstring& path, const std::wstring& alt, int quote, int indent) {
    if (open_) End();
    const std::wstring label = alt.empty() ? std::wstring(L"image") : alt;
    Begin(L'p', {}, quote, indent);
    Text(label, kDocImage, path);
    End();
}

void DocPayload::Rule() {
    if (open_) End();
    Begin(L'r');
    End(true);
}

void DocPayload::TableBegin(size_t columns, int quote, int indent) {
    if (open_) End();
    Record(L"T\t" + std::to_wstring(columns) + L'\t' + std::to_wstring(quote) + L'\t' + std::to_wstring(indent));
}

void DocPayload::TableRow() {
    if (open_) End(true);
    Record(L"R");
}

void DocPayload::TableEnd() {
    if (open_) End(true);
    Record(L"E");
}

// ---- Image cache ------------------------------------------------------------

bool PreviewImageCache::Ready() {
    if (tried_) return !dir_.empty();
    tried_ = true;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) return false;
    dir /= L"Pulse";
    dir /= L"QuickLook";
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) return false;
    dir_ = dir.wstring();
    // Prune pictures of documents looked at more than a day ago.
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    std::vector<std::filesystem::path> old;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && !e2 && it->last_write_time(e2) < cutoff && !e2) old.push_back(it->path());
        if (old.size() > 4000) break;
    }
    for (const auto& p : old) std::filesystem::remove(p, ec);
    return true;
}

std::wstring PreviewImageCache::Store(const std::vector<unsigned char>& bytes, std::wstring_view extension) {
    if (bytes.size() < 8 || bytes.size() > kMaxImageBytes) return {};
    if (stored_count_ >= kMaxStoredCount || stored_bytes_ + bytes.size() > kMaxStoredBytes) return {};
    if (!Ready()) return {};
    std::wstring ext(extension);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    for (wchar_t c : ext) if (!(c == L'.' || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) { ext = L".bin"; break; }
    if (ext.size() > 6) ext = L".bin";
    wchar_t name[40];
    swprintf(name, 40, L"%016llx", static_cast<unsigned long long>(Fnv1a(bytes.data(), bytes.size())));
    const std::filesystem::path file = std::filesystem::path(dir_) / (std::wstring(name) + ext);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        const std::filesystem::path partial = std::filesystem::path(dir_) / (std::wstring(name) + ext + L".part");
        FILE* f = nullptr;
#ifdef _WIN32
        if (_wfopen_s(&f, partial.c_str(), L"wb") != 0) f = nullptr;
#else
        f = fopen(partial.c_str(), "wb");
#endif
        if (!f) return {};
        const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        fclose(f);
        if (!ok) { std::filesystem::remove(partial, ec); return {}; }
        std::filesystem::rename(partial, file, ec);
        if (ec) {
            std::filesystem::remove(partial, ec);
            if (!std::filesystem::exists(file, ec)) return {};
        }
    } else {
        // Touch, so pruning keeps pictures still in use.
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
    }
    stored_bytes_ += bytes.size();
    ++stored_count_;
    return file.wstring();
}

std::wstring PreviewImageCache::StoreBase64(const std::wstring& data, std::wstring_view extension) {
    std::vector<unsigned char> out;
    out.reserve(data.size() * 3 / 4);
    unsigned value = 0;
    int bits = 0;
    for (wchar_t c : data) {
        int d;
        if (c >= L'A' && c <= L'Z') d = c - L'A';
        else if (c >= L'a' && c <= L'z') d = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') d = c - L'0' + 52;
        else if (c == L'+' || c == L'-') d = 62;
        else if (c == L'/' || c == L'_') d = 63;
        else continue;  // padding, line breaks
        value = (value << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((value >> bits) & 0xFF));
        }
    }
    return Store(out, extension);
}

std::wstring PreviewImageCache::StoreText(const std::wstring& text, std::wstring_view extension) {
    const std::string utf8 = ToUtf8(text);
    return Store(std::vector<unsigned char>(utf8.begin(), utf8.end()), extension);
}

// ---- Text helpers -----------------------------------------------------------

std::string ToUtf8(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    return out;
}

std::wstring DecodeDocumentText(const std::vector<unsigned char>& b) {
    std::wstring out;
    size_t i = 0;
    if (b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        const bool le = b[0] == 0xFF;
        out.reserve(b.size() / 2);
        for (i = 2; i + 1 < b.size(); i += 2)
            out += static_cast<wchar_t>(le ? (b[i] | (b[i + 1] << 8)) : ((b[i] << 8) | b[i + 1]));
        return out;
    }
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;
    out.reserve(b.size());
    while (i < b.size()) {
        const unsigned c = b[i];
        if (c < 0x80) { out += static_cast<wchar_t>(c); ++i; continue; }
        int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > b.size()) { out += L'\xFFFD'; ++i; continue; }
        uint32_t cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            if ((b[i + k] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (b[i + k] & 0x3F);
        }
        if (!ok) { out += L'\xFFFD'; ++i; continue; }
        AppendCodePoint(out, cp);
        i += len;
    }
    return out;
}

std::wstring DecodeXmlEntities(std::wstring_view s) {
    if (s.find(L'&') == std::wstring_view::npos) return std::wstring(s);
    struct Named { const wchar_t* name; uint32_t cp; };
    static constexpr Named kNamed[] = {
        {L"lt", '<'}, {L"gt", '>'}, {L"amp", '&'}, {L"quot", '"'}, {L"apos", '\''},
        {L"nbsp", 0xA0}, {L"copy", 0xA9}, {L"reg", 0xAE}, {L"trade", 0x2122}, {L"hellip", 0x2026},
        {L"mdash", 0x2014}, {L"ndash", 0x2013}, {L"lsquo", 0x2018}, {L"rsquo", 0x2019},
        {L"ldquo", 0x201C}, {L"rdquo", 0x201D}, {L"laquo", 0xAB}, {L"raquo", 0xBB},
        {L"middot", 0xB7}, {L"bull", 0x2022}, {L"times", 0xD7}, {L"deg", 0xB0}, {L"shy", 0xAD},
        {L"emsp", 0x2003}, {L"ensp", 0x2002}, {L"thinsp", 0x2009}, {L"zwnj", 0x200C}, {L"zwj", 0x200D},
        {L"sect", 0xA7}, {L"para", 0xB6}, {L"eacute", 0xE9}, {L"egrave", 0xE8}, {L"agrave", 0xE0},
        {L"ccedil", 0xE7}, {L"uuml", 0xFC}, {L"ouml", 0xF6}, {L"auml", 0xE4}, {L"szlig", 0xDF},
    };
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') { out += s[i]; continue; }
        const size_t semi = s.find(L';', i + 1);
        if (semi == std::wstring_view::npos || semi - i > 12) { out += s[i]; continue; }
        const std::wstring_view e = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        bool known = false;
        if (!e.empty() && e[0] == L'#') {
            const bool hex = e.size() > 1 && (e[1] == L'x' || e[1] == L'X');
            for (size_t k = hex ? 2 : 1; k < e.size(); ++k) {
                const wchar_t d = e[k];
                int v = -1;
                if (d >= L'0' && d <= L'9') v = d - L'0';
                else if (hex && d >= L'a' && d <= L'f') v = d - L'a' + 10;
                else if (hex && d >= L'A' && d <= L'F') v = d - L'A' + 10;
                if (v < 0 || cp > 0x10FFFF) { cp = 0xFFFD; break; }
                cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(v);
            }
            known = e.size() > (hex ? 2u : 1u);
        } else {
            for (const Named& n : kNamed) if (e == n.name) { cp = n.cp; known = true; break; }
        }
        if (!known) { out += s[i]; continue; }
        if (cp != 0xAD) AppendCodePoint(out, cp);  // soft hyphens vanish
        i = semi;
    }
    return out;
}

std::wstring ResolvePartName(std::wstring_view base_dir, std::wstring_view href) {
    std::wstring h(href.substr(0, href.find(L'#')));
    // Percent-decoding (UTF-8 sequences).
    std::vector<unsigned char> bytes;
    bool encoded = false;
    const std::string utf8 = ToUtf8(h);
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto hexval = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (utf8[i] == '%' && i + 2 < utf8.size() && hexval(utf8[i + 1]) >= 0 && hexval(utf8[i + 2]) >= 0) {
            bytes.push_back(static_cast<unsigned char>(hexval(utf8[i + 1]) * 16 + hexval(utf8[i + 2])));
            i += 2;
            encoded = true;
        } else {
            bytes.push_back(static_cast<unsigned char>(utf8[i]));
        }
    }
    if (encoded) h = DecodeDocumentText(bytes);
    std::replace(h.begin(), h.end(), L'\\', L'/');
    std::wstring joined = !h.empty() && h[0] == L'/' ? h.substr(1) : std::wstring(base_dir) + h;
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= joined.size()) {
        size_t slash = joined.find(L'/', start);
        if (slash == std::wstring::npos) slash = joined.size();
        const std::wstring part = joined.substr(start, slash - start);
        if (part == L"..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != L".") parts.push_back(part);
        start = slash + 1;
    }
    std::wstring out;
    for (const auto& p : parts) { if (!out.empty()) out += L'/'; out += p; }
    return out;
}

// ---- XmlPull ----------------------------------------------------------------

XmlPull::Kind XmlPull::Next() {
    while (pos_ < s_.size()) {
        if (s_[pos_] != L'<') {
            const size_t lt = s_.find(L'<', pos_);
            const size_t end = lt == std::wstring_view::npos ? s_.size() : lt;
            text_ = DecodeXmlEntities(s_.substr(pos_, end - pos_));
            pos_ = end;
            return Text;
        }
        if (s_.compare(pos_, 4, L"<!--") == 0) {
            const size_t end = s_.find(L"-->", pos_ + 4);
            pos_ = end == std::wstring_view::npos ? s_.size() : end + 3;
            continue;
        }
        if (s_.compare(pos_, 9, L"<![CDATA[") == 0) {
            const size_t end = s_.find(L"]]>", pos_ + 9);
            const size_t stop = end == std::wstring_view::npos ? s_.size() : end;
            text_.assign(s_.substr(pos_ + 9, stop - pos_ - 9));
            pos_ = end == std::wstring_view::npos ? s_.size() : end + 3;
            return Text;
        }
        if (s_.compare(pos_, 2, L"<?") == 0 || s_.compare(pos_, 2, L"<!") == 0) {
            const size_t end = s_.find(L'>', pos_ + 2);
            pos_ = end == std::wstring_view::npos ? s_.size() : end + 1;
            continue;
        }
        const bool closing = pos_ + 1 < s_.size() && s_[pos_ + 1] == L'/';
        size_t i = pos_ + (closing ? 2 : 1);
        const size_t name_start = i;
        while (i < s_.size() && !std::iswspace(s_[i]) && s_[i] != L'>' && s_[i] != L'/') ++i;
        std::wstring qname(s_.substr(name_start, i - name_start));
        if (qname.empty()) {  // stray '<' in text
            text_ = L"<";
            ++pos_;
            return Text;
        }
        const size_t colon = qname.find(L':');
        name_ = colon == std::wstring::npos ? qname : qname.substr(colon + 1);
        if (html_) for (auto& c : name_) c = static_cast<wchar_t>(std::towlower(c));
        attrs_.clear();
        self_closing_ = false;
        // Attributes.
        while (i < s_.size() && s_[i] != L'>') {
            if (s_[i] == L'/') { self_closing_ = true; ++i; continue; }
            if (std::iswspace(s_[i])) { ++i; continue; }
            const size_t an = i;
            while (i < s_.size() && !std::iswspace(s_[i]) && s_[i] != L'=' && s_[i] != L'>' && s_[i] != L'/') ++i;
            Attribute a;
            a.qualified.assign(s_.substr(an, i - an));
            while (i < s_.size() && std::iswspace(s_[i])) ++i;
            if (i < s_.size() && s_[i] == L'=') {
                ++i;
                while (i < s_.size() && std::iswspace(s_[i])) ++i;
                if (i < s_.size() && (s_[i] == L'"' || s_[i] == L'\'')) {
                    const wchar_t q = s_[i];
                    const size_t close = s_.find(q, i + 1);
                    const size_t stop = close == std::wstring_view::npos ? s_.size() : close;
                    a.value = DecodeXmlEntities(s_.substr(i + 1, stop - i - 1));
                    i = close == std::wstring_view::npos ? s_.size() : close + 1;
                } else {
                    const size_t vs = i;
                    while (i < s_.size() && !std::iswspace(s_[i]) && s_[i] != L'>') ++i;
                    a.value = DecodeXmlEntities(s_.substr(vs, i - vs));
                }
            }
            if (a.qualified.empty()) { ++i; continue; }
            const size_t ac = a.qualified.find(L':');
            a.local = ac == std::wstring::npos ? a.qualified : a.qualified.substr(ac + 1);
            if (html_) for (auto& c : a.local) c = static_cast<wchar_t>(std::towlower(c));
            attrs_.push_back(std::move(a));
        }
        pos_ = i < s_.size() ? i + 1 : s_.size();
        if (closing) return Close;
        if (html_) {
            static constexpr std::wstring_view kVoid[] = {L"br", L"img", L"hr", L"meta", L"link", L"input",
                                                          L"col", L"area", L"base", L"wbr", L"source", L"param"};
            for (auto v : kVoid) if (name_ == v) self_closing_ = true;
            if (!self_closing_ && (name_ == L"script" || name_ == L"style")) {
                const std::wstring close = L"</" + qname;
                size_t end = pos_;
                for (;;) {
                    end = s_.find(L"</", end);
                    if (end == std::wstring_view::npos) break;
                    std::wstring tag(s_.substr(end, close.size()));
                    for (auto& c : tag) c = static_cast<wchar_t>(std::towlower(c));
                    std::wstring want = close;
                    for (auto& c : want) c = static_cast<wchar_t>(std::towlower(c));
                    if (tag == want) break;
                    end += 2;
                }
                if (end == std::wstring_view::npos) { pos_ = s_.size(); }
                else { const size_t gt = s_.find(L'>', end); pos_ = gt == std::wstring_view::npos ? s_.size() : gt + 1; }
                self_closing_ = true;  // content skipped
            }
        }
        return Open;
    }
    return Eof;
}

std::wstring XmlPull::Attr(std::wstring_view local) const {
    for (const auto& a : attrs_) if (a.local == local) return a.value;
    return {};
}

std::wstring XmlPull::QAttr(std::wstring_view qualified) const {
    for (const auto& a : attrs_) if (a.qualified == qualified) return a.value;
    return {};
}

void XmlPull::SkipElement() {
    if (self_closing_) return;
    const std::wstring target = name_;
    int depth = 1;
    for (;;) {
        const Kind k = Next();
        if (k == Eof) return;
        if (k == Open && !self_closing_ && name_ == target) ++depth;
        else if (k == Close && name_ == target && --depth == 0) return;
    }
}

}  // namespace pulse::preview
