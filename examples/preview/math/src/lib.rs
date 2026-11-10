use std::{panic::catch_unwind, ptr, slice, str};

use ratex_layout::{layout, to_display_list, validate_layout, LayoutOptions};
use ratex_parser::parser::parse;
use ratex_render::{render_to_png, RenderOptions};
use ratex_types::{color::Color, MathStyle};

#[repr(C)]
pub struct FormulaPng {
    data: *mut u8,
    size: usize,
    ascent: f32,
    descent: f32,
}

/// Renders borrowed UTF-8 as an owned PNG. The caller releases it with folio_free_formula.
#[no_mangle]
pub unsafe extern "C" fn folio_render_formula(
    source: *const u8,
    size: usize,
    font_size: f32,
    red: f32,
    green: f32,
    blue: f32,
    alpha: f32,
    inline: bool,
) -> FormulaPng {
    let empty = FormulaPng {
        data: ptr::null_mut(),
        size: 0,
        ascent: 0.0,
        descent: 0.0,
    };
    if source.is_null() || size == 0 || size > 4096 {
        return empty;
    }
    let result = catch_unwind(|| {
        let source = str::from_utf8(slice::from_raw_parts(source, size)).ok()?;
        let ast = parse(source).ok()?;
        let mut options = LayoutOptions::default().with_color(Color::new(red, green, blue, alpha));
        if inline {
            options = options.with_style(MathStyle::Text);
        }
        let formula = layout(&ast, &options);
        validate_layout(&formula).ok()?;
        let display = to_display_list(&formula);
        if display.width > 100.0 || display.height + display.depth > 30.0 {
            return None;
        }
        let options = RenderOptions {
            font_size: font_size.clamp(12.0, 32.0),
            padding: if inline { 0.0 } else { 2.0 },
            background_color: Color::new(0.0, 0.0, 0.0, 0.0),
            device_pixel_ratio: 2.0,
            ..Default::default()
        };
        let ascent = display.height as f32 * options.font_size + options.padding;
        let descent = display.depth as f32 * options.font_size + options.padding;
        Some((render_to_png(&display, &options).ok()?, ascent, descent))
    });
    let Ok(Some((png, ascent, descent))) = result else { return empty };
    let size = png.len();
    let data = Box::into_raw(png.into_boxed_slice()) as *mut u8;
    FormulaPng { data, size, ascent, descent }
}

/// Releases a nonempty result returned by folio_render_formula.
#[no_mangle]
pub unsafe extern "C" fn folio_free_formula(png: FormulaPng) {
    if !png.data.is_null() {
        drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
            png.data, png.size,
        )));
    }
}
