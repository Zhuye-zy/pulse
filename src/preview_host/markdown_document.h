// markdown_document.h — Markdown payload for the Quick Look rendered view.
#pragma once

#include <string>

namespace pulse::preview {

// Parses Markdown (CommonMark plus the GitHub table, task list, strikethrough
// and autolink extensions, plus $ / $$ and \( \) / \[ \] math spans) with md4c
// and writes the payload of
// ipc::PreviewContentKind::Markdown, drawn by ui/markdown_view.cpp. One record
// per line, fields separated by tabs; backslash, tab and newline inside a
// field are escaped as \\ \t \n.
//   PULSEMD \t 1
//   B \t kind \t arg \t quote \t indent \t marker \t text \t runs      leaf block
//      kind   p paragraph, h heading (arg 1-6), c code (arg language), r rule,
//             x raw HTML block, t table cell (arg l|c|r|-; marker h = header),
//             m standalone display math (paragraph containing only a display span)
//      quote  enclosing block quotes;  indent  enclosing lists
//      marker first block of a list item: u bullet, o<N> ordered, t0 / t1 task
//      runs   start,length,flags[,target];...  in UTF-16 units of text; flags
//             1 bold, 2 italic, 4 code, 8 strike, 16 link, 32 image, 64 underline,
//             128 inline math, 256 display math; math text/ranges include delimiters;
//             in target (link / image) '%' ',' ';' are percent-encoded
// Embedded display spans keep their containing block; math cannot cross blank paragraphs.
// Backslash-delimited math is limited to 256 spans / 65536 total UTF-16 units,
// with at most 4096 units per span (including delimiters). Code, HTML, images,
// link boundaries and existing dollar spans interrupt candidate discovery.
// Explicit '>' quote prefixes inside a multiline candidate also cause fallback;
// list-continuation indentation is retained as formula whitespace.
//   T \t columns \t quote \t indent     table start;   R   row;   E   table end
//   S \t source                         the Markdown text (source view)
// Remote content is never fetched here; images are only named.
bool MakeMarkdownDocument(const std::wstring& source, std::wstring& payload);

// Building blocks for documents converted to this payload (Jupyter notebooks).
// Appends the blocks of `markdown` (no header, no S record) while the payload
// stays under `limit` characters; lists are indented by base_indent levels.
// False when md4c failed or the limit was reached.
bool AppendMarkdownBlocks(const std::wstring& markdown, std::wstring& payload, size_t limit,
                          int base_indent = 0);
// Appends one leaf block record. image_target: an image paragraph (kind 'p')
// showing that local picture. False when it would pass `limit`.
bool AppendMarkdownBlock(std::wstring& payload, size_t limit, wchar_t kind, const std::wstring& arg,
                         int indent, const std::wstring& marker, const std::wstring& text,
                         const std::wstring& image_target = {});
//   G \t levels    (optional) lists start this many levels in: bullets are
//                  chosen from the level relative to it
//   I \t kernel \t cells   (optional) notebook facts for the status pill
// Marker l<label> on a code block: the notebook In/Out label in the gutter;
// n<label>: a cell output (plain monospace text or a picture, no box).

} // namespace pulse::preview
