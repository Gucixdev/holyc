//! Real application state, tick refresh, and key handling.

use std::io::Write;
use std::time::{Duration, Instant};

use crate::gpu::GpuCollector;
use crate::procfs::{aggregate_partition_usage, read_mounts, CpuCollector, DiskCollector, MemInfo, NetCollector};
use crate::proc_list::ProcCollector;

pub const TICK: Duration = Duration::from_secs(1);

pub struct App {
    pub cpu: CpuCollector,
    pub mem: MemInfo,
    pub disk: DiskCollector,
    pub net: NetCollector,
    pub gpu: GpuCollector,
    pub tpu: Option<String>,
    pub npu: Option<String>,

    pub procs: ProcCollector,

    pub proc_action_msg: String,
    pub proc_action_at: Option<Instant>,

    /// Real, user-adjustable refresh interval (2026-08-29, "zarzadzanie
    /// ms odwierzania" - +/- keys tune how often tick() actually runs).
    /// Clamped in handle_key to a real sane range, not a raw unbounded
    /// value.
    pub tick_interval: Duration,

    /// Real "r"-toggled resource recording (2026-08-29, "nagrywanie
    /// wszystkich zasobow i zapisywanie do jakiegos pliku") - a real
    /// CSV row appended every tick while active, one real file per
    /// recording session (a fresh timestamped file each time "r" turns
    /// it back on, not appended across sessions).
    pub recording: bool,
    pub record_path: Option<String>,
    record_file: Option<std::fs::File>,

    /// Real "/"-triggered command palette - typed name dispatches to
    /// commands/<name> against the current PROC selection/marks.
    pub palette_open: bool,
    pub palette_input: String,
    pub palette_result: String,

    pub should_quit: bool,
}

impl App {
    pub fn new() -> Self {
        Self {
            cpu: CpuCollector::default(),
            mem: MemInfo::default(),
            disk: DiskCollector::default(),
            net: NetCollector::default(),
            gpu: GpuCollector::default(),
            tpu: crate::accel::find_coral_tpu(),
            npu: crate::accel::find_npu(),
            procs: ProcCollector::default(),
            proc_action_msg: String::new(),
            proc_action_at: None,
            tick_interval: TICK,
            recording: false,
            record_path: None,
            record_file: None,
            palette_open: false,
            palette_input: String::new(),
            palette_result: String::new(),
            should_quit: false,
        }
    }

    pub fn tick(&mut self) {
        self.cpu.refresh();
        self.mem.refresh();
        self.disk.refresh();
        self.net.refresh();
        self.gpu.refresh();

        let mounts = read_mounts();
        let (used, total, per_device) = aggregate_partition_usage(&self.disk.devices, &mounts);
        self.disk.used_bytes = used;
        self.disk.total_bytes = total;
        self.disk.device_usage = per_device;
        // Real per-partition tree (2026-08-29, "pokazuj partycje
        // dyskow w drzewie jak wczesniej").
        for dev in &mut self.disk.devices {
            dev.partitions = crate::procfs::list_partitions(&dev.name, &mounts);
        }

        self.procs.refresh(self.mem.mem_total);

        self.record_tick();
    }

    /// record_tick - appends one real CSV row of the same real metrics
    /// every panel already shows, while recording is active. Never
    /// panics/crashes on a write failure (a full disk, a removed file)
    /// - just silently stops recording so the rest of the app keeps
    /// running.
    fn record_tick(&mut self) {
        let Some(file) = self.record_file.as_mut() else {
            return;
        };
        let ts = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let mem_pct = self.mem.used_pct_hist.last();
        let disk_rate = self.disk.agg_hist.last();
        let net_rate = self.net.agg_hist.last();
        let ok = writeln!(
            file,
            "{ts},{:.2},{mem_pct:.2},{:.2},{disk_rate:.2},{net_rate:.2}",
            self.cpu.agg_pct, self.gpu.reading.util,
        )
        .is_ok();
        if !ok {
            self.recording = false;
            self.record_file = None;
        }
    }

