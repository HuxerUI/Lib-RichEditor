#include <huxerui/richeditor/format.h>
#include <huxerui/richeditor/extension.h>

#include "../examples/preview/src/formula.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::richeditor;
using folio::LatexBlock;
using folio::LatexInline;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

constexpr std::string_view fixture = R"markdown(# Heading

Hello **bold *italic*** and ~~struck~~, `code`, 中文🙂. [A **link**](<https://example.org/a_(b)> "Title")

3. Parent
   - Child
   - Other
4. Sibling

> An editable quote
>
> A second paragraph

~~~cpp additional info
const auto x = "```";

~~~
)markdown";

void RoundTrip() {
  const auto& markdown = MarkdownCodec();
  const auto& json = JsonCodec();
  const auto imported = markdown.Import(fixture);
  Check(imported.document.has_value() && imported.issues.empty(), "Markdown fixture import");
  const auto blocks = imported.document->Blocks();
  Check(blocks.size() == 5, "Top-level block count");
  Check(blocks[0].Data().heading_level == 1, "Heading level");
  Check(blocks[1].Data().links.size() == 1 && blocks[1].Data().links[0].title == "Title", "Link metadata");
  const auto& list = blocks[2].Data();
  Check(list.kind == BlockKind::List && list.ordered && list.list_start == 3, "Ordered list metadata");
  Check(list.children[0].Data().children[1].Data().kind == BlockKind::List, "Nested list");
  Check(blocks[3].Data().kind == BlockKind::Quote && blocks[3].Data().children.size() == 2, "Quote children");
  Check(blocks[4].Data().fence_info == "cpp additional info" && blocks[4].Data().text.ends_with('\n'), "Code metadata");
  const auto saved = json.Export(*imported.document);
  Check(saved.text && saved.issues.empty(), "JSON export preserves the complete document");
  const auto reopened = json.Import(*saved.text);
  Check(reopened.document == imported.document && reopened.issues.empty(), "JSON round trip");
  const auto encoded = markdown.Export(*reopened.document);
  if (!encoded.issues.empty()) {
    throw std::runtime_error(encoded.issues.front().message);
  }
  Check(encoded.text && markdown.Import(*encoded.text).document == imported.document, "Markdown semantic round trip");
}

void InlineObjectCodecs() {
  const auto& codec = folio::DocumentMarkdown();
  const auto imported = codec.Import("A🙂 $x^2$ 中文 ![map](attachments/map.png) and [**$y$**](https://example.org).");
  Check(imported.document.has_value() && imported.issues.empty(), "Import math and images embedded in text");
  const auto& paragraph = imported.document->Blocks().front().Data();
  Check(paragraph.inline_objects.size() == 3 && paragraph.inline_objects[0].offset == 4 &&
            paragraph.inline_objects[0].data == LatexInline("x^2") &&
            ReadInlineImage(paragraph.inline_objects[1].data)->source == "attachments/map.png",
        "Inline math and images use UTF-16 offsets and structured attachment references");
  const auto json = JsonCodec().Export(*imported.document);
  Check(json.text && JsonCodec().Import(*json.text).document == imported.document, "JSON v1 retains inline objects");
  const auto markdown = codec.Export(*imported.document);
  Check(markdown.text && codec.Import(*markdown.text).document == imported.document,
        "Strict Markdown round trips inline math, images, marks and links");

  const InlineObjectData unknown{"folio.mention", 2, R"({"id":"42"})", "Alice", {{"avatar", "attachments/a.png"}}};
  const auto custom = RichDocument::Create({Block(BlockData{.text = "Hi \xef\xbf\xbc" "!",
      .marks = {{{3, 4}, Mark::Bold}}, .inline_objects = {{3, unknown}}})});
  const auto saved = JsonCodec().Export(custom);
  Check(saved.text && JsonCodec().Import(*saved.text).document == custom, "Unknown objects keep payload and attachments");
  const auto strict = codec.Export(custom);
  const auto lossy = codec.Export(custom, LossPolicy::AllowLoss);
  Check(!strict.text && strict.issues.front().range == TextRange{3, 4} &&
            lossy.text && lossy.text->find("**Alice**") != std::string::npos,
        "Lossy Markdown replaces an object with styled fallback text and reports its original range");
  Check(!JsonCodec().Import(R"({"version":1,"blocks":[{"type":"paragraph","text":"A","inline_objects":[
      {"offset":0,"type":"folio.mention","version":1,"payload":""}]}]})").document,
        "An inline object must reference a replacement character");
}

