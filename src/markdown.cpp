#include <huxerui/richeditor/format.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#include <huxerui/markdown/document.h>

#include "document_internal.h"

namespace huxerui::richeditor {

enum class MarkdownObjectSyntax::Kind { DollarSpan, DoubleDollarBlock, FencedBlock };

MarkdownObjectSyntax::MarkdownObjectSyntax(Kind kind, std::string_view fence_info)
    : kind_(kind), fence_info_(fence_info) {}

MarkdownObjectSyntax MarkdownObjectSyntax::DollarSpan() {
  return MarkdownObjectSyntax(Kind::DollarSpan);
}

MarkdownObjectSyntax MarkdownObjectSyntax::DoubleDollarBlock() {
  return MarkdownObjectSyntax(Kind::DoubleDollarBlock);
}

MarkdownObjectSyntax MarkdownObjectSyntax::FencedBlock(std::string_view fence_info) {
  return MarkdownObjectSyntax(Kind::FencedBlock, fence_info);
}

namespace detail {

struct MarkdownObjectSyntaxAccess {
  using Kind = MarkdownObjectSyntax::Kind;

  static Kind GetKind(const MarkdownObjectSyntax& syntax) { return syntax.kind_; }
  static std::string_view FenceInfo(const MarkdownObjectSyntax& syntax) { return syntax.fence_info_; }
};

} // namespace detail

namespace {

using markdown::MarkdownNode;
using markdown::NodeKind;
using detail::BlockPath;
using SyntaxAccess = detail::MarkdownObjectSyntaxAccess;
using SyntaxKind = SyntaxAccess::Kind;

template<class Data>
using MappingMember = std::optional<MarkdownObjectMapping<Data>> MarkdownExtension::*;

template<class Data, class Predicate>
const MarkdownObjectMapping<Data>* FindMapping(const MarkdownOptions& options, MappingMember<Data> member,
                                               Predicate matches) {
  for (const auto& extension : options.extensions) {
    if (const auto& mapping = extension.*member; mapping && matches(*mapping)) {
      return &*mapping;
    }
  }
  return nullptr;
}

template<class Data>
const MarkdownObjectMapping<Data>* FindSyntax(const MarkdownOptions& options, MappingMember<Data> member,
                                              const MarkdownObjectSyntax& syntax) {
  return FindMapping(options, member, [&](const auto& mapping) {
    return std::ranges::find(mapping.syntax, syntax) != mapping.syntax.end();
  });
}

bool ValidLiteral(std::string_view text) {
  if (text.find('\0') != text.npos || text.find('\r') != text.npos) {
    return false;
  }
  try {
    (void)AttributedText(std::string(text));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

template<class Data>
void ValidateMappings(const MarkdownOptions& options, MappingMember<Data> member) {
  std::vector<std::string_view> types;
  std::vector<MarkdownObjectSyntax> syntax;
  for (const auto& extension : options.extensions) {
    const auto& mapping = extension.*member;
    if (!mapping) {
      continue;
    }
    if (!detail::ValidCustomData({.type = mapping->type}) || !mapping->decode || !mapping->encode ||
        mapping->syntax.empty()) {
      throw std::invalid_argument("Incomplete Markdown object mapping");
    }
    constexpr bool block = std::is_same_v<Data, CustomBlockData>;
    if ((!block && mapping->type == inline_image_type) || std::ranges::find(types, mapping->type) != types.end()) {
      throw std::invalid_argument("Conflicting Markdown object type: " + mapping->type);
    }
    types.push_back(mapping->type);
    for (const auto& entry : mapping->syntax) {
      const auto info = SyntaxAccess::FenceInfo(entry);
      const auto kind = SyntaxAccess::GetKind(entry);
      const bool valid = kind == SyntaxKind::FencedBlock
          ? block && !info.empty() && info.find('\n') == info.npos &&
              info.find_first_not_of(" \t") == 0 && info.find_last_not_of(" \t") == info.size() - 1 && ValidLiteral(info)
          : kind == (block ? SyntaxKind::DoubleDollarBlock : SyntaxKind::DollarSpan);
      if (!valid || std::ranges::find(syntax, entry) != syntax.end()) {
        throw std::invalid_argument("Invalid or conflicting Markdown object syntax");
      }
      syntax.push_back(entry);
    }
  }
}

struct InlineBuilder {
  BlockData& block;
  TextOffset length = 0;

  void AppendObject(InlineObjectData data, Mark marks) {
    block.inline_objects.emplace_back(length, std::move(data));
    Append(detail::object_replacement, marks);
  }

  void Append(std::string_view text, Mark marks) {
    auto normalized = detail::NormalizeLineEndings(text);
    const TextOffset end = length + AttributedText(normalized).Length();
    detail::AddMark(block.marks, {length, end}, marks);
    block.text += normalized;
    length = end;
  }
};

std::string_view TakeLine(std::string_view& source) {
  const auto line = source.substr(0, source.find_first_of("\r\n"));
  source.remove_prefix(line.size());
  if (source.starts_with('\r')) {
    source.remove_prefix(1);
  }
  if (source.starts_with('\n')) {
    source.remove_prefix(1);
  }
  return line;
}

bool FrontMatterFence(std::string_view line) {
  return line.starts_with("---") && line.find_first_not_of(" \t", 3) == line.npos;
}

/// Returns the offset after a single-line quoted key, or npos if the quote is unclosed.
std::size_t QuotedKeyEnd(std::string_view line) {
  const auto quote = line.front();
  for (std::size_t i = 1; i < line.size(); ++i) {
    if (quote == '"' && line[i] == '\\') {
      ++i;
    } else if (line[i] == quote) {
      if (quote == '\'' && line.substr(i + 1).starts_with('\'')) {
        ++i;
      } else {
        return i + 1;
      }
    }
  }
  return line.npos;
}

bool FrontMatterKey(std::string_view line) {
  if (line.empty()) {
    return false;
  }
  std::size_t end;
  if (line.starts_with('"') || line.starts_with('\'')) {
    end = QuotedKeyEnd(line);
  } else {
    const auto first_delimiter = std::ranges::find_if_not(line, [](unsigned char byte) {
      constexpr std::string_view ascii_key = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-";
      // The codec validates the UTF-8 prefix before reporting it as front matter.
      return byte >= 0x80 || ascii_key.find(static_cast<char>(byte)) != ascii_key.npos;
    });
    end = static_cast<std::size_t>(first_delimiter - line.begin());
  }
  if (end == 0 || end == line.npos) {
    return false;
  }
  const auto colon = line.find_first_not_of(" \t", end);
  return colon != line.npos && line[colon] == ':' &&
      (colon + 1 == line.size() || line[colon + 1] == ' ' || line[colon + 1] == '\t');
}

/// Recognizes a leading fenced mapping with simple or quoted keys, indented values, and block sequences.
std::optional<std::string_view> FrontMatterBody(std::string_view source) {
  if (source.starts_with("\xef\xbb\xbf")) {
    source.remove_prefix(3);
  }
  if (!FrontMatterFence(TakeLine(source))) {
    return std::nullopt;
  }
  bool has_key = false;
  while (!source.empty()) {
    const auto line = TakeLine(source);
    if (FrontMatterFence(line)) {
      return has_key ? std::optional(source) : std::nullopt;
    }
    if (FrontMatterKey(line)) {
      has_key = true;
      continue;
    }
    const auto start = line.find_first_not_of(" \t");
    if (start == line.npos || line[start] == '#') {
      continue;
    }
    const bool sequence_item = line.starts_with("- ") || line.starts_with("-\t");
    if (!has_key || (start == 0 && !sequence_item)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

TableColumnAlignment ConvertAlignment(markdown::CellAlignment alignment) {
  switch (alignment) {
  case markdown::CellAlignment::Default: return TableColumnAlignment::Default;
  case markdown::CellAlignment::Left: return TableColumnAlignment::Left;
  case markdown::CellAlignment::Center: return TableColumnAlignment::Center;
  case markdown::CellAlignment::Right: return TableColumnAlignment::Right;
  }
  return TableColumnAlignment::Default;
}

class MarkdownImport {
public:
  explicit MarkdownImport(const MarkdownOptions& options) : options_(options) {}

  ImportResult Run(std::string_view source, LossPolicy policy) {
    auto options = markdown::ParseOptions::CommonMark();
    options.tables = true;
    options.task_lists = true;
    options.strikethrough = true;
    options.math = FindSyntax(options_, &MarkdownExtension::block, MarkdownObjectSyntax::DoubleDollarBlock()) ||
        FindSyntax(options_, &MarkdownExtension::inline_object, MarkdownObjectSyntax::DollarSpan());
    try {
      if (source.size() <= options.max_input_bytes) {
        if (const auto body = FrontMatterBody(source)) {
          (void)AttributedText(std::string(source.substr(0, source.size() - body->size())));
          Report("markdown.front_matter", "YAML front matter is unsupported. AllowLoss omits it.");
          source = *body;
        }
      }
      const auto parsed = markdown::ParseMarkdown(source, options);
      auto blocks = ConvertChildren(parsed.Root());
      if (issues_.empty() || policy == LossPolicy::AllowLoss) {
        return {RichDocument::Create(std::move(blocks)), std::move(issues_)};
      }
    } catch (const std::logic_error& error) {
      Report("markdown.invalid", error.what());
    } catch (const std::runtime_error& error) {
      Report("markdown.invalid", error.what());
    }
    return {.issues = std::move(issues_)};
  }

private:
  void Report(std::string code, std::string message) {
    issues_.push_back({std::move(code), path_, {}, std::move(message)});
  }

  template<class Data>
  std::optional<Data> Decode(const MarkdownObjectMapping<Data>* mapping, const MarkdownObjectText& source,
                             bool (*valid)(const Data&)) {
    if (!mapping) {
      Report("markdown.extension_missing", "Markdown object syntax has no registered mapping");
      return std::nullopt;
    }
    auto data = mapping->decode(source);
    if (data && data->type == mapping->type && valid(*data)) {
      return data;
    }
    Report("markdown.extension_import", "Markdown object could not be decoded as " + mapping->type);
    return std::nullopt;
  }

  void Inline(const MarkdownNode& node, InlineBuilder& builder, Mark marks, bool in_table = false) {
    switch (node.kind) {
    case NodeKind::Text:
      builder.Append(node.text, marks);
      return;
    case NodeKind::Code:
      if (in_table && node.text.find("\\|") != node.text.npos) {
        // MD4C leaves the table escape in the code span's literal text.
        std::string code;
        for (std::size_t i = 0; i < node.text.size(); ++i) {
          if (node.text[i] == '\\' && i + 1 < node.text.size() && node.text[i + 1] == '|') {
            ++i;
          }
          code += node.text[i];
        }
        builder.Append(code, marks | Mark::Code);
      } else {
        builder.Append(node.text, marks | Mark::Code);
      }
      return;
    case NodeKind::Strong:
      marks = marks | Mark::Bold;
      break;
    case NodeKind::Emphasis:
      marks = marks | Mark::Italic;
      break;
    case NodeKind::Strikethrough:
      marks = marks | Mark::Strike;
      break;
    case NodeKind::Link:
      break;
    case NodeKind::Image: {
      auto alt = detail::NormalizeLineEndings(markdown::PlainText(node));
      const ImageBlockData image{node.destination, alt, detail::NormalizeLineEndings(node.title)};
      if (options_.inline_images && detail::ValidImageData(image) && alt.find('\n') == alt.npos) {
        builder.AppendObject(InlineImage(image), marks);
      } else {
        Report("markdown.image", "Image replaced by its alternative text");
        builder.Append(alt, marks);
      }
      return;
    }
    case NodeKind::InlineMath: {
      const MarkdownObjectText source{MarkdownObjectSyntax::DollarSpan(), detail::NormalizeLineEndings(node.text)};
      if (auto data = Decode(FindSyntax(options_, &MarkdownExtension::inline_object, source.syntax), source,
                             detail::ValidInlineData)) {
        builder.AppendObject(std::move(*data), marks);
      } else {
        builder.Append(source.text, marks);
      }
      return;
    }
    case NodeKind::DisplayMath:
      Report("markdown.extension_placement", "Display math here cannot be imported as a top-level custom block");
      builder.Append(node.text, marks);
      return;
    default:
      Report("markdown.inline", "Unsupported inline syntax converted to text");
      builder.Append(markdown::PlainText(node), marks);
      return;
    }

    const auto start = builder.length;
    for (const auto& child : node.children) {
      Inline(child, builder, marks, in_table);
    }
    if (node.kind == NodeKind::Link && builder.length > start) {
      builder.block.links.push_back({{start, builder.length}, detail::NormalizeLineEndings(node.destination),
                                     detail::NormalizeLineEndings(node.title)});
    } else if (node.kind == NodeKind::Link) {
      Report("markdown.empty_link", "Empty link omitted");
    }
  }

  std::vector<Block> ConvertChildren(const MarkdownNode& node) {
    std::vector<Block> blocks;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
      path_.push_back(i);
      blocks.push_back(Convert(node.children[i]));
      path_.pop_back();
    }
    return blocks;
  }

  BlockData ConvertTable(const MarkdownNode& node) {
    // Lib-Markdown emits one header row, then the body with GFM cell padding already applied.
    const auto& header = node.children.front().children.front();
    const auto columns = header.children.size();
    std::size_t rows = 0;
    for (const auto& section : node.children) {
      rows += section.children.size();
    }
    BlockData table;
    if (path_.size() != 1 || !detail::ValidTableSize(rows, columns)) {
      Report("markdown.table", path_.size() != 1 ? "Nested table converted to plain text"
                                                : "Table exceeds the supported grid size and was converted to plain text");
      bool first = true;
      for (const auto& section : node.children) {
        for (const auto& row : section.children) {
          if (!first) {
            table.text += '\n';
          }
          first = false;
          for (std::size_t i = 0; i < row.children.size(); ++i) {
            if (i != 0) {
              table.text += " | ";
            }
            table.text += detail::NormalizeLineEndings(markdown::PlainText(row.children[i]));
          }
        }
      }
      return table;
    }

    table.kind = BlockKind::Table;
    table.table_has_header = true;
    table.table_column_alignments.reserve(columns);
    for (const auto& cell : header.children) {
      table.table_column_alignments.push_back(ConvertAlignment(cell.alignment));
    }
    for (std::size_t s = 0; s < node.children.size(); ++s) {
      const auto& section = node.children[s];
      path_.push_back(s);
      for (std::size_t r = 0; r < section.children.size(); ++r) {
        const auto& source = section.children[r];
        BlockData row{.kind = BlockKind::TableRow};
        path_.push_back(r);
        for (std::size_t c = 0; c < source.children.size(); ++c) {
          const auto& cell = source.children[c];
          path_.push_back(c);
          BlockData paragraph;
          InlineBuilder builder{paragraph};
          for (const auto& child : cell.children) {
            Inline(child, builder, Mark::None, true);
          }
          row.children.emplace_back(BlockData{.kind = BlockKind::TableCell,
                                              .children = {Block(std::move(paragraph))}});
          path_.pop_back();
        }
        table.children.emplace_back(std::move(row));
        path_.pop_back();
      }
      path_.pop_back();
    }
    return table;
  }

  Block Convert(const MarkdownNode& node) {
    BlockData block;
    switch (node.kind) {
    case NodeKind::Heading:
      block.kind = BlockKind::Heading;
      block.heading_level = node.heading_level;
      [[fallthrough]];
    case NodeKind::Paragraph: {
      if (node.kind == NodeKind::Paragraph && path_.size() == 1 && node.children.size() == 1 &&
          node.children.front().kind == NodeKind::DisplayMath) {
        MarkdownObjectText source{MarkdownObjectSyntax::DoubleDollarBlock(),
                                  detail::NormalizeLineEndings(node.children.front().text)};
        if (auto data = Decode(FindSyntax(options_, &MarkdownExtension::block, source.syntax), source,
                               detail::ValidCustomData)) {
          block.kind = BlockKind::Custom;
          block.custom = std::move(data);
        } else {
          block.text = std::move(source.text);
        }
        break;
      }
      if (node.kind == NodeKind::Paragraph && path_.size() == 1 && node.children.size() == 1 &&
          node.children.front().kind == NodeKind::Image) {
        const auto& image = node.children.front();
        ImageBlockData data{image.destination, detail::NormalizeLineEndings(markdown::PlainText(image)),
                            detail::NormalizeLineEndings(image.title)};
        if (detail::ValidImageData(data)) {
          block.kind = BlockKind::Image;
          block.image = std::move(data);
          break;
        }
      }
      InlineBuilder builder{block};
      for (const auto& child : node.children) {
        Inline(child, builder, Mark::None);
      }
      if (block.kind == BlockKind::Heading && block.text.find('\n') != std::string::npos) {
        Report("markdown.heading_break", "Line breaks in a heading replaced by spaces");
        std::ranges::replace(block.text, '\n', ' ');
      }
      break;
    }
    case NodeKind::BlockQuote:
    case NodeKind::ListItem:
      block.kind = node.kind == NodeKind::BlockQuote ? BlockKind::Quote : BlockKind::ListItem;
      block.children = ConvertChildren(node);
      if (block.children.empty()) {
        block.children.emplace_back(BlockData{});
      }
      block.task_checked = node.task_checked;
      break;
    case NodeKind::OrderedList:
    case NodeKind::UnorderedList:
      block.kind = BlockKind::List;
      block.ordered = node.kind == NodeKind::OrderedList;
      block.list_start = node.list_start;
      block.tight = node.tight;
      block.children = ConvertChildren(node);
      break;
    case NodeKind::CodeBlock:
      block.kind = BlockKind::CodeBlock;
      block.text = detail::NormalizeLineEndings(node.text);
      // The fence supplies one line terminator after its final content line.
      if (block.text.ends_with('\n')) {
        block.text.pop_back();
      }
      block.fence_info = detail::NormalizeLineEndings(node.fence_info);
      if (!block.fence_info.empty()) {
        const auto syntax = MarkdownObjectSyntax::FencedBlock(block.fence_info);
        if (const auto* mapping = FindSyntax(options_, &MarkdownExtension::block, syntax)) {
          if (path_.size() != 1) {
            Report("markdown.extension_placement", "Mapped code fence kept as code outside the document root");
          } else if (auto data = Decode(mapping, {syntax, block.text}, detail::ValidCustomData)) {
            block = BlockData{.kind = BlockKind::Custom, .custom = std::move(data)};
            break;
          }
        }
      }
      if (block.fence_info.find('\n') != std::string::npos) {
        Report("markdown.fence_info", "Line breaks in code fence info replaced by spaces");
        std::ranges::replace(block.fence_info, '\n', ' ');
      }
      break;
    case NodeKind::ThematicBreak:
      block.kind = BlockKind::ThematicBreak;
      break;
    case NodeKind::Table:
      block = ConvertTable(node);
      break;
    default:
      Report("markdown.block", "Unsupported block syntax converted to text");
      block.text = detail::NormalizeLineEndings(markdown::PlainText(node));
      break;
    }
    return Block(std::move(block));
  }

  BlockPath path_;
  std::vector<ConversionIssue> issues_;
  const MarkdownOptions& options_;
};

std::string EscapeText(std::string_view text, bool protect) {
  constexpr std::string_view punctuation = "\\`*_{}[]()#+-.!><|~&$";
  std::string result;
  std::size_t last = text.empty() ? 0 : text.size() - 1;
  while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xC0U) == 0x80U) {
    --last;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '\n') {
      result += "\\\n";
    } else if (protect && (i == 0 || i == last)) {
      const auto lead = static_cast<unsigned char>(ch);
      const unsigned width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      unsigned scalar = width == 1 ? lead : lead & (0x7FU >> width);
      for (unsigned unit = 1; unit < width; ++unit) {
        scalar = (scalar << 6U) | (static_cast<unsigned char>(text[++i]) & 0x3FU);
      }
      result += "&#" + std::to_string(scalar) + ';';
    } else if (ch == '\t') {
      result += "&#9;";
    } else if (ch == ' ' && (i == 0 || i + 1 == text.size() || text[i - 1] == '\n' || text[i + 1] == '\n')) {
      result += "&#32;";
    } else {
      if (punctuation.find(ch) != punctuation.npos) {
        result += '\\';
      }
      result += ch;
    }
  }
  return result;
}

std::string EscapeAttribute(std::string_view text, bool in_table = false) {
  std::string result;
  for (const char ch : text) {
    if (ch == '\n') {
      result += "&#10;";
    } else {
      if (ch == '\\' || ch == '<' || ch == '>' || ch == '"' || ch == '&' || (in_table && ch == '|')) {
        result += '\\';
      }
      result += ch;
    }
  }
  return result;
}

std::size_t LongestRun(std::string_view text, char delimiter) {
  std::size_t longest = 0;
  std::size_t run = 0;
  for (const char ch : text) {
    run = ch == delimiter ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  return longest;
}

std::string WriteFence(std::string_view source, std::string_view info) {
  const std::string fence(std::max<std::size_t>(3, LongestRun(source, '~') + 1), '~');
  auto text = fence;
  if (!info.empty()) {
    text += ' ';
    text += EscapeAttribute(info);
  }
  text += '\n';
  if (!source.empty()) {
    text += source;
    text += '\n';
  }
  return text + fence;
}

template<class Data>
std::optional<std::string> EncodeObject(const MarkdownOptions& options, MappingMember<Data> member,
                                        const Data& data, bool in_table = false) {
  const auto* mapping = FindMapping(options, member, [&](const auto& entry) { return entry.type == data.type; });
  if (!mapping) {
    return std::nullopt;
  }
  const auto encoded = mapping->encode(data);
  if (!encoded || std::ranges::find(mapping->syntax, encoded->syntax) == mapping->syntax.end() ||
      !ValidLiteral(encoded->text)) {
    return std::nullopt;
  }
  const auto& text = encoded->text;
  switch (SyntaxAccess::GetKind(encoded->syntax)) {
  case SyntaxKind::FencedBlock:
    return WriteFence(text, SyntaxAccess::FenceInfo(encoded->syntax));
  case SyntaxKind::DoubleDollarBlock:
    if (!text.empty() && text.find_first_of("$\n") == text.npos) {
      return "$$\n" + text + "\n$$";
    }
    break;
  case SyntaxKind::DollarSpan:
    if (!text.empty() && text.find_first_of(in_table ? "$\n|" : "$\n") == text.npos) {
      return '$' + text + '$';
    }
    break;
  }
  return std::nullopt;
}

std::string CodeSpan(std::string text, bool in_table) {
  if (in_table && text.find('|') != text.npos) {
    std::string escaped;
    for (const char ch : text) {
      if (ch == '|') {
        escaped += '\\';
      }
      escaped += ch;
    }
    text = std::move(escaped);
  }
  const std::string fence(LongestRun(text, '`') + 1, '`');
  const bool pad = text.starts_with('`') || text.ends_with('`') ||
      (text.starts_with(' ') && text.ends_with(' ') && text.find_first_not_of(' ') != text.npos);
  if (pad) {
    return fence + " " + text + " " + fence;
  }
  return fence + text + fence;
}

void PrefixLines(std::string& out, std::string_view text, std::string_view first, std::string_view rest) {
  out += first;
  for (std::size_t begin = 0;;) {
    const auto end = text.find('\n', begin);
    out += text.substr(begin, end == text.npos ? text.npos : end - begin);
    if (end == text.npos) {
      return;
    }
    out += '\n';
    out += rest;
    begin = end + 1;
  }
}

constexpr std::array markdown_marks{
    std::pair{Mark::Bold, std::string_view("**")}, std::pair{Mark::Italic, std::string_view("*")},
    std::pair{Mark::Strike, std::string_view("~~")}, std::pair{Mark::Code, std::string_view{}},
};

class MarkdownWriter {
public:
  explicit MarkdownWriter(const MarkdownOptions& options) : options_(options) {}

  ExportResult Run(const RichDocument& document, LossPolicy policy) {
    auto result = WriteOnce(document, policy);
    if (policy == LossPolicy::AllowLoss) {
      // Bound full-document retries and keep objects in unrelated blocks intact.
      constexpr unsigned max_fallback_retries = 4;
      for (unsigned retry = 0; mismatch_ && retry < max_fallback_retries; ++retry) {
        if (FallbackAt(*mismatch_)) {
          break;
        }
        const auto dropped = dropped_encodable_;
        fallback_paths_.push_back(*mismatch_);
        auto fallback = WriteOnce(document, policy);
        if (dropped_encodable_ == dropped) {
          break;
        }
        result = std::move(fallback);
      }
    }
    return result;
  }

private:
  ExportResult WriteOnce(const RichDocument& document, LossPolicy policy) {
    issues_.clear();
    mismatch_.reset();
    protect_ = false;
    dropped_encodable_ = 0;
    auto projected = RichDocument::Create(Project(document.Blocks()));
    auto text = Write(projected);
    auto round_trip = MarkdownImport(options_).Run(text, LossPolicy::Strict);
    if (round_trip.document != projected) {
      // Character references give adjacent emphasis delimiters unambiguous punctuation boundaries.
      protect_ = true;
      auto protected_text = Write(projected);
      auto protected_round_trip = MarkdownImport(options_).Run(protected_text, LossPolicy::Strict);
      if (protected_round_trip.document == projected) {
        text = std::move(protected_text);
        round_trip = std::move(protected_round_trip);
      }
    }
    if (!round_trip.document) {
      mismatch_ = round_trip.issues.empty() ? BlockPath{} : round_trip.issues.front().block_path;
      issues_.push_back({"markdown.round_trip", *mismatch_, {}, "Generated Markdown could not be re-imported without loss"});
    } else if (projected != *round_trip.document) {
      FindMismatch(projected.Blocks(), round_trip.document->Blocks(), mismatch_.emplace());
      issues_.push_back({"markdown.round_trip", *mismatch_, {},
                         "This structure, text, or mark combination has no faithful encoding in the Markdown subset"});
    }
    if (issues_.empty() || policy == LossPolicy::AllowLoss) {
      return {std::move(text), std::move(issues_)};
    }
    return {.issues = std::move(issues_)};
  }

  bool FallbackAt(const BlockPath& path) const {
    return std::ranges::any_of(fallback_paths_, [&](const BlockPath& prefix) {
      return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
    });
  }

  std::string Write(const RichDocument& document) const {
    auto text = Blocks(document.Blocks());
    if (!text.empty()) {
      text += '\n';
    }
    return text;
  }

  std::vector<Block> Project(std::span<const Block> blocks, bool in_cell = false) {
    std::vector<Block> result;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      auto data = blocks[i].Data();
      path_.push_back(i);
      if (!data.children.empty()) {
        data.children = Project(data.children, data.kind == BlockKind::TableCell);
      }
      if (data.kind == BlockKind::Table && !data.table_has_header) {
        data.table_has_header = true;
        const auto columns = data.children.front().Data().children.size();
        if (detail::ValidTableSize(data.children.size() + 1, columns)) {
          const Block empty_cell(BlockData{.kind = BlockKind::TableCell, .children = {Block(BlockData{})}});
          data.children.emplace(data.children.begin(), BlockData{.kind = BlockKind::TableRow,
                                                                 .children = std::vector<Block>(columns, empty_cell)});
          issues_.push_back({"markdown.table_header", path_, {},
                             "Markdown requires a header row. AllowLoss adds an empty header."});
        } else {
          issues_.push_back({"markdown.table_header", path_, {},
                             "Markdown requires a header row. AllowLoss uses the first row as the header."});
        }
      }
      if (in_cell && data.text.find('\n') != data.text.npos) {
        issues_.push_back({"markdown.table_break", path_, {}, "Cell line breaks replaced by spaces"});
        std::ranges::replace(data.text, '\n', ' ');
      }
      if (data.image && data.image->width) {
        issues_.push_back({"markdown.image_size", path_, {},
                           "Markdown cannot store image dimensions. AllowLoss uses automatic sizing."});
        data.image->width.reset();
      }
      if (data.custom) {
        const auto& custom = *data.custom;
        const bool representable = EncodeObject(options_, &MarkdownExtension::block, custom).has_value();
        if (!representable || FallbackAt(path_)) {
          dropped_encodable_ += representable;
          issues_.push_back({"markdown.custom_block", path_, {},
                             "This block has no faithful Markdown encoding. AllowLoss writes its fallback text."});
          data = BlockData{.text = custom.fallback_text};
        }
      } else if (data.kind == BlockKind::CodeBlock && !data.fence_info.empty() &&
                 FindSyntax(options_, &MarkdownExtension::block, MarkdownObjectSyntax::FencedBlock(data.fence_info))) {
        issues_.push_back({"markdown.extension_conflict", path_, {},
                           "The fence info is claimed by a Markdown extension. AllowLoss keeps the code without it."});
        data.fence_info.clear();
      }
      ProjectInlineObjects(data, in_cell);
      auto marks = std::move(data.marks);
      data.marks.clear();
      for (const auto& run : marks) {
        for (const auto& [mark, code, message] : {
                 std::tuple{Mark::Underline, "markdown.underline", "Underline is omitted"},
                 std::tuple{Mark::Highlight, "markdown.highlight", "Highlight is omitted"},
             }) {
          if (detail::Has(run.marks, mark)) {
            issues_.push_back({code, path_, run.range, message});
          }
        }
        detail::AddMark(data.marks, run.range, detail::Without(run.marks, Mark::Underline | Mark::Highlight));
      }
      result.emplace_back(std::move(data));
      path_.pop_back();
    }
    return result;
  }

  void ProjectInlineObjects(BlockData& data, bool in_cell) {
    if (data.inline_objects.empty()) {
      return;
    }
    const AttributedText body(data.text);
    std::vector<std::pair<TextOffset, TextOffset>> shifts;
    std::vector<InlineObject> retained;
    std::string text;
    TextOffset start = 0;
    TextOffset shift = 0;
    for (const auto& object : data.inline_objects) {
      const auto& value = object.data;
      const auto image = ReadInlineImage(value);
      const bool code = std::ranges::any_of(data.marks, [&](const MarkRange& mark) {
        return mark.range.start <= object.offset && object.offset < mark.range.end &&
            detail::Has(mark.marks, Mark::Code);
      });
      const bool standalone_image = path_.size() == 1 && data.kind == BlockKind::Paragraph &&
          body.Length() == 1 && data.marks.empty() && data.links.empty();
      const bool representable = !code &&
          ((options_.inline_images && image && !image->width && value == InlineImage(*image) && !standalone_image) ||
           EncodeObject(options_, &MarkdownExtension::inline_object, value, in_cell).has_value());
      if (representable && !FallbackAt(path_)) {
        auto copy = object;
        copy.offset += shift;
        retained.push_back(std::move(copy));
        continue;
      }
      dropped_encodable_ += representable;
      issues_.push_back({"markdown.inline_object", path_,
                         TextRange{object.offset, object.offset + 1},
                         "This inline object has no faithful Markdown encoding. AllowLoss writes its fallback text."});
      text += body.TextInRange({start, object.offset});
      text += value.fallback_text;
      start = object.offset + 1;
      shift += AttributedText(value.fallback_text).Length() - 1;
      shifts.emplace_back(start, shift);
    }
    if (shifts.empty()) {
      return;
    }
    text += body.TextInRange({start, body.Length()});
    const auto translate = [&](TextOffset offset) {
      const auto after = std::ranges::upper_bound(shifts, offset, {}, &std::pair<TextOffset, TextOffset>::first);
      return offset + (after == shifts.begin() ? 0 : std::prev(after)->second);
    };
    for (auto& mark : data.marks) {
      mark.range = {translate(mark.range.start), translate(mark.range.end)};
    }
    for (auto& link : data.links) {
      link.range = {translate(link.range.start), translate(link.range.end)};
    }
    std::erase_if(data.marks, [](const MarkRange& mark) { return mark.range.IsCollapsed(); });
    std::erase_if(data.links, [](const LinkRange& link) { return link.range.IsCollapsed(); });
    data.inline_objects = std::move(retained);
    data.text = std::move(text);
  }

  static void FindMismatch(std::span<const Block> expected, std::span<const Block> actual, BlockPath& path) {
    const auto count = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < count; ++i) {
      if (expected[i] != actual[i]) {
        path.push_back(i);
        const auto& left = expected[i].Data();
        const auto& right = actual[i].Data();
        if (left.kind == right.kind && !detail::IsLeaf(left.kind) && left.ordered == right.ordered &&
            left.list_start == right.list_start && left.tight == right.tight &&
            left.table_has_header == right.table_has_header) {
          FindMismatch(left.children, right.children, path);
        }
        return;
      }
    }
    path.push_back(count);
  }

  std::string Inline(const Block& block, TextRange range, bool in_table, std::size_t depth = 0) const {
    const auto& body = detail::DocumentAccess::Body(block);
    if (depth == markdown_marks.size()) {
      std::string text;
      auto start = range.start;
      const auto& objects = block.Data().inline_objects;
      auto object = std::ranges::lower_bound(objects, start, {}, &InlineObject::offset);
      for (; object != objects.end() && object->offset < range.end; ++object) {
        text += EscapeText(body.TextInRange({start, object->offset}), protect_);
        if (const auto image = ReadInlineImage(object->data)) {
          text += "![" + EscapeText(image->alt_text, protect_) + "](<" + EscapeAttribute(image->source, in_table) + ">";
          if (!image->title.empty()) {
            text += " \"" + EscapeAttribute(image->title, in_table) + '"';
          }
          text += ')';
        } else {
          text += EncodeObject(options_, &MarkdownExtension::inline_object, object->data, in_table).value();
        }
        start = object->offset + 1;
      }
      return text + EscapeText(body.TextInRange({start, range.end}), protect_);
    }
    const auto [mark, preferred_delimiter] = markdown_marks[depth];
    const auto delimiter = protect_ && mark == Mark::Italic ? std::string_view("_") : preferred_delimiter;
    std::string out;
    TextOffset position = range.start;
    TextRange marked{};
    const auto emit = [&] {
      out += Inline(block, {position, marked.start}, in_table, depth + 1);
      if (mark == Mark::Code) {
        out += CodeSpan(body.TextInRange(marked), in_table);
      } else {
        out += delimiter;
        out += Inline(block, marked, in_table, depth + 1);
        out += delimiter;
      }
      position = marked.end;
    };
    for (const auto& run : block.Data().marks) {
      if (!detail::Has(run.marks, mark) || run.range.end <= range.start || run.range.start >= range.end) {
        continue;
      }
      const TextRange part{std::max(range.start, run.range.start), std::min(range.end, run.range.end)};
      if (!marked.IsCollapsed() && marked.end == part.start) {
        marked.end = part.end;
      } else {
        if (!marked.IsCollapsed()) {
          emit();
        }
        marked = part;
      }
    }
    if (!marked.IsCollapsed()) {
      emit();
    }
    out += Inline(block, {position, range.end}, in_table, depth + 1);
    return out;
  }

  std::string TextWithLinks(const Block& block, bool in_table = false) const {
    std::string out;
    TextOffset start = 0;
    for (const auto& link : block.Data().links) {
      out += Inline(block, {start, link.range.start}, in_table);
      out += '[';
      out += Inline(block, link.range, in_table);
      out += "](<" + EscapeAttribute(link.target, in_table) + ">";
      if (!link.title.empty()) {
        out += " \"" + EscapeAttribute(link.title, in_table) + '"';
      }
      out += ')';
      start = link.range.end;
    }
    out += Inline(block, {start, detail::DocumentAccess::Body(block).Length()}, in_table);
    return out;
  }

  void WriteTable(std::string& out, const BlockData& table) const {
    constexpr std::array delimiters{" --- |", " :--- |", " :---: |", " ---: |"};
    for (std::size_t i = 0; i < table.children.size(); ++i) {
      if (i != 0) {
        out += '\n';
      }
      const auto& cells = table.children[i].Data().children;
      out += '|';
      for (const auto& cell : cells) {
        out += ' ';
        out += TextWithLinks(cell.Data().children.front(), true);
        out += " |";
      }
      if (i == 0) {
        out += "\n|";
        for (std::size_t c = 0; c < cells.size(); ++c) {
          out += delimiters[static_cast<std::size_t>(detail::ColumnAlignment(table, c))];
        }
      }
    }
  }

  std::string Blocks(std::span<const Block> blocks, bool tight = false) const {
    std::string out;
    bool alternate_marker = false;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      const auto& data = blocks[i].Data();
      if (i != 0) {
        const auto previous = blocks[i - 1].Data().kind;
        out += tight && !(previous == BlockKind::Paragraph && data.kind == BlockKind::Paragraph) ? "\n" : "\n\n";
        alternate_marker = previous == BlockKind::List && data.kind == BlockKind::List ? !alternate_marker : false;
      }
      switch (data.kind) {
      case BlockKind::Image: {
        const auto& image = *data.image;
        out += "![" + EscapeText(image.alt_text, protect_) + "](<" + EscapeAttribute(image.source) + ">";
        if (!image.title.empty()) {
          out += " \"" + EscapeAttribute(image.title) + '"';
        }
        out += ')';
        break;
      }
      case BlockKind::Custom:
        out += EncodeObject(options_, &MarkdownExtension::block, *data.custom).value();
        break;
      case BlockKind::ThematicBreak:
        out += "***";
        break;
      case BlockKind::Paragraph:
        out += TextWithLinks(blocks[i]);
        break;
      case BlockKind::Heading:
        out += std::string(data.heading_level, '#');
        if (!data.text.empty()) {
          out += ' ';
          out += TextWithLinks(blocks[i]);
        }
        break;
      case BlockKind::CodeBlock:
        out += WriteFence(data.text, data.fence_info);
        break;
      case BlockKind::Quote:
        PrefixLines(out, Blocks(data.children), "> ", "> ");
        break;
      case BlockKind::List:
        for (std::size_t j = 0; j < data.children.size(); ++j) {
          if (j != 0) {
            out += data.tight ? "\n" : "\n\n";
          }
          const auto ordinal = static_cast<std::uint64_t>(data.list_start) + j;
          const std::string marker = data.ordered
              ? std::to_string(ordinal <= 999999999 ? ordinal : 1) + (alternate_marker ? ") " : ". ")
              : alternate_marker ? "+ " : "- ";
          const auto& item = data.children[j].Data();
          const std::string indent(marker.size(), ' ');
          const auto content = Blocks(item.children, data.tight);
          auto first = marker;
          if (item.task_checked.has_value()) {
            first += *item.task_checked ? "[x]" : "[ ]";
            if (item.children.front().Data().kind != BlockKind::Paragraph) {
              // A standalone marker lets the parser recognize the first child as a block.
              first += '\n';
              first += indent;
            } else if (!content.empty() && content.front() != '\n') {
              first += ' ';
            }
          }
          PrefixLines(out, content, first, indent);
        }
        break;
      case BlockKind::Table:
        WriteTable(out, data);
        break;
      case BlockKind::ListItem:
      case BlockKind::TableRow:
      case BlockKind::TableCell:
        break;
      }
    }
    return out;
  }