    /// toggle_record - real "r" key (2026-08-29, "r for toggle record
    /// nagrywanie wszystkich zasobow i zapisywanie do jakiegos pliku").
    /// A fresh, real, timestamped CSV file per session - turning
    /// recording back on later starts a NEW file rather than appending
    /// to the old one, so each recording is a clean, self-contained
    /// real log.
    fn toggle_record(&mut self) {
        if self.recording {
            self.recording = false;
            self.record_file = None;
            return;
        }
        let ts = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let path = format!("bsc-record-{ts}.csv");
        match std::fs::File::create(&path) {
            Ok(mut f) => {
                let _ = writeln!(f, "unix_ts,cpu_pct,mem_used_pct,gpu_util_pct,disk_rate_bytes_s,net_rate_bytes_s");
                self.record_file = Some(f);
                self.record_path = Some(path);
                self.recording = true;
            }
            Err(e) => {
                self.proc_action_msg = format!("record: could not create {path}: {e}");
                self.proc_action_at = Some(Instant::now());
            }
        }
    }

    pub fn handle_key(&mut self, key: crossterm::event::KeyCode) {
        use crossterm::event::KeyCode as K;

        if self.palette_open {
            match key {
                K::Esc => {
                    self.palette_open = false;
                    self.palette_input.clear();
                }
                K::Enter => {
                    self.run_palette_command();
                    self.palette_open = false;
                }
                K::Backspace => {
                    self.palette_input.pop();
                }
                K::Char(c) => self.palette_input.push(c),
                _ => {}
            }
            return;
        }

        match key {
            K::Char('q') => self.should_quit = true,
            K::Left => {
                let m = self.procs.mode.prev();
                self.procs.set_mode(m);
            }
            K::Right => {
                let m = self.procs.mode.next();
                self.procs.set_mode(m);
            }
            K::Up => self.procs.move_up(),
            K::Down => self.procs.move_down(),
            K::Char(' ') => self.procs.toggle_mark(),
            K::Char('k') => {
                let msg = self.procs.kill_selected();
                if !msg.is_empty() {
                    self.proc_action_msg = msg;
                    self.proc_action_at = Some(Instant::now());
                }
            }
            K::Char('/') => {
                self.palette_open = true;
                self.palette_input.clear();
            }
            K::Char('r') => self.toggle_record(),
            // Real refresh-interval control (2026-08-29, "zarzadzanie ms
            // odwierzania") - clamped to [100ms, 5s] so it can't be
            // tuned down into a real busy-loop or up into "never really
            // updates".
            K::Char('+') | K::Char('=') => {
                self.tick_interval =
                    (self.tick_interval + Duration::from_millis(100)).min(Duration::from_secs(5));
            }
            K::Char('-') => {
                self.tick_interval = self
                    .tick_interval
                    .saturating_sub(Duration::from_millis(100))
                    .max(Duration::from_millis(100));
            }
            _ => {}
        }
    }

    fn run_palette_command(&mut self) {
        let input = self.palette_input.trim().to_string();
        self.palette_input.clear();
        if input.is_empty() {
            return;
        }
        // real args: every pid in the marked set, or the cursor row.
        let mut args: Vec<String> = Vec::new();
        if self.procs.marked.is_empty() {
            if let Some(g) = self.procs.groups.get(self.procs.cursor) {
                if !g.is_divider {
                    args.extend(g.pids.iter().map(|p| p.to_string()));
                }
            }
        } else {
            for g in &self.procs.groups {
                if self.procs.marked.contains(&g.name) {
                    args.extend(g.pids.iter().map(|p| p.to_string()));
                }
            }
        }
        self.procs.marked.clear();
        self.palette_result = match crate::commands::run(&input, &args) {
            Ok(out) => {
                if out.is_empty() {
                    format!("{input}: ok")
                } else {
                    format!("{input}: {out}")
                }
            }
            Err(e) => format!("{input}: {e}"),
        };
    }
}