void ValidateLoss() {
  const auto& markdown = MarkdownCodec();
  const auto& json = JsonCodec();
  const auto document = RichDocument::Create({Block(BlockData{
      .text = "A🙂好",
      .marks = {{{0, 1}, Mark::Underline}, {{1, 4}, Mark::Bold | Mark::Highlight}},
  })});
  const auto saved = json.Export(document);
  Check(saved.text && saved.issues.empty() && json.Import(*saved.text).document == document,
        "JSON preserves editor-only styles");
  const auto json_allow_loss = json.Export(document, LossPolicy::AllowLoss);
  Check(json_allow_loss.text == saved.text && json_allow_loss.issues.empty(), "JSON is lossless under either policy");
  const auto strict = markdown.Export(document);
  Check(!strict.text && strict.issues.size() == 2 && strict.issues[0].code == "markdown.underline" &&
            strict.issues[1].code == "markdown.highlight" && strict.issues[0].block_path == std::vector<std::size_t>{0},
        "Strict Markdown loss reporting");
  const auto lossy = markdown.Export(document, LossPolicy::AllowLoss);
  Check(lossy.text && lossy.issues.size() == 2 && lossy.issues[1].range == TextRange{1, 4}, "Explicit Markdown loss");
  const auto returned = markdown.Import(*lossy.text);
  Check(returned.document && returned.document->Blocks()[0].Data().marks == std::vector<MarkRange>{{{1, 4}, Mark::Bold}},
        "Markdown keeps representable styles");
  const auto strict_image = markdown.Import("See ![alt](url)");
  Check(!strict_image.document && strict_image.issues.size() == 1 && strict_image.issues[0].code == "markdown.image",
        "Strict Markdown detects unsupported images");
  const auto image = markdown.Import("See ![alt](url)", LossPolicy::AllowLoss);
  Check(image.document && image.issues.size() == 1 && image.issues[0].code == "markdown.image" &&
            image.document->Blocks()[0].Data().text == "See alt",
        "Explicit image fallback");
  const auto empty = RichDocument::Create({Block(BlockData{.text = "A"}), Block(BlockData{}), Block(BlockData{.text = "B"})});
  Check(!markdown.Export(empty).text && markdown.Export(empty, LossPolicy::AllowLoss).text.has_value(),
        "Empty paragraph loss is reported");
}

void TaskListCodecs() {
  const MarkdownCodec markdown;
  const auto& json = JsonCodec();
  const auto imported = markdown.Import(R"md(7. [X] **中文🙂**
   - [ ] Child
   - \[x\] Literal
8. Ordinary
9. [ ]
10. [x]
    - [ ] Only child
)md");
  Check(imported.document && imported.issues.empty(), "Strict imports mixed, ordered and nested task items");
  const auto& list = imported.document->Blocks().front().Data();
  const auto& items = list.children;
  const auto& first = items[0].Data();
  const auto& nested = first.children[1].Data().children;
  Check(list.ordered && list.list_start == 7 && items.size() == 4 && first.task_checked == true &&
            first.children[0].Data().text == "中文🙂" &&
            first.children[0].Data().marks == std::vector<MarkRange>{{{0, 4}, Mark::Bold}} &&
            nested[0].Data().task_checked == false && !nested[1].Data().task_checked &&
            nested[1].Data().children[0].Data().text == "[x] Literal" && !items[1].Data().task_checked &&
            items[2].Data().task_checked == false && items[2].Data().children[0].Data().text.empty() &&
            items[3].Data().task_checked == true && items[3].Data().children[0].Data().kind == BlockKind::List,
        "Task markers are independent of numbering, children, text and UTF-16 marks");
  const auto saved = json.Export(*imported.document);
  Check(saved.text && saved.text->find("\"version\": 1") != std::string::npos &&
            saved.text->find("\"task_checked\": false") != std::string::npos &&
            saved.text->find("\"task_checked\": true") != std::string::npos &&
            json.Import(*saved.text).document == imported.document,
        "JSON v1 round trips all task states");
  const auto exported = markdown.Export(*imported.document);
  Check(exported.text && exported.issues.empty() && exported.text->find("7. [x] ") != std::string::npos &&
            markdown.Import(*exported.text).document == imported.document,
        "Strict exports canonical task markers and preserves mixed list structure");

  for (const auto source : {
           "- [ ] First\n\n  Continuation\n\n- [x] Second\n",
           "> - [x] Quoted task\n",
           "- [x]\n  # Heading\n",
           "- [ ]\n  > Quote\n",
           "- [x]\n  ```txt\n  Code\n  ```\n",
           "- [ ]\n  ***\n",
           "- [ ]\n\n  - Child\n",
       }) {
    const auto decoded = markdown.Import(source);
    Check(decoded.document.has_value(), "Import task with continuation or structural child blocks");
    const auto encoded = markdown.Export(*decoded.document);
    if (!encoded.text || markdown.Import(*encoded.text).document != decoded.document) {
      throw std::runtime_error(std::string("Task list round trip: ") + source);
    }
  }

  const auto styled = RichDocument::Create({Block(BlockData{.kind = BlockKind::List, .children = {
      Block(BlockData{.kind = BlockKind::ListItem, .children = {
          Block(BlockData{.text = "Done", .marks = {{{0, 4}, Mark::Underline}}})}, .task_checked = true})}})});
  const auto strict = markdown.Export(styled);
  const auto lossy = markdown.Export(styled, LossPolicy::AllowLoss);
  Check(!strict.text && strict.issues.size() == 1 && strict.issues.front().code == "markdown.underline" &&
            strict.issues.front().block_path == std::vector<std::size_t>{0, 0, 0} &&
            lossy.text && lossy.issues.size() == 1 &&
            markdown.Import(*lossy.text).document == markdown.Import("- [x] Done").document,
        "AllowLoss drops the unsupported mark and retains the task");

  const auto legacy = json.Import(R"({"version":1,"blocks":[{"type":"list","ordered":false,"start":1,"tight":true,
      "children":[{"type":"list_item","children":[{"type":"paragraph","text":"Ordinary"}]}]}]})");
  Check(legacy.document && !legacy.document->Blocks()[0].Data().children[0].Data().task_checked &&
            json.Export(*legacy.document).text->find("task_checked") == std::string::npos,
        "Ordinary JSON v1 items omit task metadata");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"paragraph","text":"Wrong","task_checked":false}]})").document &&
            !json.Import(R"({"version":1,"blocks":[{"type":"list","ordered":false,"start":1,"tight":true,
      "children":[{"type":"list_item","task_checked":null,"children":[{"type":"paragraph","text":"Wrong"}]}]}]})").document,
        "JSON accepts task_checked only as a boolean on a list item");
  bool rejected = false;
  try {
    RichDocument::Create({Block(BlockData{.task_checked = false})});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "Document validation rejects a task marker on a non-list-item block");
}

