//! Real process listing/grouping/modes. Every number here is a real
//! /proc read - CPU% from real /proc/[pid]/stat jiffie deltas, MEM from
//! real VmRSS, DISK from a real /proc/[pid]/io byte-count delta (0 for
//! a process this user can't read - another user's - which is an
//! honest "unmeasurable", not a fabricated zero-usage claim), GPU from
//! real nvidia-smi --query-compute-apps (only ever shown as a column at
//! all when a real NVIDIA GPU was actually detected). NET is
//! deliberately not tracked per-process anywhere in this file - there
//! is no honest per-process byte-count source on Linux without eBPF or
//! netlink TCP_INFO plumbing.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::time::{Duration, Instant};

use crate::history::History;

/// PROC_RESORT_INTERVAL - real anti-jitter throttle: row ORDER only
/// re-sorts this often; live numbers still refresh every tick. Without
/// this, resorting every tick by noisy instantaneous cpu% made rows
/// visibly swap positions constantly - reported directly as eye strain.
const PROC_RESORT_INTERVAL: Duration = Duration::from_secs(4);
pub const DISPLAY_CAP: usize = 15;
const KIND_SECTION_CAP: usize = 6;

#[derive(Debug, Clone)]
pub struct ProcEntry {
    pub pid: u32,
    pub comm: String,
    pub cpu_pct: f64,
    pub mem_mb: u64,
    pub disk_bps: f64,
    pub gpu_mb: u64,
    pub is_kernel: bool,
}

#[derive(Debug, Clone, Default)]
pub struct ProcGroup {
    pub name: String,
    pub count: u32,
    pub cpu_pct: f64,
    pub mem_pct: f64,
    pub mem_mb: u64,
    pub disk_bps: f64,
    pub gpu_mb: u64,
    pub pids: Vec<u32>,
    pub is_divider: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ProcMode {
    #[default]
    App,
    Raw,
    Kind,
}

impl ProcMode {
    pub fn next(self) -> Self {
        match self {
            ProcMode::App => ProcMode::Raw,
            ProcMode::Raw => ProcMode::Kind,
            ProcMode::Kind => ProcMode::App,
        }
    }
    pub fn prev(self) -> Self {
        match self {
            ProcMode::App => ProcMode::Kind,
            ProcMode::Raw => ProcMode::App,
            ProcMode::Kind => ProcMode::Raw,
        }
    }
    pub fn label(&self) -> &'static str {
        match self {
            ProcMode::App => "app",
            ProcMode::Raw => "raw",
            ProcMode::Kind => "user/system",
        }
    }
}

fn group_by_name(procs: &[ProcEntry], mem_total_bytes: u64) -> Vec<ProcGroup> {
    let mut idx: HashMap<&str, usize> = HashMap::new();
    let mut groups: Vec<ProcGroup> = Vec::new();
    for p in procs {
        if let Some(&i) = idx.get(p.comm.as_str()) {
            let g = &mut groups[i];
            g.count += 1;
            g.cpu_pct += p.cpu_pct;
            g.mem_mb += p.mem_mb;
            g.disk_bps += p.disk_bps;
            g.gpu_mb += p.gpu_mb;
            g.pids.push(p.pid);
        } else {
            idx.insert(&p.comm, groups.len());
            groups.push(ProcGroup {
                name: p.comm.clone(),
                count: 1,
                cpu_pct: p.cpu_pct,
                mem_mb: p.mem_mb,
                disk_bps: p.disk_bps,
                gpu_mb: p.gpu_mb,
                pids: vec![p.pid],
                ..Default::default()
            });
        }
    }
    if mem_total_bytes > 0 {
        let mem_total_mb = mem_total_bytes as f64 / 1024.0 / 1024.0;
        for g in &mut groups {
            g.mem_pct = g.mem_mb as f64 / mem_total_mb * 100.0;
        }
    }
    groups.sort_by(|a, b| b.cpu_pct.partial_cmp(&a.cpu_pct).unwrap());
    groups
}

fn build_raw_groups(procs: &[ProcEntry], mem_total_bytes: u64) -> Vec<ProcGroup> {
    let mut groups: Vec<ProcGroup> = procs
        .iter()
        .map(|p| ProcGroup {
            name: format!("{} [{}]", p.comm, p.pid),
            count: 1,
            cpu_pct: p.cpu_pct,
            mem_mb: p.mem_mb,
            disk_bps: p.disk_bps,
            gpu_mb: p.gpu_mb,
            pids: vec![p.pid],
            ..Default::default()
        })
        .collect();
    if mem_total_bytes > 0 {
        let mem_total_mb = mem_total_bytes as f64 / 1024.0 / 1024.0;
        for g in &mut groups {
            g.mem_pct = g.mem_mb as f64 / mem_total_mb * 100.0;
        }
    }
    groups.sort_by(|a, b| b.cpu_pct.partial_cmp(&a.cpu_pct).unwrap());
    groups
}

