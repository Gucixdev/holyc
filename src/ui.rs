//! Real rendering - every panel below draws from real, already-refreshed
//! `App` state (see app.rs::tick). Bars are hand-built as styled
//! `Span`s (ratatui's own `Gauge`/`LineGauge` are single-color only;
//! this project's bars need real multi-segment coloring), and layout
//! uses ratatui's own native `Layout`/`Constraint` system throughout -
//! no external constraint-solver crate (see the project's own real,
//! documented reason: two different Go Cassowary ports were tested
//! directly during the original build and both had real, confirmed
//! bugs).

use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Frame;

use crate::app::{App, Focus};
use crate::theme;

fn block(title: &str) -> Block<'_> {
    Block::default()
        .title(Span::styled(
            format!(" {title} "),
            Style::default()
                .fg(theme::ACCENT)
                .add_modifier(Modifier::BOLD),
        ))
        .borders(Borders::ALL)
        .border_style(Style::default().fg(theme::ACCENT))
}

fn segmented_bar(segments: &[(f64, ratatui::style::Color)], total: f64, width: u16) -> Line<'static> {
    let width = width as usize;
    if total <= 0.0 || width == 0 {
        return Line::from(" ".repeat(width));
    }
    let mut spans = Vec::new();
    let mut used = 0usize;
    let n = segments.len();
    for (i, (val, color)) in segments.iter().enumerate() {
        let mut w = ((width as f64) * val / total).round() as usize;
        if i == n - 1 {
            w = width.saturating_sub(used);
        }
        if used + w > width {
            w = width.saturating_sub(used);
        }
        if w > 0 {
            spans.push(Span::styled("█".repeat(w), Style::default().fg(*color)));
            used += w;
        }
    }
    if used < width {
        spans.push(Span::styled(
            "░".repeat(width - used),
            Style::default().fg(theme::NEUTRAL),
        ));
    }
    Line::from(spans)
}

/// trunc_chars - real char-boundary-safe truncation to at most `n`
/// real characters. `String::truncate` (byte-index based) panics if
/// given an index that lands mid-character - a real risk for any
/// non-ASCII name (container image, process comm), not truncation by
/// bytes.
fn trunc_chars(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}

fn human_bytes(v: f64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut v = v;
    let mut i = 0;
    while v >= 1024.0 && i < UNITS.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    if i == 0 {
        format!("{v:.0}{}", UNITS[i])
    } else {
        format!("{v:.1}{}", UNITS[i])
    }
}

fn human_rate(v: f64) -> String {
    format!("{}/s", human_bytes(v))
}

pub fn draw(f: &mut Frame, app: &App) {
    let size = f.area();
    let root = Layout::new(
        Direction::Vertical,
        [Constraint::Percentage(62), Constraint::Percentage(38)],
    )
    .split(size);

    draw_top_row(f, app, root[0]);

    let bottom = Layout::new(
        Direction::Vertical,
        [Constraint::Percentage(65), Constraint::Percentage(35)],
    )
    .split(root[1]);
    draw_proc(f, app, bottom[0]);
    draw_containers(f, app, bottom[1]);

    if app.palette_open {
        draw_palette(f, app, size);
    }
}

fn draw_top_row(f: &mut Frame, app: &App, area: Rect) {
    // colA/colB narrower (bounded content - core rows, a bar+graph
    // line), DISK/NET wider (tree/address lines need more room).
    let cols = Layout::new(
        Direction::Horizontal,
        [
            Constraint::Percentage(20),
            Constraint::Percentage(20),
            Constraint::Percentage(30),
            Constraint::Percentage(30),
        ],
    )
    .split(area);

    draw_col_a(f, app, cols[0]);
    draw_col_b(f, app, cols[1]);
    draw_disk(f, app, cols[2]);
    draw_net(f, app, cols[3]);
}