void ImageCodecs() {
  const auto& markdown = MarkdownCodec();
  const auto& json = JsonCodec();
  const auto imported = markdown.Import(R"md(![照片 \[1\] 🙂](<attachments/my photo.jpg> "A & B")

![Remote](https://example.org/photo.png)
)md");
  Check(imported.document && imported.issues.empty(), "Strict imports standalone image blocks");
  const auto blocks = imported.document->Blocks();
  Check(blocks.size() == 2 && blocks[0].Data().kind == BlockKind::Image &&
            blocks[0].Data().image == ImageBlockData{"attachments/my photo.jpg", "照片 [1] 🙂", "A & B"} &&
            blocks[1].Data().image->source == "https://example.org/photo.png",
        "Image block preserves its relative or external reference and metadata");
  const auto saved = json.Export(*imported.document);
  Check(saved.text && saved.text->find("\"version\": 1") != std::string::npos &&
            saved.text->find("\xEF\xBF\xBC") == std::string::npos && json.Import(*saved.text).document == imported.document,
        "JSON v1 preserves image blocks without serializing the position placeholder");
  const auto exported = markdown.Export(*imported.document);
  Check(exported.text && markdown.Import(*exported.text).document == imported.document,
        "Strict Markdown round trips image sources, alternative text, and titles");

  auto resized = blocks.front().Data();
  resized.image->width = 125.5F;
  const auto sized = RichDocument::Create({Block(std::move(resized)), blocks[1]});
  const auto sized_json = json.Export(sized);
  Check(sized_json.text && json.Import(*sized_json.text).document == sized, "JSON v1 preserves preferred image width");
  const auto strict = markdown.Export(sized);
  const auto lossy = markdown.Export(sized, LossPolicy::AllowLoss);
  Check(!strict.text && strict.issues.size() == 1 && strict.issues[0].code == "markdown.image_size" &&
            strict.issues[0].block_path == std::vector<std::size_t>{0},
        "Strict Markdown locates the unrepresentable image dimensions");
  Check(lossy.text && lossy.issues.size() == 1 && lossy.issues[0].code == "markdown.image_size" &&
            markdown.Import(*lossy.text).document == imported.document,
        "AllowLoss discards only the preferred width");
  Check(!markdown.Import("> ![alt](attachments/image.png)").document, "Strict rejects nested images");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"image","source":""}]})").document &&
            !json.Import(R"({"version":1,"blocks":[{"type":"image","source":"a.png","text":"extra"}]})").document,
        "Reject invalid image records");
  for (const std::string_view width : {"0", "-1", "1e100", "1e-100", "true", "\"125\"", "null"}) {
    const auto source = std::string(R"({"version":1,"blocks":[{"type":"image","source":"a.png","width":)") +
        std::string(width) + "}]}";
    Check(!json.Import(source).document, "Reject nonpositive or unrepresentable JSON image widths");
  }
}

