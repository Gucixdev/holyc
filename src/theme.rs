//! One real theme ("theme zrobimy tylko jedno") - a green-on-black
//! "matrix" aesthetic: green is the always-on default text color, red
//! is reserved for genuine warnings, yellow for medium severity, purple
//! for borders/headers/accents/selected state. No configurable theme
//! system, no light variant yet - a real light mode is a secondary
//! concern that can follow later if actually wanted.

use ratatui::style::Color;

pub const PRIMARY: Color = Color::Rgb(0, 255, 65); // green - default/active text, OK-tier
pub const WARN: Color = Color::Rgb(255, 0, 0); // red - genuine warning/critical only
pub const CAUTION: Color = Color::Rgb(255, 255, 0); // yellow - medium severity
pub const ACCENT: Color = Color::Rgb(153, 68, 255); // purple - borders/headers/accents/selected
pub const NEUTRAL: Color = Color::Rgb(180, 180, 180); // light gray - "empty/available capacity", never used for a warning tier
pub const DISABLED: Color = Color::Rgb(120, 90, 160); // dim muted purple - unavailable/disabled

/// tier_color - real, consistent severity coloring shared by every
/// section: green under 60%, yellow 60-85%, red above 85%. The one
/// place this threshold lives.
pub fn tier_color(pct: f64) -> Color {
    if pct > 85.0 {
        WARN
    } else if pct > 60.0 {
        CAUTION
    } else {
        PRIMARY
    }
}

/// RAM's five real, fully-saturated, widely-separated segment colors -
/// used/zram/cache/swap/free. No gray anywhere (a first attempt used a
/// muted palette that still read as blending together; this is the
/// palette that was actually accepted).
pub const RAM_USED: Color = Color::Rgb(0, 217, 255); // cyan
pub const RAM_ZRAM: Color = Color::Rgb(255, 46, 196); // magenta
pub const RAM_CACHE: Color = Color::Rgb(255, 174, 0); // amber
pub const RAM_SWAP: Color = Color::Rgb(255, 59, 59); // red
pub const RAM_FREE: Color = Color::Rgb(124, 77, 255); // violet
