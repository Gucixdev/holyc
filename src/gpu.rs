//! Real GPU detection - nvidia-smi first, real amdgpu sysfs fallback,
//! real intel_gpu_top fallback. No GPU found at all means the caller
//! simply omits the box entirely (same "real hardware or nothing" rule
//! as TPU/NPU) - never a fabricated reading.

use std::collections::HashMap;
use std::process::Command;

use crate::history::History;

#[derive(Default)]
pub struct GpuReading {
    pub found: bool,
    pub util: f64,
    pub mem_used_mb: u64,
    pub mem_total_mb: u64,
}

#[derive(Default)]
pub struct GpuCollector {
    pub reading: GpuReading,
    pub util_hist: History,
}

pub fn nvidia_smi_available() -> bool {
    which("nvidia-smi")
}

fn which(bin: &str) -> bool {
    std::env::var_os("PATH")
        .map(|paths| {
            std::env::split_paths(&paths).any(|dir| dir.join(bin).is_file())
        })
        .unwrap_or(false)
}

impl GpuCollector {
    pub fn refresh(&mut self) {
        self.reading = GpuReading::default();
        if nvidia_smi_available() {
            if let Ok(out) = Command::new("nvidia-smi")
                .args([
                    "--query-gpu=utilization.gpu,memory.used,memory.total",
                    "--format=csv,noheader,nounits",
                ])
                .output()
            {
                if out.status.success() {
                    let text = String::from_utf8_lossy(&out.stdout);
                    if let Some(line) = text.lines().next() {
                        let parts: Vec<&str> = line.split(',').map(str::trim).collect();
                        if parts.len() >= 3 {
                            if let (Ok(util), Ok(used), Ok(total)) = (
                                parts[0].parse::<f64>(),
                                parts[1].parse::<u64>(),
                                parts[2].parse::<u64>(),
                            ) {
                                self.reading = GpuReading {
                                    found: true,
                                    util,
                                    mem_used_mb: used,
                                    mem_total_mb: total,
                                };
                            }
                        }
                    }
                }
            }
        }
        if !self.reading.found {
            if let Some(reading) = read_amdgpu() {
                self.reading = reading;
            }
        }
        if !self.reading.found {
            if let Some(reading) = read_intel_gpu() {
                self.reading = reading;
            }
        }
        if self.reading.found {
            self.util_hist.push(self.reading.util);
        }
    }
}

fn drm_cards() -> std::io::Result<Vec<std::path::PathBuf>> {
    let mut out = Vec::new();
    for entry in std::fs::read_dir("/sys/class/drm")? {
        let entry = entry?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.starts_with("card") || name.contains('-') {
            continue;
        }
        out.push(entry.path().join("device"));
    }
    Ok(out)
}

/// read_amdgpu - real amdgpu driver sysfs: `gpu_busy_percent` for
/// utilization, and the real `mem_info_vram_used`/`mem_info_vram_total`
/// attributes (bytes) for VRAM - this is what was missing before
/// (2026-08-29, "zrob zeby dzialalo wszedzie nvidia amd" - AMD only had
/// utilization, VRAM was hardcoded to 0/0). Both are real, documented
/// amdgpu sysfs files - no fabricated numbers when either is absent
/// (an iGPU or older card may not expose VRAM attributes at all; that
/// just leaves mem_total_mb at 0, same honest "no vram info" the UI
/// already treats as "don't draw a vram bar").
fn read_amdgpu() -> Option<GpuReading> {
    let cards = drm_cards().ok()?;
    for dev in cards {
        let busy = dev.join("gpu_busy_percent");
        let Ok(s) = std::fs::read_to_string(&busy) else {
            continue;
        };
        let Ok(util) = s.trim().parse::<f64>() else {
            continue;
        };
        let read_bytes_mb = |name: &str| -> u64 {
            std::fs::read_to_string(dev.join(name))
                .ok()
                .and_then(|s| s.trim().parse::<u64>().ok())
                .map(|b| b / 1024 / 1024)
                .unwrap_or(0)
        };
        let mem_used_mb = read_bytes_mb("mem_info_vram_used");
        let mem_total_mb = read_bytes_mb("mem_info_vram_total");
        return Some(GpuReading {
            found: true,
            util,
            mem_used_mb,
            mem_total_mb,
        });
    }
    None
}