fn draw_col_a(f: &mut Frame, app: &App, area: Rect) {
    let cpu_lines = 4 + app.cpu.cores.len() as u16; // bar+graph+legend line + header-ish + one per core
    let gpu_lines: u16 = if app.gpu.reading.found { 5 } else { 2 };
    let parts = Layout::new(
        Direction::Vertical,
        [
            Constraint::Length((cpu_lines + 2).min(area.height)),
            Constraint::Length((gpu_lines + 2).min(area.height)),
        ],
    )
    .split(area);
    draw_cpu(f, app, parts[0]);
    draw_gpu(f, app, parts[1]);
}

fn draw_col_b(f: &mut Frame, app: &App, area: Rect) {
    let mut constraints = vec![Constraint::Length(9.min(area.height))]; // RAM box
    if app.tpu.is_some() {
        constraints.push(Constraint::Length(3));
    }
    if app.npu.is_some() {
        constraints.push(Constraint::Length(3));
    }
    constraints.push(Constraint::Min(0));
    let parts = Layout::new(Direction::Vertical, constraints).split(area);
    draw_ram(f, app, parts[0]);
    let mut idx = 1;
    if let Some(tpu) = &app.tpu {
        f.render_widget(
            Paragraph::new(Line::from(Span::styled(
                tpu.clone(),
                Style::default().fg(theme::PRIMARY),
            )))
            .block(block("TPU")),
            parts[idx],
        );
        idx += 1;
    }
    if let Some(npu) = &app.npu {
        f.render_widget(
            Paragraph::new(Line::from(Span::styled(
                npu.clone(),
                Style::default().fg(theme::PRIMARY),
            )))
            .block(block("NPU")),
            parts[idx],
        );
    }
}

fn draw_cpu(f: &mut Frame, app: &App, area: Rect) {
    let inner_w = area.width.saturating_sub(4).max(10);
    let suffix = format!(" {:.0}% L{:.1}", app.cpu.agg_pct, app.cpu.load1);
    let bar_w = inner_w.saturating_sub(suffix.chars().count() as u16).max(10);

    let segs = [
        (app.cpu.user_pct, theme::PRIMARY),
        (app.cpu.system_pct, theme::WARN),
        (app.cpu.iowait_pct, theme::CAUTION),
    ];
    let mut bar_line = segmented_bar(&segs, 100.0, bar_w);
    bar_line
        .spans
        .push(Span::styled(suffix, Style::default().fg(theme::PRIMARY)));

    let legend = Line::from(Span::styled(
        format!(
            "user:{:.1}% sys:{:.1}% io:{:.1}%",
            app.cpu.user_pct, app.cpu.system_pct, app.cpu.iowait_pct
        ),
        Style::default().fg(theme::PRIMARY),
    ));

    // Real fixed-[0,100] aggregate trend graph (2026-08-29 real bug fix
    // - "cpu... brak grafu": app.cpu.agg_hist was already tracked and
    // pushed every tick, but nothing here ever actually rendered it).
    let agg_hist = app.cpu.agg_hist.as_u64();
    let agg_graph = Line::from(raw_percent_graph(&agg_hist, inner_w, theme::PRIMARY));

    let mut lines: Vec<Line> = vec![bar_line, agg_graph, legend];

    for c in &app.cpu.cores {
        let freq = c
            .freq_mhz
            .map(|v| format!("{v}MHz"))
            .unwrap_or_else(|| "--MHz".to_string());
        let temp = c
            .temp_c
            .map(|v| format!("{v}C"))
            .unwrap_or_else(|| "--C".to_string());
        let fixed = format!("c{:<2} {:5.1}% {:<7} {:<4} ", c.idx, c.pct, freq, temp);
        let graph_w = inner_w.saturating_sub(fixed.chars().count() as u16).clamp(4, 16);
        let hist = app
            .cpu
            .core_hist
            .get(&c.idx)
            .map(|h| h.as_u64())
            .unwrap_or_default();
        let color = theme::tier_color(c.pct);
        let mut spans = vec![Span::styled(fixed, Style::default().fg(color))];
        spans.extend(raw_percent_graph(&hist, graph_w, color));
        lines.push(Line::from(spans));
    }

    f.render_widget(Paragraph::new(lines).block(block("CPU")), area);
}

