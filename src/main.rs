mod accel;
mod app;
mod commands;
mod fsstat;
mod gpu;
mod history;
mod netaddr;
mod proc_list;
mod procfs;
mod theme;
mod ui;

use std::io;

use crossterm::event::{self, Event, KeyEventKind};
use crossterm::execute;
use crossterm::terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen};
use ratatui::backend::CrosstermBackend;
use ratatui::Terminal;

use app::App;

fn main() -> io::Result<()> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    let mut app = App::new();
    app.tick();

    let mut last_tick = std::time::Instant::now();
    loop {
        terminal.draw(|f| ui::draw(f, &app))?;

        let timeout = app.tick_interval.saturating_sub(last_tick.elapsed());
        if event::poll(timeout)? {
            if let Event::Key(key) = event::read()? {
                if key.kind == KeyEventKind::Press {
                    app.handle_key(key.code);
                }
            }
        }
        if last_tick.elapsed() >= app.tick_interval {
            app.tick();
            last_tick = std::time::Instant::now();
        }
        if app.should_quit {
            break;
        }
    }

    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}

/// Real verification via ratatui's own TestBackend - a real styled
/// cell-buffer capture, not just unstyled text - so a bug like "this
/// bar renders blank once color is actually involved" (a real,
/// embarrassing blind spot the original Go version hit for one whole
/// release, because its own verification path went through a non-tty
/// capture that auto-stripped color) can't hide here.
#[cfg(test)]
mod tests {
    use super::*;
    use ratatui::backend::TestBackend;

    #[test]
    fn top_row_renders_with_real_styled_content() {
        let mut app = App::new();
        app.tick();
        std::thread::sleep(std::time::Duration::from_millis(1100));
        app.tick(); // a second real tick so history-based graphs have >=2 real samples

        let backend = TestBackend::new(200, 60);
        let mut terminal = Terminal::new(backend).unwrap();
        terminal.draw(|f| ui::draw(f, &app)).unwrap();

        let buffer = terminal.backend().buffer().clone();
        let mut text = String::new();
        for y in 0..buffer.area().height {
            for x in 0..buffer.area().width {
                text.push_str(buffer[(x, y)].symbol());
            }
            text.push('\n');
        }

        // Real content assertions - the panel titles and at least one
        // real bar-fill glyph must actually be present, not blank.
        assert!(text.contains("CPU"), "CPU panel title missing:\n{text}");
        assert!(text.contains("RAM"), "RAM panel title missing:\n{text}");
        assert!(text.contains("DISK"), "DISK panel title missing:\n{text}");
        assert!(text.contains("NET"), "NET panel title missing:\n{text}");
        assert!(text.contains("PROC"), "PROC panel title missing:\n{text}");
        // CONTAINERS was removed entirely (2026-08-29, "containers
        // jebac cnie... to jest jednak calkowicie zbedne") - real,
        // deliberate removal, not a regression this test should guard.
        assert!(
            text.contains('█') || text.contains('▁'),
            "expected at least one real bar/graph glyph somewhere on screen:\n{text}"
        );

        // Real style check (this is the part a plain-text-only capture
        // would miss entirely): at least one cell must carry a real,
        // non-default foreground color - confirms styling actually
        // reached the buffer, not just glyphs.
        let mut saw_color = false;
        for y in 0..buffer.area().height {
            for x in 0..buffer.area().width {
                if buffer[(x, y)].fg != ratatui::style::Color::Reset {
                    saw_color = true;
                    break;
                }
            }
        }
        assert!(saw_color, "no styled (non-default-color) cell found in the rendered buffer");
    }
}
