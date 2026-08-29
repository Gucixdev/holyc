//! Real GPU detection - nvidia-smi first, real amdgpu sysfs fallback.
//! No GPU found at all means the caller simply omits the box entirely
//! (same "real hardware or nothing" rule as TPU/NPU) - never a
//! fabricated reading.

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
            // Real amdgpu sysfs fallback.
            if let Ok(entries) = glob_drm_cards() {
                for busy_path in entries {
                    if let Ok(s) = std::fs::read_to_string(&busy_path) {
                        if let Ok(pct) = s.trim().parse::<f64>() {
                            self.reading = GpuReading {
                                found: true,
                                util: pct,
                                mem_used_mb: 0,
                                mem_total_mb: 0,
                            };
                            break;
                        }
                    }
                }
            }
        }
        if self.reading.found {
            self.util_hist.push(self.reading.util);
        }
    }
}

fn glob_drm_cards() -> std::io::Result<Vec<std::path::PathBuf>> {
    let mut out = Vec::new();
    for entry in std::fs::read_dir("/sys/class/drm")? {
        let entry = entry?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.starts_with("card") || name.contains('-') {
            continue;
        }
        let busy = entry.path().join("device/gpu_busy_percent");
        if busy.exists() {
            out.push(busy);
        }
    }
    Ok(out)
}

/// real_nvidia_proc_mem - real per-process VRAM via `nvidia-smi
/// --query-compute-apps`, one exec per tick (not per-pid). Empty map on
/// any non-NVIDIA host - callers just see 0 for everyone, honestly.
pub fn real_nvidia_proc_mem() -> HashMap<u32, u64> {
    let mut out = HashMap::new();
    if !nvidia_smi_available() {
        return out;
    }
    let Ok(res) = Command::new("nvidia-smi")
        .args([
            "--query-compute-apps=pid,used_memory",
            "--format=csv,noheader,nounits",
        ])
        .output()
    else {
        return out;
    };
    if !res.status.success() {
        return out;
    }
    let text = String::from_utf8_lossy(&res.stdout);
    for line in text.lines() {
        let parts: Vec<&str> = line.split(',').map(str::trim).collect();
        if parts.len() < 2 {
            continue;
        }
        if let (Ok(pid), Ok(mem)) = (parts[0].parse::<u32>(), parts[1].parse::<u64>()) {
            out.insert(pid, mem);
        }
    }
    out
}