void TableCodecs() {
  const auto& markdown = MarkdownCodec();
  const auto& json = JsonCodec();
  const auto imported = markdown.Import(R"markdown(| Key | Value | Source |
| --- | --- | --- |
| **中文🙂** | `a\|b` | [a\|b](<https://example.org/a> "Title") |
| Last |
)markdown");
  Check(imported.document && imported.issues.empty(), "Strict imports a GFM table");
  const auto& table = imported.document->Blocks()[0].Data();
  const auto paragraph = [](const Block& row, std::size_t column) -> const BlockData& {
    return row.Data().children[column].Data().children.front().Data();
  };
  Check(table.kind == BlockKind::Table && table.table_has_header && table.children.size() == 3,
        "GFM header and data rows");
  const auto legacy_json = json.Export(*imported.document);
  Check(table.table_column_alignments.empty() && legacy_json.text && legacy_json.text->find("column_alignments") ==
            std::string::npos, "Default column alignment keeps the original JSON v1 representation");
  Check(paragraph(table.children[1], 0).marks == std::vector<MarkRange>{{{0, 4}, Mark::Bold}} &&
            paragraph(table.children[1], 1).text == "a|b" &&
            paragraph(table.children[1], 2).links.size() == 1 &&
            paragraph(table.children[1], 2).links.front().title == "Title",
        "Table inline marks, code pipes, and links");
  Check(table.children[2].Data().children.size() == 3 && paragraph(table.children[2], 2).text.empty(),
        "GFM pads missing cells");
  const auto strict = markdown.Export(*imported.document);
  Check(strict.text && strict.issues.empty() && markdown.Import(*strict.text).document == imported.document,
        "Strict Markdown table round trip");

  auto headerless = table;
  headerless.table_has_header = false;
  auto row = headerless.children[1].Data();
  row.children[0] = Block(BlockData{.kind = BlockKind::TableCell, .children = {Block(BlockData{
      .text = "甲\n乙🙂", .marks = {{{0, 1}, Mark::Underline}, {{2, 5}, Mark::Bold}},
      .links = {{{2, 5}, "https://example.org", "Title"}},
  })}});
  headerless.children[1] = Block(std::move(row));
  const auto rich = RichDocument::Create({Block(std::move(headerless))});
  const auto stored = json.Export(rich);
  Check(stored.text && stored.text->find("\"version\": 1") != std::string::npos &&
            stored.issues.empty() && json.Import(*stored.text).document == rich,
        "JSON v1 preserves the headerless grid, empty cells, breaks, marks, and links");
  const auto rejected = markdown.Export(rich);
  Check(!rejected.text && rejected.issues.size() == 3, "Strict refuses table losses");
  const auto lossy = markdown.Export(rich, LossPolicy::AllowLoss);
  Check(lossy.text && std::ranges::equal(lossy.issues, rejected.issues, {},
                                        &ConversionIssue::code, &ConversionIssue::code),
        "AllowLoss reports the same changes");
  const auto reopened = markdown.Import(*lossy.text);
  Check(reopened.document.has_value(), "The lossy result is valid GFM");
  const auto& approximate = reopened.document->Blocks()[0].Data();
  Check(approximate.table_has_header && approximate.children.size() == 4 &&
            paragraph(approximate.children[0], 0).text.empty() && paragraph(approximate.children[2], 0).text == "甲 乙🙂",
        "Lossy export adds a blank header and replaces cell breaks");

  using Alignment = TableColumnAlignment;
  constexpr auto aligned = "| A | B | C | D |\n| --- | :--- | :---: | ---: |\n| 甲 | س | 乙 | ص |\n";
  const auto aligned_document = markdown.Import(aligned);
  Check(aligned_document.document && aligned_document.issues.empty() &&
            aligned_document.document->Blocks()[0].Data().table_column_alignments ==
                std::vector{Alignment::Default, Alignment::Left, Alignment::Center, Alignment::Right},
        "Strict imports all alignments, distinguishing explicit left from default");
  const auto aligned_json = json.Export(*aligned_document.document);
  const auto aligned_markdown = markdown.Export(*aligned_document.document);
  Check(aligned_json.text && json.Import(*aligned_json.text).document == aligned_document.document &&
            aligned_markdown.text == aligned && markdown.Import(*aligned_markdown.text).document ==
                aligned_document.document, "Column alignments round trip through JSON and Strict Markdown");
  for (const std::string_view alignment : {"[]", "[\"right\"]", "[\"default\",\"left\",\"center\",\"invalid\"]", "null"}) {
    auto invalid = *aligned_json.text;
    const auto start = invalid.find('[', invalid.find("\"column_alignments\""));
    invalid.replace(start, invalid.find(']', start) + 1 - start, alignment);
    Check(!json.Import(invalid).document, "Invalid JSON column alignment arrays are rejected");
  }
  Check(!markdown.Import("> | A |\n> | --- |\n> | B |\n").document, "Tables are top-level");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"table","has_header":true,"children":[
      {"type":"table_row","children":[{"type":"table_cell","children":[{"type":"paragraph","text":""}]}]},
      {"type":"table_row","children":[{"type":"table_cell","children":[{"type":"paragraph","text":""}]},
                                    {"type":"table_cell","children":[{"type":"paragraph","text":""}]}]}
  ]}]})", LossPolicy::AllowLoss).document, "JSON rejects ragged tables even under AllowLoss");
}

