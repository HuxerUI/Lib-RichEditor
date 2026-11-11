#include "appearance.h"

#include <huxerui/navigation.h>
#include <huxerui/presentation.h>

using namespace huxerui;

namespace folio {
namespace {

constexpr Color Rgb(std::uint32_t rgb) { return Color::Rgba32((rgb << 8U) | 0xFFU); }

constexpr ColorScheme light{
    .primary = Rgb(0x2C5844),
    .on_primary = Rgb(0xF7FCF8),
    .primary_container = Rgb(0xDFEBE2),
    .on_primary_container = Rgb(0x244634),
    .secondary = Rgb(0x556259),
    .on_secondary = Rgb(0xFDFCF8),
    .secondary_container = Rgb(0xE4EAE2),
    .on_secondary_container = Rgb(0x222B25),
    .tertiary_container = Rgb(0xF1E7D1),
    .on_tertiary_container = Rgb(0x503914),
    .background = Rgb(0xF1F2ED),
    .surface = Rgb(0xFDFCF8),
    .surface_container_low = Rgb(0xECEFE8),
    .surface_container = Rgb(0xF7F8F3),
    .surface_container_high = Rgb(0xE4EAE2),
    .surface_container_highest = Rgb(0xE8EDE5),
    .on_surface = Rgb(0x222B25),
    .on_surface_variant = Rgb(0x556259),
    .outline = Rgb(0xD7DDD4),
    .inverse_surface = Rgb(0x2A342E),
    .inverse_on_surface = Rgb(0xEEF3ED),
    .scrim = Color::Rgb(20, 31, 25, 0.30F),
    .error = Rgb(0x9C3933),
};

constexpr ColorScheme dark{
    .primary = Rgb(0xAAD0B9),
    .on_primary = Rgb(0x15271E),
    .primary_container = Rgb(0x304438),
    .on_primary_container = Rgb(0xD0E8D8),
    .secondary = Rgb(0xAEBAB1),
    .on_secondary = Rgb(0x1B211E),
    .secondary_container = Rgb(0x2E3932),
    .on_secondary_container = Rgb(0xE7ECE7),
    .tertiary_container = Rgb(0x51432D),
    .on_tertiary_container = Rgb(0xF3DEAE),
    .background = Rgb(0x1B211E),
    .surface = Rgb(0x242B27),
    .surface_container_low = Rgb(0x1D2420),
    .surface_container = Rgb(0x212824),
    .surface_container_high = Rgb(0x2E3932),
    .surface_container_highest = Rgb(0x2D3932),
    .on_surface = Rgb(0xE7ECE7),
    .on_surface_variant = Rgb(0xAEBAB1),
    .outline = Rgb(0x38443D),
    .inverse_surface = Rgb(0xE3ECE5),
    .inverse_on_surface = Rgb(0x202923),
    .scrim = Color::Rgb(7, 12, 9, 0.58F),
    .error = Rgb(0xF2A8A4),
};

Color WithOpacity(Color color, float opacity) {
  color.alpha *= opacity;
  return color;
}

} // namespace

ThemeDefinition FolioTheme(ThemeBrightness brightness) {
  auto theme = brightness == ThemeBrightness::Dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  theme.colors = brightness == ThemeBrightness::Dark ? dark : light;
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
      .corner_radii = CornerRadii{shape.small},
      .indication = theme.interactions.indication,
  });
  definition.Set(IconButtonStyle{
      .foreground = colors.on_surface_variant,
      .disabled_foreground = WithOpacity(colors.on_surface_variant, 0.44F),
      .icon_size = control_icon_size,
      .minimum_interactive_size = 30.0F,
      .state_layer_size = 27.0F,
      .corner_radius = shape.extra_small,
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
      .disabled_foreground = WithOpacity(colors.on_surface_variant, 0.42F),
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

View PrimaryAction(View button, const ThemeSpec& theme) {
  return std::move(button).With(Background(theme.colors.primary), Foreground(theme.colors.on_primary));
}

View DialogPanel(View content, const ThemeSpec& theme, float width) {
  return std::move(content).With(
      Frame{.width = width}, Padding(20.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.surface), Border{theme.colors.outline, 1.0F}, CornerRadius(theme.shapes.large),
      Shadow{Color::Rgb(0, 0, 0, 0.21F), {0.0F, 8.0F}, 22.0F, 0.0F});
}

} // namespace folio