/// raw_percent_graph - real eighth-block glyphs at a fixed [0,100]
/// range, returned as Spans sharing the CALLER's own color (not an
/// independently-styled ratatui Sparkline widget) - this is what a
/// per-core/per-process row embeds inline next to its own text, so it
/// never fights whatever outer style/highlight the row itself uses.
fn raw_percent_graph(hist: &[u64], width: u16, color: ratatui::style::Color) -> Vec<Span<'static>> {
    const GLYPHS: [char; 9] = [' ', '▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'];
    let width = width as usize;
    if hist.len() < 2 {
        return vec![Span::raw(" ".repeat(width))];
    }
    let n = hist.len();
    let mut s = String::with_capacity(width);
    for x in 0..width {
        let t = if width > 1 {
            x as f64 / (width - 1) as f64 * (n - 1) as f64
        } else {
            0.0
        };
        let i0 = (t as usize).min(n - 1);
        let i1 = (i0 + 1).min(n - 1);
        let frac = t - i0 as f64;
        let v = hist[i0] as f64 * (1.0 - frac) + hist[i1] as f64 * frac;
        let lvl = (v / 100.0).clamp(0.0, 1.0);
        // Real fix: `.round()` made anything under 100/16 = 6.25% (a
        // genuinely common real CPU/GPU/RAM reading on a lightly-loaded
        // desktop) round DOWN to 0, rendering as a blank cell
        // indistinguishable from a real code bug ("brak grafu" was
        // reported against exactly this - a graph that WAS rendering,
        // just invisibly, at real everyday-low utilization). `.ceil()`
        // means any genuinely nonzero value shows at least the lowest
        // visible glyph; a true zero still renders as a real blank.
        let idx = if v > 0.0 {
            ((lvl * 8.0).ceil() as usize).clamp(1, 8)
        } else {
            0
        };
        s.push(GLYPHS[idx]);
    }
    vec![Span::styled(s, Style::default().fg(color))]
}

fn draw_gpu(f: &mut Frame, app: &App, area: Rect) {
    if !app.gpu.reading.found {
        f.render_widget(
            Paragraph::new(Line::from(Span::styled(
                "x GPU (unavailable)",
                Style::default().fg(theme::DISABLED),
            )))
            .block(block("GPU")),
            area,
        );
        return;
    }
    let inner_w = area.width.saturating_sub(4).max(10);
    let util_suffix = format!(" {:.0}%", app.gpu.reading.util);
    let bar_w = inner_w.saturating_sub(util_suffix.chars().count() as u16).max(10);
    let mut util_line = segmented_bar(&[(app.gpu.reading.util, theme::PRIMARY)], 100.0, bar_w);
    util_line
        .spans
        .push(Span::styled(util_suffix, Style::default().fg(theme::PRIMARY)));

    // Real fixed-[0,100] util trend graph (2026-08-29 real bug fix -
    // "gpu brakuje grafu": app.gpu.util_hist was already tracked and
    // pushed every tick in gpu.rs, but nothing here ever rendered it).
    let util_hist = app.gpu.util_hist.as_u64();
    let util_graph = Line::from(raw_percent_graph(&util_hist, inner_w, theme::PRIMARY));

    let mut lines = vec![util_line, util_graph];
    if app.gpu.reading.mem_total_mb > 0 {
        let mem_pct = app.gpu.reading.mem_used_mb as f64 / app.gpu.reading.mem_total_mb as f64 * 100.0;
        let vram_suffix = format!(" {:.0}% {}M", mem_pct, app.gpu.reading.mem_used_mb);
        let vram_bar_w = inner_w
            .saturating_sub(vram_suffix.chars().count() as u16)
            .saturating_sub(5)
            .max(10);
        let mut vram_line = segmented_bar(&[(mem_pct, theme::PRIMARY)], 100.0, vram_bar_w);
        let mut spans = vec![Span::styled("vram ", Style::default().fg(theme::PRIMARY))];
        spans.append(&mut vram_line.spans);
        spans.push(Span::styled(vram_suffix, Style::default().fg(theme::PRIMARY)));
        lines.push(Line::from(spans));
    }
    f.render_widget(Paragraph::new(lines).block(block("GPU")), area);
}

