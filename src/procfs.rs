//! Real /proc and /sys readers - CPU, memory, disk, network. Every
//! number here comes from an actual read of the real kernel interface,
//! same convention the original Go version used throughout: never
//! fabricate a reading, show an honest "?"/omission when something
//! genuinely isn't available.

use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::time::Instant;

use crate::history::History;

// ---------- CPU ----------

#[derive(Debug, Default, Clone)]
pub struct CoreStat {
    pub idx: usize,
    pub pct: f64,
    pub freq_mhz: Option<u32>,
    pub temp_c: Option<i32>,
}

#[derive(Default)]
pub struct CpuCollector {
    prev_total: Vec<[u64; 7]>, // index 0 = aggregate, 1.. = per-core
    prev_at: Option<Instant>,
    pub agg_pct: f64,
    pub user_pct: f64,
    pub system_pct: f64,
    pub iowait_pct: f64,
    pub load1: f64,
    pub cores: Vec<CoreStat>,
    pub agg_hist: History,
    pub core_hist: HashMap<usize, History>,
    /// core index -> hwmon tempN_input path, resolved once and cached -
    /// the real core->hwmon mapping isn't stable/guessable and has to be
    /// searched for (see resolve_core_temp_paths).
    temp_paths: Option<HashMap<usize, String>>,
}

/// parse_stat_line - real /proc/stat field layout: name user nice
/// system idle iowait irq softirq steal guest guest_nice. Returns the
/// first 7 real jiffie counters (through softirq) - the same real
/// fields the aggregate/per-mode split needs.
fn parse_stat_line(line: &str) -> Option<[u64; 7]> {
    let mut fields = line.split_whitespace();
    fields.next()?; // "cpu" / "cpuN"
    let mut out = [0u64; 7];
    for slot in out.iter_mut() {
        *slot = fields.next()?.parse().ok()?;
    }
    Some(out)
}

impl CpuCollector {
    pub fn refresh(&mut self) {
        let now = Instant::now();
        let dt = self.prev_at.map(|p| now.duration_since(p).as_secs_f64());

        let stat = fs::read_to_string("/proc/stat").unwrap_or_default();
        let mut cur: Vec<[u64; 7]> = Vec::new();
        for line in stat.lines() {
            if !line.starts_with("cpu") {
                break;
            }
            if let Some(f) = parse_stat_line(line) {
                cur.push(f);
            }
        }
        if cur.is_empty() {
            return;
        }

        if let (Some(dt), false) = (dt, self.prev_total.is_empty()) {
            if dt > 0.0 {
                // aggregate + per-mode (index 0)
                if let (Some(p), Some(c)) = (self.prev_total.first(), cur.first()) {
                    let tot_delta: i64 = (0..7).map(|i| c[i] as i64 - p[i] as i64).sum();
                    let idle_delta = c[3] as i64 - p[3] as i64;
                    if tot_delta > 0 {
                        self.agg_pct = (tot_delta - idle_delta) as f64 / tot_delta as f64 * 100.0;
                        let user_d = (c[0] as i64 - p[0] as i64) + (c[1] as i64 - p[1] as i64);
                        let sys_d = (c[2] as i64 - p[2] as i64)
                            + (c[5] as i64 - p[5] as i64)
                            + (c[6] as i64 - p[6] as i64);
                        let io_d = c[4] as i64 - p[4] as i64;
                        self.user_pct = user_d as f64 / tot_delta as f64 * 100.0;
                        self.system_pct = sys_d as f64 / tot_delta as f64 * 100.0;
                        self.iowait_pct = io_d as f64 / tot_delta as f64 * 100.0;
                        self.agg_hist.push(self.agg_pct);
                    }
                }
                // per-core
                let mut cores = Vec::with_capacity(cur.len().saturating_sub(1));
                for i in 1..cur.len() {
                    if i >= self.prev_total.len() {
                        continue;
                    }
                    let p = &self.prev_total[i];
                    let c = &cur[i];
                    let tot_delta: i64 = (0..7).map(|k| c[k] as i64 - p[k] as i64).sum();
                    let idle_delta = c[3] as i64 - p[3] as i64;
                    let pct = if tot_delta > 0 {
                        (tot_delta - idle_delta) as f64 / tot_delta as f64 * 100.0
                    } else {
                        0.0
                    };
                    let core_idx = i - 1;
                    let hist = self.core_hist.entry(core_idx).or_default();
                    hist.push(pct);
                    cores.push(CoreStat {
                        idx: core_idx,
                        pct,
                        freq_mhz: read_core_freq(core_idx),
                        temp_c: self.core_temp(core_idx),
                    });
                }
                self.cores = cores;
            }
        }
        self.prev_total = cur;
        self.prev_at = Some(now);

        if let Ok(s) = fs::read_to_string("/proc/loadavg") {
            if let Some(v) = s.split_whitespace().next() {
                self.load1 = v.parse().unwrap_or(0.0);
            }
        }
    }

