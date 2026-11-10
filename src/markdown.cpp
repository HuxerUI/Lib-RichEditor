#include <huxerui/richeditor/format.h>
#include <huxerui/richeditor/math.h>

#include <array>
#include <stdexcept>
#include <tuple>

#include <huxerui/markdown/document.h>

#include "document_internal.h"

namespace huxerui::richeditor {
namespace {

using markdown::MarkdownNode;
using markdown::NodeKind;
using detail::BlockPath;

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

class MarkdownImport {
public:
  explicit MarkdownImport(MarkdownOptions options) : options_(options) {}

  ImportResult Run(std::string_view source, LossPolicy policy) {
    auto options = markdown::ParseOptions::CommonMark();
    options.tables = true;
    options.task_lists = true;
    options.strikethrough = true;
    options.math = options_.display_math || options_.inline_math;
    try {
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
    case NodeKind::InlineMath:
      if (options_.inline_math) {
        builder.AppendObject(LatexInline(node.text), marks);
        return;
      }
      [[fallthrough]];
    case NodeKind::DisplayMath:
      Report("markdown.math", "Formula converted to text because this Markdown math syntax is disabled or unsupported here");
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
          if (section.kind == NodeKind::TableHead && cell.alignment != markdown::CellAlignment::Default) {
            Report("markdown.table_alignment", "Column alignment omitted");
          }
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
        auto source = detail::NormalizeLineEndings(node.children.front().text);
        if (source.starts_with(' ') && source.ends_with(' ') && source.size() >= 2) {
          // Lib-Markdown folds line breaks around a display span into spaces.
          source = source.substr(1, source.size() - 2);
        }
        block.kind = BlockKind::Custom;
        block.custom = LatexBlock(std::move(source));
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
      if (node.task_checked) {
        Report("markdown.task", "Task marker converted to literal text");
        block.children.insert(block.children.begin(), Block(BlockData{.text = *node.task_checked ? "[x]" : "[ ]"}));
      }
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
      if (options_.display_math && block.fence_info == "math") {
        if (path_.size() == 1) {
          block = BlockData{.kind = BlockKind::Custom, .custom = LatexBlock(std::move(block.text))};
          break;
        }
        Report("markdown.math", "Nested display math converted to a code block");
      }
      if (block.fence_info.find('\n') != std::string::npos) {
        Report("markdown.fence_info", "Line breaks in code fence info replaced by spaces");
        std::ranges::replace(block.fence_info, '\n', ' ');
      }
      break;
    case NodeKind::ThematicBreak:
      Report("markdown.rule", "Thematic break converted to literal text");
      block.text = "---";
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
  MarkdownOptions options_;
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

std::string MathFence(std::string_view source) {
  const std::string fence(std::max<std::size_t>(3, LongestRun(source, '~') + 1), '~');
  return fence + "math\n" + std::string(source) + "\n" + fence;
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
  explicit MarkdownWriter(MarkdownOptions options) : options_(options) {}

  ExportResult Run(const RichDocument& document, LossPolicy policy) {
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
    if (round_trip.document != projected && policy == LossPolicy::AllowLoss && !drop_inline_at_) {
      BlockPath mismatch;
      if (round_trip.document) {
        FindMismatch(projected.Blocks(), round_trip.document->Blocks(), mismatch);
      } else if (!round_trip.issues.empty()) {
        mismatch = round_trip.issues.front().block_path;
      }
      MarkdownWriter fallback(options_);
      fallback.drop_inline_at_ = std::move(mismatch);
      auto result = fallback.Run(document, policy);
      if (fallback.dropped_encodable_) {
        return result;
      }
    }
    if (!round_trip.document) {
      issues_.push_back({"markdown.round_trip", {}, {}, "Generated Markdown could not be re-imported without loss"});
    } else if (projected != *round_trip.document) {
      BlockPath mismatch;
      FindMismatch(projected.Blocks(), round_trip.document->Blocks(), mismatch);
      issues_.push_back({"markdown.round_trip", std::move(mismatch), {},
                         "This structure, text, or mark combination has no faithful encoding in the Markdown subset"});
    }
    if (issues_.empty() || policy == LossPolicy::AllowLoss) {
      return {std::move(text), std::move(issues_)};
    }
    return {.issues = std::move(issues_)};
  }

private:
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
        const bool latex = custom.type == latex_block_type && custom.version == 1;
        if (!(latex && options_.display_math && custom == LatexBlock(custom.payload))) {
          issues_.push_back({latex ? "markdown.math" : "markdown.custom_block", path_, {},
                             "This block has no faithful Markdown encoding. AllowLoss writes its fallback text."});
          data = BlockData{.text = custom.fallback_text};
        }
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
      const bool latex = value.type == latex_block_type;
      const auto image = ReadInlineImage(value);
      const bool code = std::ranges::any_of(data.marks, [&](const MarkRange& mark) {
        return mark.range.start <= object.offset && object.offset < mark.range.end &&
            detail::Has(mark.marks, Mark::Code);
      });
      const bool standalone_image = path_.size() == 1 && data.kind == BlockKind::Paragraph &&
          body.Length() == 1 && data.marks.empty() && data.links.empty();
      const bool representable = !code &&
          ((latex && options_.inline_math && value == LatexInline(value.payload) && !value.payload.empty() &&
            value.payload.find_first_of(in_cell ? "$\n|" : "$\n") == value.payload.npos) ||
           (options_.inline_images && image && !image->width && value == InlineImage(*image) && !standalone_image));
      const bool fallback = drop_inline_at_ && path_.size() >= drop_inline_at_->size() &&
          std::equal(drop_inline_at_->begin(), drop_inline_at_->end(), path_.begin());
      if (representable && !fallback) {
        auto copy = object;
        copy.offset += shift;
        retained.push_back(std::move(copy));
        continue;
      }
      dropped_encodable_ |= representable;
      issues_.push_back({latex ? "markdown.math" : "markdown.inline_object", path_,
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

  std::string WriteMath(const CustomBlockData& custom) const {
    if (custom.payload.find('\n') == std::string::npos) {
      const auto span = "$$\n" + custom.payload + "\n$$";
      const auto imported = MarkdownImport(options_).Run(span, LossPolicy::Strict);
      if (imported.document && imported.document->Blocks().size() == 1 &&
          imported.document->Blocks().front().Data().custom == custom) {
        return span;
      }
    }
    return MathFence(custom.payload);
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
        if (object->data.type == latex_block_type) {
          text += "$" + object->data.payload + "$";
        } else if (const auto image = ReadInlineImage(object->data)) {
          text += "![" + EscapeText(image->alt_text, protect_) + "](<" + EscapeAttribute(image->source, in_table) + ">";
          if (!image->title.empty()) {
            text += " \"" + EscapeAttribute(image->title, in_table) + '"';
          }
          text += ')';
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
          out += " --- |";
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
        out += WriteMath(*data.custom);
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
      case BlockKind::CodeBlock: {
        const std::string fence(std::max<std::size_t>(3, LongestRun(data.text, '~') + 1), '~');
        out += fence;
        if (!data.fence_info.empty()) {
          out += ' ';
          out += EscapeAttribute(data.fence_info);
        }
        out += '\n';
        if (!data.text.empty()) {
          out += data.text + '\n';
        }
        out += fence;
        break;
      }
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
          PrefixLines(out, Blocks(data.children[j].Data().children, data.tight), marker, std::string(marker.size(), ' '));
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
  std::optional<BlockPath> drop_inline_at_;
  bool dropped_encodable_ = false;
  MarkdownOptions options_;
};

class MarkdownDocumentCodec final : public DocumentCodec {
public:
  explicit MarkdownDocumentCodec(MarkdownOptions options) : options_(options) {}

  ImportResult Import(std::string_view source, LossPolicy policy) const override {
    return MarkdownImport(options_).Run(source, policy);
  }

  ExportResult Export(const RichDocument& document, LossPolicy policy) const override {
    return MarkdownWriter(options_).Run(document, policy);
  }

private:
  MarkdownOptions options_;
};

} // namespace

const DocumentCodec& MarkdownCodec(MarkdownOptions options) {
  static const std::array codecs{
      MarkdownDocumentCodec({}), MarkdownDocumentCodec({.display_math = true}),
      MarkdownDocumentCodec({.inline_math = true}), MarkdownDocumentCodec({.display_math = true, .inline_math = true}),
      MarkdownDocumentCodec({.inline_images = true}),
      MarkdownDocumentCodec({.display_math = true, .inline_images = true}),
      MarkdownDocumentCodec({.inline_math = true, .inline_images = true}),
      MarkdownDocumentCodec({.display_math = true, .inline_math = true, .inline_images = true}),
  };
  return codecs[options.display_math + 2 * options.inline_math + 4 * options.inline_images];
}

} // namespace huxerui::richeditor