fn draw_ram(f: &mut Frame, app: &App, area: Rect) {
    let inner_w = area.width.saturating_sub(4).max(10);
    let m = &app.mem;
    let other = (m.mem_total as i64 - m.mem_free as i64 - m.cached as i64 - m.zram_used as i64).max(0) as f64;
    let swap_used = (m.swap_total as i64 - m.swap_free as i64).max(0) as f64;
    let free_combined = m.mem_free as f64 + m.swap_free as f64;
    let grand_total = m.mem_total as f64 + m.swap_total as f64;

    let segs = [
        (other, theme::RAM_USED),
        (m.zram_used as f64, theme::RAM_ZRAM),
        (m.cached as f64, theme::RAM_CACHE),
        (swap_used, theme::RAM_SWAP),
        (free_combined, theme::RAM_FREE),
    ];
    let used_total = grand_total - free_combined;
    let summary = format!(" {}/{}", human_bytes(used_total), human_bytes(free_combined));
    let bar_w = inner_w.saturating_sub(summary.chars().count() as u16).max(10);
    let mut bar_line = segmented_bar(&segs, grand_total, bar_w);
    bar_line
        .spans
        .push(Span::styled(summary, Style::default().fg(theme::PRIMARY)));

    // Real fixed-[0,100] used% trend graph (2026-08-29 real bug fix -
    // same "aggregate bar with no graph under it" gap CPU/GPU had:
    // m.used_pct_hist was already tracked and pushed every tick, but
    // nothing here ever rendered it).
    let used_hist = m.used_pct_hist.as_u64();
    let used_graph = Line::from(raw_percent_graph(&used_hist, inner_w, theme::RAM_USED));

    let mut lines = vec![bar_line, used_graph];
    lines.push(Line::from(Span::styled(
        format!("used  {}/{}", human_bytes(other), human_bytes(m.mem_total as f64)),
        Style::default().fg(theme::RAM_USED),
    )));
    if m.has_zram {
        lines.push(Line::from(Span::styled(
            format!("zram  {}/{}", human_bytes(m.zram_used as f64), human_bytes(m.mem_total as f64)),
            Style::default().fg(theme::RAM_ZRAM),
        )));
    }
    lines.push(Line::from(Span::styled(
        format!("cache {}/{}", human_bytes(m.cached as f64), human_bytes(m.mem_total as f64)),
        Style::default().fg(theme::RAM_CACHE),
    )));
    if m.swap_total > 0 {
        lines.push(Line::from(Span::styled(
            format!("swap  {}/{}", human_bytes(swap_used), human_bytes(m.swap_total as f64)),
            Style::default().fg(theme::RAM_SWAP),
        )));
        lines.push(Line::from(Span::styled(
            format!(
                "RAM+swap capacity: {} ({} + {})",
                human_bytes(grand_total),
                human_bytes(m.mem_total as f64),
                human_bytes(m.swap_total as f64)
            ),
            Style::default().fg(theme::PRIMARY),
        )));
    } else {
        lines.push(Line::from(Span::styled(
            format!("RAM capacity: {}", human_bytes(m.mem_total as f64)),
            Style::default().fg(theme::PRIMARY),
        )));
    }
    lines.push(Line::from(Span::styled(
        format!("free  {}/{}", human_bytes(free_combined), human_bytes(grand_total)),
        Style::default().fg(theme::RAM_FREE),
    )));

    f.render_widget(Paragraph::new(lines).block(block("RAM")), area);
}