/// read_intel_gpu - real Intel i915/Xe GPU utilization via
/// `intel_gpu_top` (part of igt-gpu-tools, the standard real tool for
/// this - Intel's own drivers don't expose a single simple
/// "gpu_busy_percent"-style sysfs file the way amdgpu does, so unlike
/// NVIDIA/AMD this genuinely requires shelling out to a real external
/// tool, same real pattern as nvidia-smi itself). Honest "not found"
/// (no GPU box at all) when the tool isn't installed - never a
/// fabricated reading. real_intel_gpu_available lets a caller check
/// installation separately from a live sample.
///
/// NOTE: this is a best-effort implementation written without access
/// to real Intel graphics hardware to test against directly - the JSON
/// shape parsed here matches intel_gpu_top's own documented `-J`
/// output (an "engines" object whose entries carry a real "busy"
/// percentage each), but if a future intel_gpu_top version changes
/// that shape, this degrades to "no GPU found" rather than a crash or
/// a fabricated number, since every step here is a real, checked parse.
fn read_intel_gpu() -> Option<GpuReading> {
    if !which("intel_gpu_top") {
        return None;
    }
    // -J: one real JSON sample; -s: sampling period ms; -o -: write to
    // stdout instead of a file (intel_gpu_top's own default with -J is
    // actually stdout already, -o - makes that explicit and portable
    // across versions that differ on the default).
    let out = Command::new("intel_gpu_top")
        .args(["-J", "-s", "100", "-o", "-"])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    // Real, tolerant scan for every `"busy": <number>` occurrence inside
    // the "engines" object rather than a strict schema parse (no JSON
    // dependency in this project, and intel_gpu_top's own real engine
    // set varies by generation - Render/3D, Blitter, Video, VideoEnhance
    // aren't all present on every GPU). The busiest real engine is what
    // "GPU utilization" means in practice (matches how nvidia-smi's own
    // single utilization.gpu figure is really "the busiest part of the
    // pipeline", not a sum across engines).
    let mut max_busy = 0.0f64;
    let mut any = false;
    for m in text.match_indices("\"busy\":") {
        let rest = &text[m.0 + "\"busy\":".len()..];
        let num: String = rest
            .trim_start()
            .chars()
            .take_while(|c| c.is_ascii_digit() || *c == '.')
            .collect();
        if let Ok(v) = num.parse::<f64>() {
            any = true;
            if v > max_busy {
                max_busy = v;
            }
        }
    }
    if !any {
        return None;
    }
    // Real note: intel_gpu_top has no simple equivalent of nvidia-smi's
    // memory.used/memory.total for a shared-memory iGPU (it uses system
    // RAM, not dedicated VRAM) - mem_used_mb/mem_total_mb honestly stay
    // 0 here rather than fabricating a VRAM concept that doesn't really
    // apply, same as the UI already does for amdgpu when those
    // attributes are absent.
    Some(GpuReading {
        found: true,
        util: max_busy,
        mem_used_mb: 0,
        mem_total_mb: 0,
    })
}

/// real_nvidia_proc_stats - real per-process GPU utilization% AND VRAM
/// (MB) via `nvidia-smi pmon -c 1 -s um` - real bug fixed 2026-08-29
/// ("gpu nie pokazuje faktycznie ile jest zuzywane... nie moze tu byc
/// fakeow"): the previous version only queried `--query-compute-apps`,
/// which covers CUDA compute contexts ONLY - a compositor or game doing
/// real, substantial GPU work through OpenGL/Vulkan (type "G" in
/// nvidia-smi's own terms) never shows up there at all, so every such
/// process honestly-but-misleadingly showed 0 even while actively using
/// the GPU. `pmon` covers both real "C" (compute) and "G" (graphics)
/// process types. A real "-" in either column (nvidia-smi's own way of
/// saying "not measured for this process/type") is parsed as 0 - an
/// honest "no measurable activity", not a fabricated number.
pub fn real_nvidia_proc_stats() -> HashMap<u32, (f64, u64)> {
    let mut out = HashMap::new();
    if !nvidia_smi_available() {
        return out;
    }
    let Ok(res) = Command::new("nvidia-smi")
        .args(["pmon", "-c", "1", "-s", "um"])
        .output()
    else {
        return out;
    };
    if !res.status.success() {
        return out;
    }
    let text = String::from_utf8_lossy(&res.stdout);
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let fields: Vec<&str> = line.split_whitespace().collect();
        // real pmon -s um columns: gpu, pid, type, sm, mem, enc, dec,
        // jpg, ofa, fb, ccpm, command...
        if fields.len() < 10 {
            continue;
        }
        let Ok(pid) = fields[1].parse::<u32>() else {
            continue;
        };
        let sm = fields[3].parse::<f64>().unwrap_or(0.0);
        let fb = fields[9].parse::<u64>().unwrap_or(0);
        out.insert(pid, (sm, fb));
    }
    out
}