void ThematicBreakCodecs() {
  const auto& markdown = MarkdownCodec();
  const auto source = markdown.Import("Title\n---\n\n* * *\n\n> ___\n\n- Before\n  ***\n  After\n");
  Check(source.document && source.issues.empty(), "Strict imports top-level and nested thematic breaks");
  const auto blocks = source.document->Blocks();
  Check(blocks.size() == 4 && blocks[3].Data().kind == BlockKind::List, "Thematic break fixture structure");
  const auto& item = blocks[3].Data().children.front().Data().children;
  Check(blocks[0].Data().kind == BlockKind::Heading &&
            blocks[1].Data().kind == BlockKind::ThematicBreak &&
            blocks[2].Data().children.front().Data().kind == BlockKind::ThematicBreak &&
            item.size() == 3 && item[1].Data().kind == BlockKind::ThematicBreak,
        "Setext headings and thematic breaks have distinct structure");
  const auto encoded = markdown.Export(*source.document);
  const auto saved = JsonCodec().Export(*source.document);
  Check(encoded.text && encoded.text->find("> ***") != std::string::npos &&
            markdown.Import(*encoded.text).document == source.document && saved.text &&
            saved.text->find("\"version\": 1") != std::string::npos &&
            JsonCodec().Import(*saved.text).document == source.document,
        "Thematic breaks preserve their parent through Strict Markdown and JSON v1");
  Check(!JsonCodec().Import(R"({"version":1,"blocks":[{"type":"thematic_break","text":""}]})").document,
        "A thematic break cannot carry text");
  Check(!JsonCodec().Import(R"({"version":1,"blocks":[{"type":"table","has_header":true,"children":[
      {"type":"table_row","children":[{"type":"table_cell","children":[{"type":"thematic_break"}]}]}
  ]}]})").document, "Thematic breaks cannot replace table cell paragraphs");

  const auto body = markdown.Import("***\n\nBody").document;
  Check(body.has_value(), "Front matter body fixture imports");
  const auto check_front_matter = [&](std::string_view input) {
    const auto strict = markdown.Import(input);
    const auto lossy = markdown.Import(input, LossPolicy::AllowLoss);
    Check(!strict.document && strict.issues.size() == 1 && strict.issues.front().code == "markdown.front_matter" &&
              lossy.issues.size() == 1 && lossy.issues.front().code == "markdown.front_matter" &&
              lossy.document == body,
          "YAML front matter is explicitly diagnosed and only AllowLoss omits it");
  };
  check_front_matter("\xef\xbb\xbf---\r\ntitle: Example\r\ntags:\r\n- one\r\n---\r\n\r\n***\r\n\r\nBody");
  for (const std::string_view mapping : {"title : Example", R"("title": Example)", "标题: 示例",
                                        R"md("title: \"example\"" : Example)md", "'post''s: title': Example"}) {
    check_front_matter("---\n" + std::string(mapping) + "\n---\n\n***\n\nBody");
  }
  const auto prose = markdown.Import("---\n\nOrdinary prose\n\n---");
  Check(prose.document && prose.document->Blocks().size() == 3 &&
            prose.document->Blocks().front().Data().kind == BlockKind::ThematicBreak &&
            prose.document->Blocks().back().Data().kind == BlockKind::ThematicBreak,
        "Leading and trailing thematic breaks remain valid around ordinary prose");
  const auto quoted_prose = markdown.Import("---\n\"Title: example\"\n---");
  Check(quoted_prose.document && quoted_prose.issues.empty() &&
            quoted_prose.document->Blocks().size() == 2 &&
            quoted_prose.document->Blocks().front().Data().kind == BlockKind::ThematicBreak &&
            quoted_prose.document->Blocks().back().Data().kind == BlockKind::Heading,
        "A colon inside a quoted heading is not a YAML key separator");
}