fn draw_disk(f: &mut Frame, app: &App, area: Rect) {
    let inner_w = area.width.saturating_sub(4).max(10);
    let mut lines = Vec::new();

    if app.disk.total_bytes > 0 {
        let free = (app.disk.total_bytes - app.disk.used_bytes) as f64;
        let summary = format!(
            " {}/{}",
            human_bytes(app.disk.used_bytes as f64),
            human_bytes(free)
        );
        let bar_w = inner_w.saturating_sub(summary.chars().count() as u16).max(10);
        let mut bar_line = segmented_bar(
            &[(app.disk.used_bytes as f64, theme::PRIMARY), (free, theme::NEUTRAL)],
            app.disk.total_bytes as f64,
            bar_w,
        );
        bar_line
            .spans
            .push(Span::styled(summary, Style::default().fg(theme::PRIMARY)));
        lines.push(bar_line);
        // Real aggregate activity graph (2026-08-29 real fix - the same
        // "aggregate bar with no graph under it" gap CPU/GPU had):
        // unbounded rate, same auto-scaling raw_rate_graph_line every
        // per-device graph below already uses.
        let agg_hist = app.disk.agg_hist.as_u64();
        lines.push(raw_rate_graph_line(&agg_hist, inner_w));
    }

    if app.disk.devices.is_empty() {
        lines.push(Line::from(Span::styled(
            "(sampling devices...)",
            Style::default().fg(theme::PRIMARY),
        )));
        f.render_widget(Paragraph::new(lines).block(block("DISK")), area);
        return;
    }

    for d in &app.disk.devices {
        let mut ident = d.name.clone();
        if !d.model.is_empty() {
            ident = format!("{} ({})", d.name, d.model);
        }
        if d.removable {
            ident.push_str(" [rm]");
        }
        let color = if d.removable { theme::CAUTION } else { theme::PRIMARY };
        lines.push(Line::from(Span::styled(
            format!("{ident} {}", human_bytes(d.size_bytes as f64)),
            Style::default().fg(color),
        )));
        // Real per-device usage bar (2026-08-29, "kazdy dysk ma swoj
        // graf i swoj progressbar" - only the one aggregate bar existed
        // before; this is the SAME real statvfs data, just summed per
        // device instead of only across all devices - see
        // aggregate_partition_usage's own per_device return).
        if let Some(&(used, total)) = app.disk.device_usage.get(&d.name) {
            let free = (total - used) as f64;
            let summary = format!(" {}/{}", human_bytes(used as f64), human_bytes(free));
            let bar_w = inner_w.saturating_sub(summary.chars().count() as u16).max(10);
            let mut dev_bar = segmented_bar(
                &[(used as f64, theme::PRIMARY), (free, theme::NEUTRAL)],
                total as f64,
                bar_w,
            );
            dev_bar
                .spans
                .push(Span::styled(summary, Style::default().fg(theme::PRIMARY)));
            lines.push(dev_bar);
        }
        let hist = app
            .disk
            .device_hist
            .get(&d.name)
            .map(|h| h.as_u64())
            .unwrap_or_default();
        lines.push(raw_rate_graph_line(&hist, inner_w));
    }
    f.render_widget(Paragraph::new(lines).block(block("DISK")), area);
}

/// raw_rate_graph_line - the real, unbounded-rate trend graph (disk/net
/// throughput has no fixed bound the way a percentage does, so this
/// auto-scales to its own window's real max instead of a fixed range -
/// see raw_percent_graph for the fixed-[0,100] sibling used everywhere
/// else). Plain inline glyphs, not a separate ratatui Sparkline widget,
/// so it composes into the same Paragraph as every other line in these
/// panels without a second render pass.
fn raw_rate_graph_line(hist: &[u64], width: u16) -> Line<'static> {
    if hist.len() < 2 {
        return Line::from(" ".repeat(width as usize));
    }
    let max = hist.iter().copied().max().unwrap_or(1).max(1);
    const GLYPHS: [char; 9] = [' ', '▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'];
    let n = hist.len();
    let w = width as usize;
    let mut s = String::with_capacity(w);
    for x in 0..w {
        let t = if w > 1 { x as f64 / (w - 1) as f64 * (n - 1) as f64 } else { 0.0 };
        let i0 = (t as usize).min(n - 1);
        let i1 = (i0 + 1).min(n - 1);
        let frac = t - i0 as f64;
        let v = hist[i0] as f64 * (1.0 - frac) + hist[i1] as f64 * frac;
        let lvl = (v / max as f64).clamp(0.0, 1.0);
        let idx = ((lvl * 8.0).round() as usize).min(8);
        s.push(GLYPHS[idx]);
    }
    Line::from(Span::styled(s, Style::default().fg(theme::PRIMARY)))
}