  BlockPath path_;
  std::vector<ConversionIssue> issues_;
  bool protect_ = false;
  std::vector<BlockPath> fallback_paths_;
  std::optional<BlockPath> mismatch_;
  std::size_t dropped_encodable_ = 0;
  const MarkdownOptions& options_;
};

} // namespace

MarkdownCodec::MarkdownCodec(MarkdownOptions options) : options_(std::move(options)) {
  if (std::ranges::any_of(options_.extensions, [](const auto& extension) {
        return !extension.block && !extension.inline_object;
      })) {
    throw std::invalid_argument("Empty Markdown extension");
  }
  ValidateMappings(options_, &MarkdownExtension::block);
  ValidateMappings(options_, &MarkdownExtension::inline_object);
}

ImportResult MarkdownCodec::Import(std::string_view source, LossPolicy policy) const {
  return MarkdownImport(options_).Run(source, policy);
}

ExportResult MarkdownCodec::Export(const RichDocument& document, LossPolicy policy) const {
  try {
    return MarkdownWriter(options_).Run(document, policy);
  } catch (const std::logic_error& error) {
    return {.issues = {{"markdown.invalid", {}, {}, error.what()}}};
  } catch (const std::runtime_error& error) {
    return {.issues = {{"markdown.invalid", {}, {}, error.what()}}};
  } catch (const std::bad_optional_access&) {
    return {.issues = {{"markdown.invalid", {}, {}, "Markdown mapping changed during export"}}};
  }
}

} // namespace huxerui::richeditor