void ValidateJsonAndBoundaries() {
  const auto& json = JsonCodec();
  const auto& markdown = MarkdownCodec();
  const auto blank = json.Export(RichDocument{});
  Check(blank.text && json.Import(*blank.text).document == RichDocument{}, "Blank document round trip");
  const auto unknown = json.Import(R"({"version":2,"blocks":[]})", LossPolicy::AllowLoss);
  Check(!unknown.document && unknown.issues.size() == 1 && unknown.issues[0].code == "json.version",
        "Unknown JSON version is rejected under AllowLoss");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"attachment"}]})").document, "Unknown JSON node");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"paragraph","text":"🙂","marks":[
      {"start":1,"end":2,"marks":["bold"]}]}]})").document, "Split surrogate rejected");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"heading","text":"Title","level":1.5}]})").document,
        "Floating-point level rejected");
  Check(!json.Import(R"({"version":1,"blocks":[
      {"type":"list_item","children":[{"type":"paragraph","text":""}]}]})").document,
        "Invalid tree shape rejected");
  const auto literal = markdown.Import("<em>literal</em>  \nnew line");
  Check(literal.document && literal.document->Blocks()[0].Data().text == "<em>literal</em>\nnew line",
        "Literal HTML and hard breaks");
  const auto exported = markdown.Export(*literal.document);
  Check(exported.text && markdown.Import(*exported.text).document == literal.document, "Hard break round trip");
  const auto line = markdown.Import("ordinary\nnewline");
  Check(line.document && line.document->Blocks()[0].Data().text == "ordinary newline", "Soft break normalization");
}

void DefaultHighlighting() {
  const auto& highlight = DefaultCodeBlockHighlighting().highlight;
  const AttributedText code("\"中文🙂\"; int count = 42;\n/* first\nsecond */\nreturn count;");
  const auto light = highlight(code.PlainText(), "cpp", Color::White());
  const auto dark = highlight(code.PlainText(), "cpp", Color::Black());
  Check(!light.empty() && dark.size() == light.size(), "Bundled C++ profile");
  const auto token = [&](std::string_view text) {
    return std::ranges::find_if(light, [&](const auto& span) { return code.TextInRange(span.range) == text; });
  };
  Check(token("int") != light.end() && token("42") != light.end() && token("return") != light.end(),
        "Tokens after an emoji and across lines use UTF-16 offsets");
  Check(token("/* first") != light.end() && token("second */") != light.end(),
        "SweetLine keeps multiline comment state");
  for (std::size_t i = 0; i < light.size(); ++i) {
    Check(light[i].range == dark[i].range && light[i].foreground != dark[i].foreground,
          "Palette follows the background without changing the highlighted ranges");
  }
  Check(highlight(code.PlainText(), "plain", Color::White()).empty(), "Unsupported languages display as plain text");
}

