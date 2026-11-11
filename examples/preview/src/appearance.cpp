#include "appearance.h"

#include <algorithm>
#include <array>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <huxerui/navigation.h>
#include <huxerui/presentation.h>

using namespace huxerui;

namespace folio {
namespace {

constexpr Color Rgb(std::uint32_t rgb) { return Color::Rgba32((rgb << 8U) | 0xFFU); }

struct Palette {
  std::uint32_t canvas, paper, sidebar, chrome, control, code;
  std::uint32_t ink, muted, border, accent, selection, highlight;
};

constexpr ThemePreset Preset(std::string_view id, std::string_view name, ThemeBrightness brightness, Palette palette) {
  const bool dark = brightness == ThemeBrightness::Dark;
  const auto on_accent = Rgb(dark ? palette.canvas : 0xFFFFFF);
  return {id, name, brightness, {
      .primary = Rgb(palette.accent),
      .on_primary = on_accent,
      .primary_container = Rgb(palette.selection),
      .on_primary_container = Rgb(palette.ink),
      .secondary = Rgb(palette.muted),
      .on_secondary = on_accent,
      .secondary_container = Rgb(palette.control),
      .on_secondary_container = Rgb(palette.ink),
      .tertiary_container = Rgb(palette.highlight),
      .on_tertiary_container = Rgb(palette.ink),
      .background = Rgb(palette.canvas),
      .surface = Rgb(palette.paper),
      .surface_container_low = Rgb(palette.sidebar),
      .surface_container = Rgb(palette.chrome),
      .surface_container_high = Rgb(palette.control),
      .surface_container_highest = Rgb(palette.code),
      .on_surface = Rgb(palette.ink),
      .on_surface_variant = Rgb(palette.muted),
      .outline = Rgb(palette.border),
      .inverse_surface = Rgb(palette.ink),
      .inverse_on_surface = Rgb(palette.paper),
      .scrim = Color::Rgb(0, 0, 0, dark ? 0.55F : 0.30F),
      .error = Rgb(dark ? 0xF8B4B1 : 0x9C3933),
  }};
}

constexpr auto light = ThemeBrightness::Light;
constexpr auto dark = ThemeBrightness::Dark;
constexpr auto grove = Preset("grove", "Grove", light, {
    .canvas = 0xEAF0E7, .paper = 0xFAFCF7, .sidebar = 0xEEF2EB, .chrome = 0xF5F8F2,
    .control = 0xE4EBE0, .code = 0xEAF0E6, .ink = 0x222F27, .muted = 0x526359,
    .border = 0xD1DBD1, .accent = 0x34634D, .selection = 0xD8E9DE, .highlight = 0xF1E4BC,
});
constexpr auto evergreen = Preset("evergreen", "Evergreen", dark, {
    .canvas = 0x141E1A, .paper = 0x1F2B25, .sidebar = 0x18231E, .chrome = 0x1C2822,
    .control = 0x2C3C33, .code = 0x26372E, .ink = 0xE2EBE3, .muted = 0xAFBDB3,
    .border = 0x3A4A40, .accent = 0xA6D0B7, .selection = 0x304D3D, .highlight = 0x54462B,
});
constexpr std::array presets{
    grove,
    Preset("porcelain", "Porcelain", light, {
        .canvas = 0xECF0F6, .paper = 0xFCFDFF, .sidebar = 0xEFF2F8, .chrome = 0xF5F7FC,
        .control = 0xE3E9F3, .code = 0xEDF0F9, .ink = 0x252D3C, .muted = 0x535F75,
        .border = 0xD2DAE7, .accent = 0x42568E, .selection = 0xDFE6FC, .highlight = 0xF7E8C5,
    }),
    Preset("parchment", "Parchment", light, {
        .canvas = 0xF3EDE2, .paper = 0xFFFBF2, .sidebar = 0xF5EEE2, .chrome = 0xFAF5EA,
        .control = 0xEEE4D3, .code = 0xF5EDE0, .ink = 0x362C22, .muted = 0x685949,
        .border = 0xE0D4C0, .accent = 0x80512A, .selection = 0xF2E0C4, .highlight = 0xF5E6AF,
    }),
    Preset("lilac", "Lilac", light, {
        .canvas = 0xF0EAF2, .paper = 0xFEFAFE, .sidebar = 0xF3EDF5, .chrome = 0xF9F4FA,
        .control = 0xECE3EF, .code = 0xF1EAF4, .ink = 0x362B3B, .muted = 0x69596F,
        .border = 0xDFD3E4, .accent = 0x744F80, .selection = 0xEBDCF0, .highlight = 0xF4E4BB,
    }),
    evergreen,
    Preset("graphite", "Graphite", dark, {
        .canvas = 0x18191D, .paper = 0x222429, .sidebar = 0x1D1F24, .chrome = 0x202227,
        .control = 0x30333A, .code = 0x2C2F36, .ink = 0xE9EAF0, .muted = 0xB8BBC5,
        .border = 0x3E424B, .accent = 0xB9C3E7, .selection = 0x3B435B, .highlight = 0x524735,
    }),
    Preset("midnight", "Midnight", dark, {
        .canvas = 0x111B27, .paper = 0x192837, .sidebar = 0x152131, .chrome = 0x172535,
        .control = 0x293C50, .code = 0x213449, .ink = 0xE4EDF6, .muted = 0xAEBDD0,
        .border = 0x34485C, .accent = 0x94CFE6, .selection = 0x2D4C60, .highlight = 0x56462D,
    }),
    Preset("ember", "Ember", dark, {
        .canvas = 0x1D1816, .paper = 0x2A2320, .sidebar = 0x231D1B, .chrome = 0x27211E,
        .control = 0x3A2F2B, .code = 0x342925, .ink = 0xF2E7DE, .muted = 0xC6B5AA,
        .border = 0x493A34, .accent = 0xEEB38F, .selection = 0x543A30, .highlight = 0x5A472C,
    }),
};

const ThemePreset* FindPreset(std::string_view id) {
  const auto it = std::ranges::find(presets, id, &ThemePreset::id);
  return it == presets.end() ? nullptr : &*it;
}

Color WithOpacity(Color color, float opacity) {
  color.alpha *= opacity;
  return color;
}

class DialogSections final : public Layout<DialogSections> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    auto limits = constraints.Loose();
    if (constraints.HasBoundedWidth()) {
      limits = limits.TightWidth(constraints.max_width);
    }
    auto& header = node.ChildAt(0);
    auto& body = node.ChildAt(1);
    auto* actions = node.ChildCount() > 2 ? &node.ChildAt(2) : nullptr;
    const auto top = context.Measure(header, limits);
    const auto bottom = actions ? context.Measure(*actions, limits) : Size{};
    const float gap = node.Spacing();
    limits.max_height = std::max(0.0F, limits.max_height - top.height - bottom.height - gap * (actions ? 2 : 1));
    const auto middle = context.Measure(body, limits);
    const float body_y = top.height + gap;
    const float actions_y = body_y + middle.height + (actions ? gap : 0.0F);
    LayoutResult result;
    result.Place(header, {}).Place(body, {0.0F, body_y});
    if (actions) {
      result.Place(*actions, {0.0F, actions_y});
    }
    return result.SetSize(constraints.Constrain({
        std::max({top.width, middle.width, bottom.width}), actions_y + bottom.height,
    }));
  }
};

} // namespace