    fn core_temp(&mut self, core_idx: usize) -> Option<i32> {
        if self.temp_paths.is_none() {
            self.temp_paths = Some(resolve_core_temp_paths());
        }
        let path = self.temp_paths.as_ref()?.get(&core_idx)?;
        let raw: i32 = fs::read_to_string(path).ok()?.trim().parse().ok()?;
        Some(raw / 1000)
    }
}

fn read_core_freq(core_idx: usize) -> Option<u32> {
    let path = format!(
        "/sys/devices/system/cpu/cpu{core_idx}/cpufreq/scaling_cur_freq"
    );
    let khz: u32 = fs::read_to_string(path).ok()?.trim().parse().ok()?;
    Some(khz / 1000)
}

/// resolve_core_temp_paths - real coretemp/k10temp hwmon search: scan
/// every /sys/class/hwmon/hwmon*, and for each real tempN_label file
/// containing "Core <N>", record that hwmon's tempN_input path for
/// core N. Not every core has its own real sensor (many CPUs only
/// expose one or a few package-level sensors) - cores with none simply
/// don't get an entry, and render an honest "--C" (see ui.rs).
fn resolve_core_temp_paths() -> HashMap<usize, String> {
    let mut map = HashMap::new();
    let Ok(entries) = fs::read_dir("/sys/class/hwmon") else {
        return map;
    };
    for entry in entries.flatten() {
        let hwmon_dir = entry.path();
        let Ok(labels) = fs::read_dir(&hwmon_dir) else {
            continue;
        };
        for label_entry in labels.flatten() {
            let name = label_entry.file_name();
            let name = name.to_string_lossy();
            let Some(n) = name
                .strip_prefix("temp")
                .and_then(|s| s.strip_suffix("_label"))
            else {
                continue;
            };
            let Ok(label) = fs::read_to_string(label_entry.path()) else {
                continue;
            };
            let label = label.trim();
            let Some(core_str) = label.strip_prefix("Core ") else {
                continue;
            };
            let Ok(core_idx) = core_str.parse::<usize>() else {
                continue;
            };
            let input_path = hwmon_dir.join(format!("temp{n}_input"));
            if input_path.exists() {
                map.insert(core_idx, input_path.to_string_lossy().into_owned());
            }
        }
    }
    map
}

// ---------- Memory ----------

#[derive(Default)]
pub struct MemInfo {
    pub mem_total: u64,
    pub mem_free: u64,
    pub cached: u64,
    pub swap_total: u64,
    pub swap_free: u64,
    pub zram_used: u64,
    pub has_zram: bool,
    pub used_pct_hist: History,
}

fn read_meminfo() -> HashMap<String, u64> {
    let mut out = HashMap::new();
    let Ok(s) = fs::read_to_string("/proc/meminfo") else {
        return out;
    };
    for line in s.lines() {
        if let Some((k, v)) = line.split_once(':') {
            let v = v.trim().trim_end_matches("kB").trim();
            if let Ok(n) = v.parse::<u64>() {
                out.insert(k.to_string(), n * 1024); // real bytes, not kB
            }
        }
    }
    out
}

/// read_zram_used - real /sys/block/zram*/mm_stat field 2 (0-indexed):
/// mem_used_total, the REAL physical bytes spent on the compressed
/// store - a different real quantity from swap-used (the logical bytes
/// swapped out), even when zram IS the swap backend.
fn read_zram_used() -> (u64, bool) {
    let Ok(entries) = fs::read_dir("/sys/block") else {
        return (0, false);
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.starts_with("zram") {
            continue;
        }
        let path = entry.path().join("mm_stat");
        if let Ok(s) = fs::read_to_string(&path) {
            let fields: Vec<&str> = s.split_whitespace().collect();
            if let Some(v) = fields.get(2).and_then(|v| v.parse::<u64>().ok()) {
                return (v, true);
            }
        }
    }
    (0, false)
}

impl MemInfo {
    pub fn refresh(&mut self) {
        let mi = read_meminfo();
        self.mem_total = mi.get("MemTotal").copied().unwrap_or(0);
        self.mem_free = mi.get("MemFree").copied().unwrap_or(0);
        self.cached =
            mi.get("Cached").copied().unwrap_or(0) + mi.get("Buffers").copied().unwrap_or(0);
        self.swap_total = mi.get("SwapTotal").copied().unwrap_or(0);
        self.swap_free = mi.get("SwapFree").copied().unwrap_or(0);
        let (zram, has_zram) = read_zram_used();
        self.zram_used = zram;
        self.has_zram = has_zram;

        if self.mem_total > 0 {
            let other = (self.mem_total as i64
                - self.mem_free as i64
                - self.cached as i64
                - self.zram_used as i64)
                .max(0) as f64;
            self.used_pct_hist.push(other / self.mem_total as f64 * 100.0);
        }
    }
}