void CustomBlockCodecs() {
  constexpr std::string_view source = R"(\frac{1}{2} + x^2)";
  const auto formula = LatexBlock(std::string(source));
  const auto document = RichDocument::Create({Block(BlockData{.kind = BlockKind::Custom, .custom = formula})});
  const auto& json = JsonCodec();
  const auto json_text = json.Export(document).text;
  Check(json_text && json.Import(*json_text).document == document, "JSON v1 preserves a formula without a renderer");

  const auto& markdown = MarkdownCodec();
  const auto strict = markdown.Export(document);
  Check(!strict.text && strict.issues.size() == 1 && strict.issues[0].code == "markdown.custom_block" &&
            strict.issues[0].block_path == std::vector<std::size_t>{0}, "CommonMark reports formula loss");
  const auto lossy = markdown.Export(document, LossPolicy::AllowLoss);
  Check(lossy.text && markdown.Import(*lossy.text).document->Blocks()[0].Data().text == source &&
            !lossy.issues.empty(), "AllowLoss exports literal source and diagnostics");

  const auto& math = folio::DocumentMarkdown();
  const auto from_span = math.Import("$$\n" + std::string(source) + "\n$$");
  Check(from_span.document == document, "Folio imports a display span using the existing formula type");
  const auto encoded = math.Export(document);
  Check(encoded.text && encoded.text->starts_with("~~~ math\n") && math.Import(*encoded.text).document == document,
        "Display math exports as a fence and round trips through the Markdown parser");
  const auto multiline = LatexBlock("x + y % note\n+ z");
  const auto multi = RichDocument::Create({Block(BlockData{.kind = BlockKind::Custom, .custom = multiline})});
  const auto fenced = math.Export(multi);
  Check(fenced.text && fenced.text->starts_with("~~~ math\n") && math.Import(*fenced.text).document == multi,
        "Math fences preserve line breaks that affect the meaning of LaTeX source");
  const auto legacy = json.Import(R"json({"version":1,"blocks":[
    {"type":"custom","custom_type":"huxerui.latex","version":1,"payload":"x^2","fallback_text":"x^2"},
    {"type":"paragraph","text":"A\uFFFC","inline_objects":[
      {"offset":1,"type":"huxerui.latex","version":1,"payload":"y","fallback_text":"y"}]}
  ]})json");
  Check(legacy.document && json.Import(*json.Export(*legacy.document).text).document == legacy.document,
        "Old formula data round trips in JSON without a registered extension");
  const auto legacy_export = math.Export(*legacy.document);
  Check(legacy_export.text && math.Import(*legacy_export.text).document == legacy.document,
        "Folio's registered mappings can export previously saved formula objects");
  auto block_math = folio::MarkdownMath();
  block_math.inline_object.reset();
  const MarkdownCodec block_only({.extensions = {block_math}});
  const auto missing_inline = block_only.Import("Text $x$ and more.");
  const auto plain_inline = block_only.Import("Text $x$ and more.", LossPolicy::AllowLoss);
  Check(!missing_inline.document && missing_inline.issues[0].code == "markdown.extension_missing" &&
            plain_inline.document && plain_inline.document->Blocks()[0].Data().text == "Text x and more.",
        "The shared parser flag does not enable an unregistered inline mapping");
  auto inline_math = folio::MarkdownMath();
  inline_math.block.reset();
  const MarkdownCodec inline_only({.extensions = {inline_math}});
  const auto missing_display = inline_only.Import("$$\nx\n$$");
  Check(!missing_display.document && missing_display.issues[0].code == "markdown.extension_missing" &&
            inline_only.Import("$$\nx\n$$", LossPolicy::AllowLoss).document,
        "The shared parser flag does not enable an unregistered block mapping");
  block_math.block->encode = [](const CustomBlockData& block) {
    return MarkdownObjectText{MarkdownObjectSyntax::DoubleDollarBlock(), block.payload};
  };
  const MarkdownCodec dollar_display({.extensions = {block_math}});
  const auto dollar_export = dollar_display.Export(document);
  Check(dollar_export.text && dollar_export.text->starts_with("$$\n") &&
            dollar_display.Import(*dollar_export.text).document == document,
        "An extension can choose display math instead of a fence for the same object");
  const auto currency = math.Import("A price of \\$5 and \\$10.");
  Check(currency.document && math.Export(*currency.document).text, "Literal dollars stay literal with math enabled");

  const CustomBlockData unknown{"example.attachment", 7, R"({"path":"attachments/a.png"})", "Attachment A"};
  const auto custom = RichDocument::Create({Block(BlockData{.kind = BlockKind::Custom, .custom = unknown})});
  Check(json.Import(*json.Export(custom).text).document == custom && !math.Export(custom).text &&
            math.Export(custom, LossPolicy::AllowLoss).issues[0].code == "markdown.custom_block",
        "Unknown custom types preserve their version and payload and report Markdown loss");
}