std::span<const ThemePreset> ThemePresets() { return presets; }

const ThemePreset& ResolveTheme(std::string_view id, ThemeBrightness system_brightness) {
  if (const auto* preset = FindPreset(id)) {
    return *preset;
  }
  return system_brightness == ThemeBrightness::Dark ? evergreen : grove;
}

ThemeDefinition FolioTheme(const ThemePreset& preset) {
  auto theme = preset.brightness == ThemeBrightness::Dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  theme.colors = preset.colors;
  theme.typography = {.body_large = 16.0F, .body_medium = 14.0F, .body_small = 12.0F,
                      .label_large = 13.0F, .title_large = 19.0F, .headline_small = 28.0F};
  theme.shapes = {.extra_small = 5.0F, .small = 7.0F, .medium = 8.0F, .large = 12.0F,
                  .extra_large = 16.0F};
  const auto& colors = theme.colors;
  const auto& shape = theme.shapes;
  theme.interactions.indication = {
      .hover = IndicationLayer{.fill = WithOpacity(colors.on_surface, 0.07F)},
      .press = IndicationLayer{.fill = WithOpacity(colors.on_surface, 0.13F)},
      .ripple = RippleEffect{.color = WithOpacity(colors.primary, 0.18F)},
  };
  theme.interactions.focus_ring = {colors.primary, 2.0F, 2.0F};

  auto definition = MaterialThemeDefinition(theme);
  definition.Set(ButtonStyle{
      .background = colors.surface_container_high,
      .label_style = TextStyle{
          Font::FromFamily(theme.typography.family, theme.typography.label_large).WithWeight(FontWeight::Medium),
          colors.on_surface},
      .disabled_background = WithOpacity(colors.on_surface, 0.06F),
      .disabled_label = WithOpacity(colors.on_surface, 0.42F),
      .padding = EdgeInsets::Symmetric(12.0F, 6.0F),
      .minimum_height = 34.0F,
      .corner_radii = CornerRadii{17.0F},
      .indication = theme.interactions.indication,
  });
  definition.Set(IconButtonStyle{
      .foreground = colors.on_surface_variant,
      .disabled_foreground = WithOpacity(colors.on_surface_variant, 0.44F),
      .icon_size = control_icon_size,
      .minimum_interactive_size = 30.0F,
      .state_layer_size = 28.0F,
      .corner_radius = 14.0F,
      .indication = theme.interactions.indication,
  });
  definition.Set(DividerStyle{.color = colors.outline});
  definition.Set(MenuStyle{
      .background = colors.surface,
      .foreground = colors.on_surface,
      .icon_tint = colors.on_surface_variant,
      .item_indication = theme.interactions.indication,
      .separator_color = colors.outline,
      .separator_mode = MenuSeparatorMode::BetweenSections,
      .separator_padding = EdgeInsets::Symmetric(6.0F, 3.0F),
      .content_padding = EdgeInsets::All(5.0F),
      .item_padding = EdgeInsets::Symmetric(10.0F, 6.0F),
      .icon_size = control_icon_size,
      .corner_radii = CornerRadii{shape.medium},
      .minimum_item_height = 33.0F,
  });
  definition.Set(TreeViewStyle{
      .foreground = colors.on_surface_variant,
      .disabled_foreground = colors.on_surface_variant,
      .selected_background = colors.primary_container,
      .active_background = colors.surface_container_high,
      .focus_indicator = colors.primary,
      .indentation = 15.0F,
      .indicator_size = 12.0F,
      .item_padding = 7.0F,
      .indication = theme.interactions.indication,
  });
  definition.Set(DrawerStyle{
      .background = colors.surface_container_low,
      .scrim = colors.scrim,
      .shadow = Shadow{Color::Transparent()},
      .preferred_width = 248.0F,
      .minimum_width = 180.0F,
      .corner_radius = shape.large,
  });
  definition.Set(SystemBarsAppearance{
      .status_bar_background = colors.surface_container,
      .navigation_bar_background = colors.background,
  });
  return definition;
}