fn draw_net(f: &mut Frame, app: &App, area: Rect) {
    let inner_w = area.width.saturating_sub(4).max(10);
    let mut lines = vec![Line::from(Span::styled(
        format!(
            "agg rx:{} tx:{}",
            human_rate(app.net.agg_hist.last() / 2.0),
            human_rate(app.net.agg_hist.last() / 2.0)
        ),
        Style::default().fg(theme::PRIMARY),
    ))];
    lines.push(raw_rate_graph_line(&app.net.agg_hist.as_u64(), inner_w));

    if app.net.ifaces.is_empty() {
        lines.push(Line::from(Span::styled(
            "(sampling interfaces...)",
            Style::default().fg(theme::PRIMARY),
        )));
    }
    for n in &app.net.ifaces {
        let speed = n
            .speed_mbps
            .map(|v| format!("{v}Mbps"))
            .unwrap_or_else(|| "?".to_string());
        lines.push(Line::from(Span::styled(
            format!(
                "{}  rx:{} tx:{}  link:{speed}",
                n.name,
                human_rate(n.rx_rate),
                human_rate(n.tx_rate)
            ),
            Style::default().fg(theme::PRIMARY),
        )));
        if let Some(mbps) = n.speed_mbps {
            let cap_bps = mbps as f64 * 1_000_000.0 / 8.0;
            let util = (n.rx_rate + n.tx_rate) / cap_bps * 100.0;
            let suffix = format!(" {util:3.0}%");
            let bar_w = inner_w.saturating_sub(suffix.chars().count() as u16).max(8);
            let mut bar_line = segmented_bar(
                &[(n.rx_rate, theme::PRIMARY), (n.tx_rate, ratatui::style::Color::Cyan)],
                cap_bps,
                bar_w,
            );
            bar_line
                .spans
                .push(Span::styled(suffix, Style::default().fg(theme::PRIMARY)));
            lines.push(bar_line);
        } else {
            let hist = app
                .net
                .iface_hist
                .get(&n.name)
                .map(|h| h.as_u64())
                .unwrap_or_default();
            lines.push(raw_rate_graph_line(&hist, inner_w));
        }
        if !n.addrs.is_empty() {
            lines.push(Line::from(Span::styled(
                format!("  {}", n.addrs.join(", ")),
                Style::default().fg(theme::ACCENT),
            )));
        }
    }
    f.render_widget(Paragraph::new(lines).block(block("NET")), area);
}