void MarkdownObjectMappings() {
  const auto diagram_fence = MarkdownObjectSyntax::FencedBlock("diagram flow");
  const auto diagram = MarkdownExtension{
      .block = MarkdownObjectMapping<CustomBlockData>{
          .type = "example.diagram",
          .syntax = {diagram_fence},
          .decode = [](const MarkdownObjectText& source) -> std::optional<CustomBlockData> {
            if (!source.text.starts_with("graph ")) {
              return std::nullopt;
            }
            return CustomBlockData{"example.diagram", 1, source.text, "Diagram"};
          },
          .encode = [diagram_fence](const CustomBlockData& data) -> std::optional<MarkdownObjectText> {
            if (data.version != 1) {
              return std::nullopt;
            }
            return MarkdownObjectText{diagram_fence, data.payload};
          },
      },
  };
  const MarkdownCodec base;
  const MarkdownCodec codec({.extensions = {diagram, folio::MarkdownMath()}});
  constexpr auto source = "~~~~diagram flow\ngraph A\n~~~\nB\n\n~~~~";
  const auto code = base.Import(source);
  const auto imported = codec.Import(source);
  Check(code.document && code.document->Blocks()[0].Data().kind == BlockKind::CodeBlock &&
            imported.document && imported.document->Blocks()[0].Data().custom ==
                CustomBlockData{"example.diagram", 1, "graph A\n~~~\nB\n", "Diagram"},
        "A codec claims only its own exact fence info and preserves the literal body");
  const auto exported = codec.Export(*imported.document);
  Check(exported.text && codec.Import(*exported.text).document == imported.document,
        "The codec chooses a fence longer than a run inside the body");
  Check(codec.Import("~~~diagram\ngraph A\n~~~").document->Blocks()[0].Data().kind == BlockKind::CodeBlock,
        "A language prefix alone does not match the registered fence info");
  Check(!codec.Export(*code.document).text &&
            codec.Export(*code.document).issues[0].code == "markdown.extension_conflict",
        "Strict refuses to serialize a regular code block that would reopen as an object");
  const auto plain_code = codec.Export(*code.document, LossPolicy::AllowLoss);
  const auto reopened = plain_code.text ? codec.Import(*plain_code.text).document : std::nullopt;
  Check(reopened && reopened->Blocks()[0].Data().kind == BlockKind::CodeBlock &&
            reopened->Blocks()[0].Data().text == code.document->Blocks()[0].Data().text &&
            reopened->Blocks()[0].Data().fence_info.empty(),
        "AllowLoss drops the conflicting info and keeps the code text");

  const auto declined = codec.Import("~~~diagram flow\nunsupported\n~~~");
  const auto declined_lossy = codec.Import("~~~diagram flow\nunsupported\n~~~", LossPolicy::AllowLoss);
  Check(!declined.document && declined.issues[0].code == "markdown.extension_import" &&
            declined_lossy.document && declined_lossy.document->Blocks()[0].Data().kind == BlockKind::CodeBlock,
        "A declined decode reports a loss and preserves the original code under AllowLoss");
  const auto nested = codec.Import("> ~~~diagram flow\n> graph A\n> ~~~");
  Check(!nested.document && nested.issues[0].code == "markdown.extension_placement",
        "Nested fences cannot become top-level custom objects");

  const auto mismatched = RichDocument::Create({
      Block(BlockData{.kind = BlockKind::Custom,
          .custom = CustomBlockData{"example.diagram", 1, "graph A", "Unmapped label"}}),
      imported.document->Blocks()[0],
      Block(BlockData{.kind = BlockKind::Custom,
          .custom = CustomBlockData{"example.diagram", 1, "graph B", "Another label"}}),
  });
  const auto lossy = codec.Export(mismatched, LossPolicy::AllowLoss);
  const auto lossy_document = lossy.text ? codec.Import(*lossy.text).document : std::nullopt;
  Check(!codec.Export(mismatched).text && lossy_document && lossy_document->Blocks().size() == 3 &&
            lossy_document->Blocks()[0].Data().text == "Unmapped label" &&
            lossy_document->Blocks()[1] == imported.document->Blocks()[0] &&
            lossy_document->Blocks()[2].Data().text == "Another label" && lossy.issues.size() == 2 &&
            lossy.issues[0].block_path == std::vector<std::size_t>{0} &&
            lossy.issues[1].block_path == std::vector<std::size_t>{2},
        "AllowLoss degrades mismatched blocks while retaining valid objects in other blocks");

  const auto invalid_registration = [](MarkdownOptions options) {
    try {
      (void)MarkdownCodec(std::move(options));
      return false;
    } catch (const std::invalid_argument&) {
      return true;
    }
  };
  auto conflict = diagram;
  conflict.block->type = "other.diagram";
  Check(invalid_registration({.extensions = {diagram, conflict}}), "A syntax can only have one owner");
  conflict = diagram;
  conflict.block->syntax = {MarkdownObjectSyntax::FencedBlock("other")};
  Check(invalid_registration({.extensions = {diagram, conflict}}), "Each block type has one mapping");
  conflict.block->syntax = {MarkdownObjectSyntax::DollarSpan()};
  Check(invalid_registration({.extensions = {conflict}}), "A block mapping cannot claim inline syntax");
  conflict.block->syntax = {MarkdownObjectSyntax::FencedBlock("")};
  Check(invalid_registration({.extensions = {conflict}}), "A fenced-block mapping requires nonempty info");
}

} // namespace

int main() {
  try {
    RoundTrip();
    InlineObjectCodecs();
    ValidateLoss();
    ValidateJsonAndBoundaries();
    DefaultHighlighting();
    TaskListCodecs();
    TableCodecs();
    ThematicBreakCodecs();
    ImageCodecs();
    CustomBlockCodecs();
    MarkdownObjectMappings();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
