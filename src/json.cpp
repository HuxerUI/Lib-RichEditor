#include <huxerui/richeditor/format.h>

#include <array>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "document_internal.h"

namespace huxerui::richeditor {
namespace {

using Json = nlohmann::json;

constexpr std::array block_names{
    "paragraph", "heading", "code_block",
    "quote", "list", "list_item", "table", "table_row", "table_cell",
    "thematic_break", "image", "custom",
};
constexpr std::array column_alignment_names{"default", "left", "center", "right"};
constexpr std::array mark_names{
    std::pair{Mark::Bold, "bold"}, std::pair{Mark::Italic, "italic"}, std::pair{Mark::Strike, "strike"},
    std::pair{Mark::Code, "code"}, std::pair{Mark::Underline, "underline"}, std::pair{Mark::Highlight, "highlight"},
};

void CheckFields(const Json& object, std::initializer_list<std::string_view> fields) {
  if (!object.is_object()) {
    throw std::invalid_argument("Expected a JSON object");
  }
  for (const auto& [key, value] : object.items()) {
    if (std::ranges::find(fields, key) == fields.end()) {
      throw std::invalid_argument("Unknown JSON field: " + key);
    }
  }
}

std::uint64_t Unsigned(const Json& value, std::uint64_t max) {
  if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
      value.get<std::uint64_t>() > max) {
    throw std::invalid_argument("Invalid unsigned JSON integer");
  }
  return value.get<std::uint64_t>();
}

TextRange DecodeRange(const Json& value) {
  constexpr auto max = std::numeric_limits<TextOffset>::max();
  return {static_cast<TextOffset>(Unsigned(value.at("start"), max)),
          static_cast<TextOffset>(Unsigned(value.at("end"), max))};
}

Block DecodeBlock(const Json& object);

std::vector<Block> DecodeBlocks(const Json& array) {
  if (!array.is_array()) {
    throw std::invalid_argument("Expected a JSON block array");
  }
  std::vector<Block> blocks;
  blocks.reserve(array.size());
  for (const auto& child : array) {
    blocks.push_back(DecodeBlock(child));
  }
  return blocks;
}

Block DecodeBlock(const Json& object) {
  const auto type = object.at("type").get<std::string>();
  const auto found = std::ranges::find(block_names, type);
  if (found == block_names.end()) {
    throw std::invalid_argument("Unknown JSON block type: " + type);
  }
  BlockData data;
  data.kind = static_cast<BlockKind>(std::distance(block_names.begin(), found));
  switch (data.kind) {
  case BlockKind::Image: {
    CheckFields(object, {"type", "source", "alt_text", "title", "width"});
    data.image = ImageBlockData{
        object.at("source").get<std::string>(), object.value("alt_text", std::string{}),
        object.value("title", std::string{})};
    if (const auto width = object.find("width"); width != object.end()) {
      if (!width->is_number()) {
        throw std::invalid_argument("Expected a numeric image width");
      }
      const auto value = width->get<double>();
      if (value <= 0.0 || value > std::numeric_limits<float>::max()) {
        throw std::invalid_argument("Image width must be finite and positive");
      }
      data.image->width = static_cast<float>(value);
    }
    return Block(std::move(data));
  }
  case BlockKind::Custom:
    CheckFields(object, {"type", "custom_type", "version", "payload", "fallback_text"});
    data.custom = CustomBlockData{
        object.at("custom_type").get<std::string>(),
        static_cast<unsigned>(Unsigned(object.at("version"), std::numeric_limits<unsigned>::max())),
        object.at("payload").get<std::string>(),
        object.value("fallback_text", std::string{})};
    return Block(std::move(data));
  case BlockKind::ThematicBreak:
    CheckFields(object, {"type"});
    return Block(std::move(data));
  case BlockKind::Paragraph:
    CheckFields(object, {"type", "text", "marks", "links", "inline_objects"});
    break;
  case BlockKind::Heading:
    CheckFields(object, {"type", "text", "marks", "links", "level", "inline_objects"});
    data.heading_level = static_cast<unsigned>(Unsigned(object.at("level"), 6));
    break;
  case BlockKind::CodeBlock:
    CheckFields(object, {"type", "text", "info"});
    data.fence_info = object.value("info", std::string{});
    break;
  case BlockKind::Quote:
  case BlockKind::TableRow:
  case BlockKind::TableCell:
    CheckFields(object, {"type", "children"});
    break;
  case BlockKind::ListItem:
    CheckFields(object, {"type", "children", "task_checked"});
    if (const auto checked = object.find("task_checked"); checked != object.end()) {
      if (!checked->is_boolean()) {
        throw std::invalid_argument("Expected a JSON boolean for task_checked");
      }
      data.task_checked = checked->get<bool>();
    }
    break;
  case BlockKind::Table:
    CheckFields(object, {"type", "children", "has_header", "column_alignments"});
    data.table_has_header = object.at("has_header").get<bool>();
    if (const auto alignments = object.find("column_alignments"); alignments != object.end()) {
      if (!alignments->is_array() || alignments->empty()) {
        throw std::invalid_argument("Expected a JSON array of table column alignments");
      }
      for (const auto& alignment : *alignments) {
        const auto name = alignment.get<std::string>();
        const auto found_alignment = std::ranges::find(column_alignment_names, name);
        if (found_alignment == column_alignment_names.end()) {
          throw std::invalid_argument("Unknown JSON table column alignment: " + name);
        }
        data.table_column_alignments.push_back(
            static_cast<TableColumnAlignment>(std::distance(column_alignment_names.begin(), found_alignment)));
      }
    }
    break;
  case BlockKind::List:
    CheckFields(object, {"type", "children", "ordered", "start", "tight"});
    data.ordered = object.at("ordered").get<bool>();
    data.list_start = static_cast<unsigned>(Unsigned(object.at("start"), std::numeric_limits<unsigned>::max()));
    data.tight = object.at("tight").get<bool>();
    break;
  }

  if (!detail::IsLeaf(data.kind)) {
    data.children = DecodeBlocks(object.at("children"));
    return Block(std::move(data));
  }
  data.text = object.at("text").get<std::string>();
  if (object.contains("marks")) {
    const auto& marks = object.at("marks");
    if (!marks.is_array()) {
      throw std::invalid_argument("Expected a JSON mark range array");
    }
    for (const auto& entry : marks) {
      CheckFields(entry, {"start", "end", "marks"});
      const auto& names = entry.at("marks");
      if (!names.is_array()) {
        throw std::invalid_argument("Expected a JSON mark name array");
      }
      Mark value = Mark::None;
      for (const auto& name : names) {
        const auto found_mark = std::ranges::find(mark_names, name.get<std::string>(), &decltype(mark_names)::value_type::second);
        if (found_mark == mark_names.end() || detail::Has(value, found_mark->first)) {
          throw std::invalid_argument("Unknown or duplicate JSON mark");
        }
        value = value | found_mark->first;
      }
      data.marks.push_back({DecodeRange(entry), value});
    }
  }
  if (object.contains("links")) {
    const auto& links = object.at("links");
    if (!links.is_array()) {
      throw std::invalid_argument("Expected a JSON link array");
    }
    for (const auto& link : links) {
      CheckFields(link, {"start", "end", "target", "title"});
      data.links.push_back({DecodeRange(link), link.at("target").get<std::string>(), link.value("title", std::string{})});
    }
  }
  if (const auto objects = object.find("inline_objects"); objects != object.end()) {
    if (!objects->is_array()) {
      throw std::invalid_argument("Expected a JSON inline object array");
    }
    for (const auto& entry : *objects) {
      CheckFields(entry, {"offset", "type", "version", "payload", "fallback_text", "attachments"});
      InlineObjectData content{
          entry.at("type").get<std::string>(),
          static_cast<unsigned>(Unsigned(entry.at("version"), std::numeric_limits<unsigned>::max())),
          entry.at("payload").get<std::string>(), entry.value("fallback_text", std::string{})};
      if (const auto attachments = entry.find("attachments"); attachments != entry.end()) {
        if (!attachments->is_array()) {
          throw std::invalid_argument("Expected a JSON inline attachment array");
        }
        for (const auto& attachment : *attachments) {
          CheckFields(attachment, {"role", "source"});
          content.attachments.push_back(
              {attachment.at("role").get<std::string>(), attachment.at("source").get<std::string>()});
        }
      }
      const auto offset = Unsigned(entry.at("offset"), std::numeric_limits<TextOffset>::max());
      data.inline_objects.emplace_back(static_cast<TextOffset>(offset), std::move(content));
    }
  }
  return Block(std::move(data));
}

Json EncodeBlock(const Block& block);

Json EncodeBlocks(std::span<const Block> blocks) {
  Json array = Json::array();
  for (const auto& block : blocks) {
    array.push_back(EncodeBlock(block));
  }
  return array;
}

Json EncodeBlock(const Block& block) {
  const auto& data = block.Data();
  Json object = {{"type", block_names[static_cast<std::size_t>(data.kind)]}};
  if (data.image) {
    object["source"] = data.image->source;
    object["alt_text"] = data.image->alt_text;
    if (!data.image->title.empty()) {
      object["title"] = data.image->title;
    }
    if (data.image->width) {
      object["width"] = *data.image->width;
    }
  } else if (data.custom) {
    object["custom_type"] = data.custom->type;
    object["version"] = data.custom->version;
    object["payload"] = data.custom->payload;
    object["fallback_text"] = data.custom->fallback_text;
  } else if (detail::IsTextBlock(data.kind)) {
    object["text"] = data.text;
  } else if (!detail::IsLeaf(data.kind)) {
    object["children"] = EncodeBlocks(data.children);
  }
  if (data.kind == BlockKind::Heading) {
    object["level"] = data.heading_level;
  }
  if (data.kind == BlockKind::CodeBlock) {
    object["info"] = data.fence_info;
  }
  if (data.kind == BlockKind::List) {
    object["ordered"] = data.ordered;
    object["start"] = data.list_start;
    object["tight"] = data.tight;
  }
  if (data.task_checked.has_value()) {
    object["task_checked"] = *data.task_checked;
  }
  if (data.kind == BlockKind::Table) {
    object["has_header"] = data.table_has_header;
    if (!data.table_column_alignments.empty()) {
      auto& alignments = object["column_alignments"] = Json::array();
      for (const auto alignment : data.table_column_alignments) {
        alignments.push_back(column_alignment_names[static_cast<std::size_t>(alignment)]);
      }
    }
  }
  if (!data.marks.empty()) {
    auto& ranges = object["marks"] = Json::array();
    for (const auto& run : data.marks) {
      Json names = Json::array();
      for (const auto& [mark, name] : mark_names) {
        if (detail::Has(run.marks, mark)) {
          names.push_back(name);
        }
      }
      ranges.push_back({{"start", run.range.start}, {"end", run.range.end}, {"marks", std::move(names)}});
    }
  }
  if (!data.links.empty()) {
    auto& links = object["links"] = Json::array();
    for (const auto& link : data.links) {
      links.push_back({{"start", link.range.start}, {"end", link.range.end},
                       {"target", link.target}, {"title", link.title}});
    }
  }
  if (!data.inline_objects.empty()) {
    auto& objects = object["inline_objects"] = Json::array();
    for (const auto& object : data.inline_objects) {
      Json entry{{"offset", object.offset}, {"type", object.data.type}, {"version", object.data.version},
                 {"payload", object.data.payload}, {"fallback_text", object.data.fallback_text}};
      if (!object.data.attachments.empty()) {
        auto& attachments = entry["attachments"] = Json::array();
        for (const auto& attachment : object.data.attachments) {
          attachments.push_back({{"role", attachment.role}, {"source", attachment.source}});
        }
      }
      objects.push_back(std::move(entry));
    }
  }
  return object;
}

class JsonDocumentCodec final : public DocumentCodec {
public:
  ImportResult Import(std::string_view source, LossPolicy) const override;
  ExportResult Export(const RichDocument& document, LossPolicy) const override;
};

ImportResult JsonDocumentCodec::Import(std::string_view source, LossPolicy) const {
  try {
    const auto object = Json::parse(source, [](int depth, Json::parse_event_t, Json&) {
      if (depth > static_cast<int>(2 * detail::max_nesting + 8)) {
        throw std::invalid_argument("JSON nesting limit exceeded");
      }
      return true;
    });
    CheckFields(object, {"version", "blocks"});
    const auto version = Unsigned(object.at("version"), std::numeric_limits<std::uint64_t>::max());
    if (version != 1) {
      return {.issues = {{"json.version", {}, {}, "Unsupported RichEditor JSON version"}}};
    }
    return {.document = RichDocument::Create(DecodeBlocks(object.at("blocks")))};
  } catch (const Json::exception& error) {
    return {.issues = {{"json.invalid", {}, {}, error.what()}}};
  } catch (const std::invalid_argument& error) {
    return {.issues = {{"json.invalid", {}, {}, error.what()}}};
  }
}

ExportResult JsonDocumentCodec::Export(const RichDocument& document, LossPolicy) const {
  return {.text = Json{{"version", 1}, {"blocks", EncodeBlocks(document.Blocks())}}.dump(2) + "\n"};
}

} // namespace

const DocumentCodec& JsonCodec() {
  static const JsonDocumentCodec codec;
  return codec;
}

} // namespace huxerui::richeditor