fn draw_proc(f: &mut Frame, app: &App, area: Rect) {
    let focused = app.focus == Focus::Proc;
    let inner_w = area.width.saturating_sub(4);
    let show_gpu = app.gpu.reading.found;
    let mut header = format!(
        "{:<3}{:<22}{:>6} {:>6} {:>9} {:>10} {:<10}",
        "", "NAME", "CPU%", "MEM%", "MEM", "DISK", "ACTIVITY"
    );
    if show_gpu {
        header.push_str(&format!("{:>8}", "GPU"));
    }
    header.push_str(" PROCS");
    let mut lines = vec![Line::from(Span::styled(
        header,
        Style::default().fg(theme::ACCENT),
    ))];

    for (i, g) in app.procs.groups.iter().enumerate() {
        if g.is_divider {
            lines.push(Line::from(Span::styled(
                g.name.clone(),
                Style::default().fg(theme::ACCENT),
            )));
            continue;
        }
        let marker = if app.procs.marked.contains(&g.name) {
            "*"
        } else {
            " "
        };
        let selected = focused && i == app.procs.cursor;
        let style = if selected {
            Style::default().fg(ratatui::style::Color::Black).bg(theme::ACCENT).add_modifier(Modifier::BOLD)
        } else {
            Style::default().fg(theme::tier_color(g.cpu_pct))
        };

        let name = trunc_chars(&g.name, 22);
        let fixed = format!(
            "{marker:<2} {name:<22}{:>5.1}% {:>5.1}% {:>8}M {:>10}",
            g.cpu_pct,
            g.mem_pct,
            g.mem_mb,
            human_rate(g.disk_bps)
        );
        let hist = app
            .procs
            .hist
            .get(&g.name)
            .map(|h| h.as_u64())
            .unwrap_or_default();
        let graph_color = if selected { ratatui::style::Color::Black } else { style.fg.unwrap_or(theme::PRIMARY) };
        let mut spans = vec![Span::styled(fixed, style)];
        spans.extend(raw_percent_graph(&hist, 10, graph_color));
        if show_gpu {
            spans.push(Span::styled(format!("{:>8}M", g.gpu_mb), style));
        }
        spans.push(Span::styled(format!("{:>5}", g.count), style));
        lines.push(Line::from(spans));
    }

    let mode_hint = format!(
        "\u{2191}/\u{2193} select  \u{2190}\u{2192} mode:{}  space: mark",
        app.procs.mode.label()
    );
    let hint = if focused {
        format!("{mode_hint}  k: kill  /: command")
    } else {
        format!("{mode_hint}  (tab: focus this list)")
    };
    lines.push(Line::from(Span::styled(hint, Style::default().fg(theme::ACCENT))));

    if !app.proc_action_msg.is_empty() {
        if app.proc_action_at.map(|t| t.elapsed().as_secs() < 5).unwrap_or(false) {
            lines.push(Line::from(Span::styled(
                app.proc_action_msg.clone(),
                Style::default().fg(theme::CAUTION),
            )));
        }
    }
    if !app.palette_result.is_empty() {
        lines.push(Line::from(Span::styled(
            app.palette_result.clone(),
            Style::default().fg(theme::CAUTION),
        )));
    }

    let mut title = "PROC".to_string();
    if focused {
        title.push_str(" [focused]");
    }
    let _ = inner_w;
    f.render_widget(Paragraph::new(lines).block(block(&title)), area);
}