fn build_kind_groups(procs: &[ProcEntry], mem_total_bytes: u64) -> Vec<ProcGroup> {
    let (user_procs, kernel_procs): (Vec<_>, Vec<_>) =
        procs.iter().cloned().partition(|p| !p.is_kernel);
    let mut user_groups = group_by_name(&user_procs, mem_total_bytes);
    let mut kernel_groups = group_by_name(&kernel_procs, mem_total_bytes);
    user_groups.truncate(KIND_SECTION_CAP);
    kernel_groups.truncate(KIND_SECTION_CAP);

    let mut out = Vec::with_capacity(user_groups.len() + kernel_groups.len() + 2);
    out.push(ProcGroup {
        name: "── user ──".to_string(),
        is_divider: true,
        ..Default::default()
    });
    out.extend(user_groups);
    out.push(ProcGroup {
        name: "── system/kernel ──".to_string(),
        is_divider: true,
        ..Default::default()
    });
    out.extend(kernel_groups);
    out
}

fn user_hz() -> f64 {
    let v = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if v > 0 {
        v as f64
    } else {
        100.0
    }
}

#[derive(Default)]
pub struct ProcCollector {
    prev_jiffies: HashMap<u32, u64>,
    prev_io: HashMap<u32, u64>,
    prev_at: Option<Instant>,
    last_sort_at: Option<Instant>,
    pub groups: Vec<ProcGroup>,
    pub hist: HashMap<String, History>,
    pub mode: ProcMode,
    pub cursor: usize,
    /// marked - real nnn-style multi-select, keyed by group name (works
    /// across all three modes since a raw-mode name already embeds its
    /// own pid). `k` kills every pid across every marked group when any
    /// are marked, else just the group under the cursor.
    pub marked: HashSet<String>,
}

fn read_proc_entries(mem_total_bytes: u64, prev_jiffies: &HashMap<u32, u64>,
                      prev_io: &HashMap<u32, u64>, dt: Option<f64>) -> (Vec<ProcEntry>, HashMap<u32, u64>, HashMap<u32, u64>) {
    let hz = user_hz();
    let gpu_mem = crate::gpu::real_nvidia_proc_mem();
    let mut out = Vec::new();
    let mut cur_jiffies = HashMap::new();
    let mut cur_io = HashMap::new();

    let Ok(entries) = fs::read_dir("/proc") else {
        return (out, cur_jiffies, cur_io);
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let Ok(pid) = name.to_string_lossy().parse::<u32>() else {
            continue;
        };
        let base = entry.path();
        let Ok(stat) = fs::read_to_string(base.join("stat")) else {
            continue;
        };
        let Some(lp) = stat.find('(') else { continue };
        let Some(rp) = stat.rfind(')') else { continue };
        if rp < lp {
            continue;
        }
        let comm = stat[lp + 1..rp].to_string();
        let rest: Vec<&str> = stat[rp + 2..].split_whitespace().collect();
        if rest.len() < 15 {
            continue;
        }
        let utime: u64 = rest[11].parse().unwrap_or(0);
        let stime: u64 = rest[12].parse().unwrap_or(0);
        let total = utime + stime;
        cur_jiffies.insert(pid, total);

        let cpu_pct = match (prev_jiffies.get(&pid), dt) {
            (Some(&prev), Some(dt)) if dt > 0.0 => {
                (total.saturating_sub(prev)) as f64 / hz / dt * 100.0
            }
            _ => 0.0,
        };

        let mut mem_kb = 0u64;
        if let Ok(status) = fs::read_to_string(base.join("status")) {
            for line in status.lines() {
                if let Some(v) = line.strip_prefix("VmRSS:") {
                    mem_kb = v.trim().trim_end_matches("kB").trim().parse().unwrap_or(0);
                    break;
                }
            }
        }

        let mut io_total = 0u64;
        if let Ok(io) = fs::read_to_string(base.join("io")) {
            let mut rd = 0u64;
            let mut wr = 0u64;
            for line in io.lines() {
                if let Some(v) = line.strip_prefix("read_bytes:") {
                    rd = v.trim().parse().unwrap_or(0);
                } else if let Some(v) = line.strip_prefix("write_bytes:") {
                    wr = v.trim().parse().unwrap_or(0);
                }
            }
            io_total = rd + wr;
        }
        cur_io.insert(pid, io_total);
        let disk_bps = match (prev_io.get(&pid), dt) {
            (Some(&prev), Some(dt)) if dt > 0.0 && io_total > prev => {
                (io_total - prev) as f64 / dt
            }
            _ => 0.0,
        };

        let is_kernel = fs::read_link(base.join("exe")).is_err();

        out.push(ProcEntry {
            pid,
            comm,
            cpu_pct,
            mem_mb: mem_kb / 1024,
            disk_bps,
            gpu_mb: gpu_mem.get(&pid).copied().unwrap_or(0),
            is_kernel,
        });
    }
    let _ = mem_total_bytes;
    (out, cur_jiffies, cur_io)
}

