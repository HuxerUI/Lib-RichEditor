#include <huxerui/richeditor/format.h>
#include <huxerui/richeditor/extension.h>
#include <huxerui/richeditor/math.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::richeditor;

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
  const auto& codec = MarkdownCodec({.inline_math = true, .inline_images = true});
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
  Check(!markdown.Import("- [x] Done\n").document, "Strict Markdown detects unsupported task lists");
  const auto empty = RichDocument::Create({Block(BlockData{.text = "A"}), Block(BlockData{}), Block(BlockData{.text = "B"})});
  Check(!markdown.Export(empty).text && markdown.Export(empty, LossPolicy::AllowLoss).text.has_value(),
        "Empty paragraph loss is reported");
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

  constexpr auto aligned = "| A | B |\n| :--- | ---: |\n| x | y |\n";
  const auto aligned_strict = markdown.Import(aligned);
  const auto aligned_lossy = markdown.Import(aligned, LossPolicy::AllowLoss);
  Check(!aligned_strict.document && aligned_lossy.document && aligned_strict.issues.size() == 2 &&
            aligned_strict.issues[0].block_path == std::vector<std::size_t>{0, 0, 0, 0},
        "Unsupported column alignment has a source cell diagnostic");
  Check(!markdown.Import("> | A |\n> | --- |\n> | B |\n").document, "Tables are top-level");
  Check(!json.Import(R"({"version":1,"blocks":[{"type":"table","has_header":true,"children":[
      {"type":"table_row","children":[{"type":"table_cell","children":[{"type":"paragraph","text":""}]}]},
      {"type":"table_row","children":[{"type":"table_cell","children":[{"type":"paragraph","text":""}]},
                                    {"type":"table_cell","children":[{"type":"paragraph","text":""}]}]}
  ]}]})", LossPolicy::AllowLoss).document, "JSON rejects ragged tables even under AllowLoss");
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
  Check(!strict.text && strict.issues.size() == 1 && strict.issues[0].code == "markdown.math" &&
            strict.issues[0].block_path == std::vector<std::size_t>{0}, "CommonMark reports formula loss");
  const auto lossy = markdown.Export(document, LossPolicy::AllowLoss);
  Check(lossy.text && markdown.Import(*lossy.text).document->Blocks()[0].Data().text == source &&
            !lossy.issues.empty(), "AllowLoss exports literal source and diagnostics");

  const auto& math = MarkdownCodec({.display_math = true});
  const auto encoded = math.Export(document);
  Check(encoded.text && encoded.text->starts_with("$$\n") && math.Import(*encoded.text).document == document,
        "Opt-in display math round trips through the Markdown parser");
  const auto multiline = LatexBlock("x + y % note\n+ z");
  const auto multi = RichDocument::Create({Block(BlockData{.kind = BlockKind::Custom, .custom = multiline})});
  const auto fenced = math.Export(multi);
  Check(fenced.text && fenced.text->starts_with("~~~math\n") && math.Import(*fenced.text).document == multi,
        "Math fences preserve line breaks that affect the meaning of LaTeX source");
  Check(!math.Import("Text $x$ and more.").document && math.Import("Text $x$ and more.", LossPolicy::AllowLoss).document,
        "The opt-in codec reports unsupported inline math");
  const auto currency = math.Import("A price of \\$5 and \\$10.");
  Check(currency.document && math.Export(*currency.document).text, "Literal dollars stay literal with math enabled");

  const CustomBlockData unknown{"example.attachment", 7, R"({"path":"attachments/a.png"})", "Attachment A"};
  const auto custom = RichDocument::Create({Block(BlockData{.kind = BlockKind::Custom, .custom = unknown})});
  Check(json.Import(*json.Export(custom).text).document == custom && !math.Export(custom).text &&
            math.Export(custom, LossPolicy::AllowLoss).issues[0].code == "markdown.custom_block",
        "Unknown custom types preserve their version and payload and report Markdown loss");
}

} // namespace

int main() {
  try {
    RoundTrip();
    InlineObjectCodecs();
    ValidateLoss();
    ValidateJsonAndBoundaries();
    DefaultHighlighting();
    TableCodecs();
    ImageCodecs();
    CustomBlockCodecs();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