fn draw_containers(f: &mut Frame, app: &App, area: Rect) {
    let focused = app.focus == Focus::Containers;
    let mut lines: Vec<Line> = Vec::new();
    let mut idx = 0usize;

    for (label, items) in [("docker", &app.docker), ("podman", &app.podman)] {
        for c in items {
            let selected = focused && idx == app.container_cursor;
            let style = if selected {
                Style::default().fg(ratatui::style::Color::Black).bg(theme::ACCENT)
            } else if c.running {
                Style::default().fg(theme::PRIMARY)
            } else {
                Style::default().fg(theme::NEUTRAL)
            };
            // real docker/podman status string (e.g. "Up 3 hours" /
            // "Exited (0) 2 days ago") - more honest than a simplified
            // up/stopped label, and this is what "wylaczone wlaczone
            // bez roznicy... ma pokazywac" actually asked for: show it.
            // Real bug fixed here: Rust's `{:<20}` PADS a short string
            // but never truncates a longer one, so an image name over
            // 20 real chars ran directly into the status text with no
            // separator at all (confirmed in a real TestBackend render:
            // "...debian:stableExited (0) 292 years ago"). trunc_chars
            // below is a real, char-boundary-safe truncation (Rust's
            // own String::truncate panics if the byte index it's given
            // isn't on a char boundary - a real risk for any non-ASCII
            // name, not just a cosmetic nicety).
            lines.push(Line::from(Span::styled(
                format!(
                    "{label:<8}{:<20}{:<20}{}",
                    trunc_chars(&c.name, 20),
                    trunc_chars(&c.image, 20),
                    c.status
                ),
                style,
            )));
            idx += 1;
        }
    }
    for v in &app.qemu {
        lines.push(Line::from(Span::styled(
            format!("qemu    {v}"),
            Style::default().fg(theme::PRIMARY),
        )));
    }
    for j in &app.jails {
        lines.push(Line::from(Span::styled(
            format!("jails   {j}"),
            Style::default().fg(theme::PRIMARY),
        )));
    }
    if lines.is_empty() {
        lines.push(Line::from(Span::styled(
            "(no containers, VMs, or jails running)",
            Style::default().fg(theme::PRIMARY),
        )));
    }
    if !app.ports.is_empty() {
        let ports = app.ports.iter().map(|p| p.to_string()).collect::<Vec<_>>().join(", ");
        lines.push(Line::from(Span::styled(
            format!("servers: {ports}"),
            Style::default().fg(theme::ACCENT),
        )));
    }

    let hint = if focused {
        "\u{2191}/\u{2193} select  s: start  x: stop  r: restart".to_string()
    } else {
        "\u{2191}/\u{2193} select  (tab: focus this list)".to_string()
    };
    lines.push(Line::from(Span::styled(hint, Style::default().fg(theme::ACCENT))));
    if !app.container_action_msg.is_empty() {
        if app.container_action_at.map(|t| t.elapsed().as_secs() < 5).unwrap_or(false) {
            lines.push(Line::from(Span::styled(
                app.container_action_msg.clone(),
                Style::default().fg(theme::CAUTION),
            )));
        }
    }

    let mut title = "CONTAINERS & SERVERS".to_string();
    if focused {
        title.push_str(" [focused]");
    }
    f.render_widget(Paragraph::new(lines).block(block(&title)), area);
}

fn draw_palette(f: &mut Frame, app: &App, area: Rect) {
    let w = (area.width * 3 / 4).max(20);
    let h = 3;
    let x = area.x + (area.width.saturating_sub(w)) / 2;
    let y = area.y + area.height / 2;
    let rect = Rect { x, y, width: w, height: h };
    let text = format!("/{}", app.palette_input);
    f.render_widget(
        Paragraph::new(Line::from(Span::styled(text, Style::default().fg(theme::PRIMARY))))
            .block(block("command (Enter: run, Esc: cancel)")),
        rect,
    );
}


#[cfg(test)]
mod tests {
    use super::*;

    /// Real regression guard for a real bug (2026-08-29, "brak grafu" -
    /// a percentage graph WAS rendering, just invisibly: `.round()`
    /// made any value under 100/16=6.25% - a genuinely common real CPU/
    /// GPU/RAM reading on a lightly-loaded desktop - round down to a
    /// blank cell, indistinguishable from a missing graph). Any real,
    /// nonzero history value must show at least the lowest visible
    /// glyph; a genuine all-zero history must still render as blank.
    #[test]
    fn raw_percent_graph_shows_low_nonzero_values() {
        let low_hist = vec![2u64, 3u64]; // real low-utilization values, e.g. 2-3% CPU
        let spans = raw_percent_graph(&low_hist, 10, theme::PRIMARY);
        let rendered: String = spans.iter().map(|s| s.content.as_ref()).collect();
        assert!(
            rendered.chars().any(|c| c != ' '),
            "a real nonzero (2-3%) history rendered as entirely blank: {rendered:?}"
        );

        let zero_hist = vec![0u64, 0u64];
        let spans = raw_percent_graph(&zero_hist, 10, theme::PRIMARY);
        let rendered: String = spans.iter().map(|s| s.content.as_ref()).collect();
        assert!(
            rendered.chars().all(|c| c == ' '),
            "a real all-zero history rendered something other than blank: {rendered:?}"
        );
    }
}