impl ProcCollector {
    pub fn refresh(&mut self, mem_total_bytes: u64) {
        let now = Instant::now();
        let dt = self.prev_at.map(|p| now.duration_since(p).as_secs_f64());
        let (procs, cur_jiffies, cur_io) =
            read_proc_entries(mem_total_bytes, &self.prev_jiffies, &self.prev_io, dt);

        let mut groups = match self.mode {
            ProcMode::App => group_by_name(&procs, mem_total_bytes),
            ProcMode::Raw => build_raw_groups(&procs, mem_total_bytes),
            ProcMode::Kind => build_kind_groups(&procs, mem_total_bytes),
        };

        // Real anti-jitter: keep the PREVIOUS tick's row order unless
        // the resort interval has elapsed; refresh each row's live
        // values in place either way. A name genuinely new since the
        // last resort is appended at the end rather than dropped.
        let should_resort = self
            .last_sort_at
            .map(|t| now.duration_since(t) >= PROC_RESORT_INTERVAL)
            .unwrap_or(true);
        if !should_resort {
            let by_name: HashMap<&str, &ProcGroup> =
                groups.iter().map(|g| (g.name.as_str(), g)).collect();
            let mut reordered = Vec::with_capacity(groups.len());
            let mut seen = HashSet::new();
            for prev_g in &self.groups {
                if let Some(&g) = by_name.get(prev_g.name.as_str()) {
                    reordered.push(g.clone());
                    seen.insert(prev_g.name.clone());
                }
            }
            for g in &groups {
                if !seen.contains(&g.name) {
                    reordered.push(g.clone());
                }
            }
            groups = reordered;
        } else {
            self.last_sort_at = Some(now);
        }

        if self.mode != ProcMode::Kind && groups.len() > DISPLAY_CAP {
            groups.truncate(DISPLAY_CAP);
        }

        // Real per-row activity history, rebuilt fresh from ONLY the
        // currently-displayed groups (not left to grow unbounded) - a
        // name still on screen keeps its real prior samples, one that
        // scrolled off or exited isn't carried forward.
        let mut new_hist = HashMap::with_capacity(groups.len());
        for g in &groups {
            if g.is_divider {
                continue;
            }
            let mut h = self.hist.get(&g.name).cloned().unwrap_or_default();
            h.push(g.cpu_pct);
            new_hist.insert(g.name.clone(), h);
        }
        self.hist = new_hist;

        self.groups = groups;
        if self.cursor >= self.groups.len() {
            self.cursor = self.groups.len().saturating_sub(1);
        }
        self.snap_cursor_off_divider();

        self.prev_jiffies = cur_jiffies;
        self.prev_io = cur_io;
        self.prev_at = Some(now);
    }

    fn snap_cursor_off_divider(&mut self) {
        if self.groups.is_empty() {
            return;
        }
        if self.groups[self.cursor].is_divider {
            if let Some(n) = self.next_selectable(self.cursor) {
                self.cursor = n;
            } else if let Some(p) = self.prev_selectable(self.cursor) {
                self.cursor = p;
            }
        }
    }

    pub fn next_selectable(&self, from: usize) -> Option<usize> {
        ((from + 1)..self.groups.len()).find(|&i| !self.groups[i].is_divider)
    }

    pub fn prev_selectable(&self, from: usize) -> Option<usize> {
        (0..from).rev().find(|&i| !self.groups[i].is_divider)
    }

    pub fn move_up(&mut self) {
        if let Some(p) = self.prev_selectable(self.cursor) {
            self.cursor = p;
        }
    }

    pub fn move_down(&mut self) {
        if let Some(n) = self.next_selectable(self.cursor) {
            self.cursor = n;
        }
    }

    pub fn set_mode(&mut self, mode: ProcMode) {
        self.mode = mode;
        self.cursor = 0;
        self.marked.clear();
    }

    /// toggle_mark - nnn-style multi-select on the row under the
    /// cursor (real, visible per-row marker in the UI).
    pub fn toggle_mark(&mut self) {
        let Some(g) = self.groups.get(self.cursor) else {
            return;
        };
        if g.is_divider {
            return;
        }
        let name = g.name.clone();
        if !self.marked.remove(&name) {
            self.marked.insert(name);
        }
    }

    /// kill_selected - real SIGKILL to every real pid in every marked
    /// group, or just the group under the cursor when nothing is
    /// marked. Returns a real, honest result summary for the UI.
    pub fn kill_selected(&mut self) -> String {
        let targets: Vec<ProcGroup> = if self.marked.is_empty() {
            self.groups
                .get(self.cursor)
                .filter(|g| !g.is_divider)
                .cloned()
                .into_iter()
                .collect()
        } else {
            self.groups
                .iter()
                .filter(|g| self.marked.contains(&g.name))
                .cloned()
                .collect()
        };
        if targets.is_empty() {
            return String::new();
        }
        let mut killed = 0;
        let mut total = 0;
        let mut names = Vec::new();
        for g in &targets {
            names.push(g.name.clone());
            for &pid in &g.pids {
                total += 1;
                if unsafe { libc::kill(pid as i32, libc::SIGKILL) } == 0 {
                    killed += 1;
                }
            }
        }
        self.marked.clear();
        format!("killed {killed}/{total} ({})", names.join(", "))
    }
}