// ---------- Disk ----------

#[derive(Debug, Clone)]
pub struct DiskDevice {
    pub name: String,
    pub model: String,
    pub size_bytes: u64,
    pub removable: bool,
    // read_rate/write_rate - real, computed every tick, but the UI
    // deliberately shows only the combined activity GRAPH now (the
    // simplified DISK design dropped the old bare "r:X w:Y" text) - kept
    // as real per-device data for whatever reads it next.
    #[allow(dead_code)]
    pub read_rate: f64,
    #[allow(dead_code)]
    pub write_rate: f64,
}

#[derive(Default)]
pub struct DiskCollector {
    prev_stats: HashMap<String, (u64, u64)>, // dev -> (rd_sectors, wr_sectors)
    prev_at: Option<Instant>,
    pub devices: Vec<DiskDevice>,
    pub device_hist: HashMap<String, History>,
    pub agg_hist: History,
    /// used/total real bytes, summed across every real mounted
    /// partition's own statvfs (crate::fsstat) - computed by the
    /// caller (main app tick) and stored here for the UI to read.
    pub used_bytes: u64,
    pub total_bytes: u64,
}

impl DiskCollector {
    pub fn refresh(&mut self) {
        let now = Instant::now();
        let dt = self.prev_at.map(|p| now.duration_since(p).as_secs_f64());

        let Ok(entries) = fs::read_dir("/sys/block") else {
            return;
        };
        let mut devices = Vec::new();
        let mut cur_stats = HashMap::new();
        let mut agg_rd = 0.0;
        let mut agg_wr = 0.0;

        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with("loop") || name.starts_with("ram") || name.starts_with("zram") {
                continue;
            }
            let base = entry.path();
            let Ok(size_str) = fs::read_to_string(base.join("size")) else {
                continue;
            };
            let size_sectors: u64 = size_str.trim().parse().unwrap_or(0);
            if size_sectors == 0 {
                continue; // real, unbacked device (e.g. disconnected nbd) - not a real disk
            }
            let size_bytes = size_sectors * 512;
            let model = fs::read_to_string(base.join("device/model"))
                .map(|s| s.trim().to_string())
                .unwrap_or_default();
            let removable = fs::read_to_string(base.join("removable"))
                .map(|s| s.trim() == "1")
                .unwrap_or(false);
            let Ok(stat) = fs::read_to_string(base.join("stat")) else {
                continue;
            };
            let fields: Vec<&str> = stat.split_whitespace().collect();
            if fields.len() < 7 {
                continue;
            }
            let rd_sectors: u64 = fields[2].parse().unwrap_or(0);
            let wr_sectors: u64 = fields[6].parse().unwrap_or(0);
            cur_stats.insert(name.clone(), (rd_sectors, wr_sectors));

            let mut read_rate = 0.0;
            let mut write_rate = 0.0;
            if let (Some(dt), Some((prd, pwr))) = (dt, self.prev_stats.get(&name)) {
                if dt > 0.0 {
                    read_rate = (rd_sectors.saturating_sub(*prd)) as f64 * 512.0 / dt;
                    write_rate = (wr_sectors.saturating_sub(*pwr)) as f64 * 512.0 / dt;
                }
            }
            agg_rd += read_rate;
            agg_wr += write_rate;

            let hist = self.device_hist.entry(name.clone()).or_default();
            hist.push(read_rate + write_rate);

            devices.push(DiskDevice {
                name,
                model,
                size_bytes,
                removable,
                read_rate,
                write_rate,
            });
        }
        devices.sort_by(|a, b| a.name.cmp(&b.name));
        self.agg_hist.push(agg_rd + agg_wr);
        self.devices = devices;
        self.prev_stats = cur_stats;
        self.prev_at = Some(now);
    }
}

/// real_mounts - /proc/mounts, keyed by bare device name (stripped of
/// "/dev/") -> real mountpoint.
pub fn read_mounts() -> HashMap<String, String> {
    let mut out = HashMap::new();
    let Ok(s) = fs::read_to_string("/proc/mounts") else {
        return out;
    };
    for line in s.lines() {
        let mut f = line.split_whitespace();
        let Some(dev) = f.next() else { continue };
        let Some(mp) = f.next() else { continue };
        if let Some(bare) = dev.strip_prefix("/dev/") {
            out.insert(bare.to_string(), mp.to_string());
        }
    }
    out
}