Color PendingEditsColor(ThemeBrightness brightness) {
  return brightness == ThemeBrightness::Dark ? Rgb(0xE2B780) : Rgb(0x935B20);
}

bool IsMobilePlatform() {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS)
  return true;
#else
  return false;
#endif
}

View PrimaryAction(View button, const ThemeSpec& theme) {
  return std::move(button).With(Background(theme.colors.primary), Foreground(theme.colors.on_primary));
}

View DialogPanel(View content, const ThemeSpec& theme, float width) {
  return std::move(content).With(
      Frame{.width = width}, Padding(20.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface), Border{theme.colors.outline, 1.0F}, CornerRadius(theme.shapes.large),
      Shadow{Color::Rgb(0, 0, 0, 0.21F), {}, 22.0F, 0.0F});
}

View ScrollableDialogPanel(View header, View body, View actions, const ThemeSpec& theme, float width) {
  return DialogPanel(DialogSections {header, body, actions}, theme, width).With(ClipChildren());
}

View DialogActions(const ThemeSpec& theme, View cancel, View primary, View secondary, bool destructive) {
  const auto& colors = theme.colors;
  const Frame action_size{.min_height = 44.0F};
  if (cancel) {
    cancel = std::move(cancel).With(action_size, Background(Color::Transparent()), Foreground(colors.on_surface));
  }
  if (primary) {
    primary = PrimaryAction(std::move(primary).With(action_size), theme);
  }
  if (secondary) {
    secondary = std::move(secondary).With(action_size, Background(Color::Transparent()),
        Foreground(destructive ? colors.error : colors.on_surface_variant));
  }
  return Flow {
    secondary,
    Stack {
      Row {cancel, primary}.With(Spacing(6.0F)),
    }.With(Grow(), Align(HorizontalAlignment::End, VerticalAlignment::Center)),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center));
}

} // namespace folio