/// aggregate_partition_usage - real per-partition statvfs (see
/// fsstat::statvfs) summed across every real mounted partition under
/// every real whole-device this collector already found.
pub fn aggregate_partition_usage(devices: &[DiskDevice], mounts: &HashMap<String, String>) -> (u64, u64) {
    let mut used = 0u64;
    let mut total = 0u64;
    for dev in devices {
        let Ok(entries) = fs::read_dir(Path::new("/sys/block").join(&dev.name)) else {
            continue;
        };
        for entry in entries.flatten() {
            let part_name = entry.file_name().to_string_lossy().into_owned();
            if !entry.path().join("partition").exists() {
                continue;
            }
            let Some(mp) = mounts.get(&part_name) else {
                continue;
            };
            if let Some((u, t)) = crate::fsstat::statvfs_usage(mp) {
                used += u;
                total += t;
            }
        }
    }
    (used, total)
}

// ---------- Network ----------

#[derive(Debug, Clone)]
pub struct NetIface {
    pub name: String,
    pub rx_rate: f64,
    pub tx_rate: f64,
    pub speed_mbps: Option<u32>,
    pub addrs: Vec<String>,
}

#[derive(Default)]
pub struct NetCollector {
    prev_stats: HashMap<String, (u64, u64)>,
    prev_at: Option<Instant>,
    pub ifaces: Vec<NetIface>,
    pub iface_hist: HashMap<String, History>,
    pub agg_hist: History,
}

impl NetCollector {
    pub fn refresh(&mut self) {
        let now = Instant::now();
        let dt = self.prev_at.map(|p| now.duration_since(p).as_secs_f64());

        let Ok(s) = fs::read_to_string("/proc/net/dev") else {
            return;
        };
        let mut ifaces = Vec::new();
        let mut cur_stats = HashMap::new();
        let mut agg_rx = 0.0;
        let mut agg_tx = 0.0;
        let all_addrs = crate::netaddr::real_addrs();

        for line in s.lines().skip(2) {
            let Some((name, rest)) = line.split_once(':') else {
                continue;
            };
            let name = name.trim().to_string();
            if name == "lo" {
                continue;
            }
            let fields: Vec<&str> = rest.split_whitespace().collect();
            if fields.len() < 9 {
                continue;
            }
            let rx: u64 = fields[0].parse().unwrap_or(0);
            let tx: u64 = fields[8].parse().unwrap_or(0);
            cur_stats.insert(name.clone(), (rx, tx));

            let mut rx_rate = 0.0;
            let mut tx_rate = 0.0;
            if let (Some(dt), Some((prx, ptx))) = (dt, self.prev_stats.get(&name)) {
                if dt > 0.0 {
                    rx_rate = (rx.saturating_sub(*prx)) as f64 / dt;
                    tx_rate = (tx.saturating_sub(*ptx)) as f64 / dt;
                }
            }
            agg_rx += rx_rate;
            agg_tx += tx_rate;

            let speed_mbps = fs::read_to_string(format!("/sys/class/net/{name}/speed"))
                .ok()
                .and_then(|s| s.trim().parse::<i64>().ok())
                .filter(|v| *v > 0)
                .map(|v| v as u32);

            // Real noise cut: fe80::/10 link-local is auto-assigned to
            // every interface unconditionally and never means "this
            // machine's address" - filtered out entirely.
            let addrs: Vec<String> = all_addrs
                .get(&name)
                .cloned()
                .unwrap_or_default()
                .into_iter()
                .filter(|a| !a.starts_with("fe80:"))
                .collect();

            let hist = self.iface_hist.entry(name.clone()).or_default();
            hist.push(rx_rate + tx_rate);

            ifaces.push(NetIface {
                name,
                rx_rate,
                tx_rate,
                speed_mbps,
                addrs,
            });
        }
        ifaces.sort_by(|a, b| a.name.cmp(&b.name));
        self.agg_hist.push(agg_rx + agg_tx);
        self.ifaces = ifaces;
        self.prev_stats = cur_stats;
        self.prev_at = Some(now);
    }
}

// ---------- Listening ports (real "servers") ----------

/// real_listening_ports - /proc/net/tcp(+tcp6), real LISTEN-state
/// (0A) sockets - "which local services are actually listening now".
pub fn real_listening_ports() -> Vec<u16> {
    let mut seen = std::collections::BTreeSet::new();
    for path in ["/proc/net/tcp", "/proc/net/tcp6"] {
        let Ok(s) = fs::read_to_string(path) else {
            continue;
        };
        for line in s.lines().skip(1) {
            let fields: Vec<&str> = line.split_whitespace().collect();
            if fields.len() < 4 || fields[3] != "0A" {
                continue;
            }
            let Some((_, port_hex)) = fields[1].rsplit_once(':') else {
                continue;
            };
            if let Ok(port) = u16::from_str_radix(port_hex, 16) {
                seen.insert(port);
            }
        }
    }
    seen.into_iter().collect()
}
