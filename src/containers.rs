//! Real docker/podman/qemu/jail listing. docker/podman are the only two
//! runtimes with a real, simple, safe generic start/stop/restart verb -
//! qemu VMs and BSD jails stay listed but are never selectable/
//! manageable here. Lists ALL containers regardless of running state
//! (`ps -a`, not just running ones) with a real honest status per row.

use std::fs;
use std::process::Command;

#[derive(Debug, Clone)]
pub struct ContainerInfo {
    pub name: String,
    pub image: String,
    pub status: String,
    pub running: bool,
}

pub fn which(bin: &str) -> bool {
    std::env::var_os("PATH")
        .map(|paths| std::env::split_paths(&paths).any(|dir| dir.join(bin).is_file()))
        .unwrap_or(false)
}

fn list_ps_a(bin: &str) -> Vec<ContainerInfo> {
    if !which(bin) {
        return Vec::new();
    }
    let Ok(out) = Command::new(bin)
        .args(["ps", "-a", "--format", "{{.Names}}\t{{.Image}}\t{{.Status}}"])
        .output()
    else {
        return Vec::new();
    };
    if !out.status.success() {
        return Vec::new();
    }
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|line| {
            let mut parts = line.splitn(3, '\t');
            let name = parts.next().unwrap_or("").to_string();
            let image = parts.next().unwrap_or("").to_string();
            let status = parts.next().unwrap_or("").to_string();
            let running = status.to_ascii_lowercase().starts_with("up");
            ContainerInfo {
                name,
                image,
                status,
                running,
            }
        })
        .collect()
}

pub fn list_docker() -> Vec<ContainerInfo> {
    list_ps_a("docker")
}

pub fn list_podman() -> Vec<ContainerInfo> {
    list_ps_a("podman")
}

/// list_qemu_vms - real /proc scan for every live qemu-system-* process
/// - a real friendly name from the VM's own real "-name" cmdline
/// argument when present, else "pid N".
pub fn list_qemu_vms() -> Vec<String> {
    let mut out = Vec::new();
    let Ok(entries) = fs::read_dir("/proc") else {
        return out;
    };
    for entry in entries.flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        let comm = fs::read_to_string(entry.path().join("comm")).unwrap_or_default();
        if !comm.trim().starts_with("qemu-system") {
            continue;
        }
        let cmdline = fs::read_to_string(entry.path().join("cmdline")).unwrap_or_default();
        let args: Vec<&str> = cmdline.split('\0').collect();
        let mut label = format!("pid {pid}");
        for i in 0..args.len() {
            if args[i] == "-name" {
                if let Some(raw) = args.get(i + 1) {
                    let name = raw.split(',').next().unwrap_or("");
                    if !name.is_empty() {
                        label = format!("{name} (pid {pid})");
                    }
                }
                break;
            }
        }
        out.push(label);
    }
    out.sort();
    out
}

/// list_jails - real `jls` invocation when present; empty (not an
/// error) on a Linux host where it simply doesn't exist.
pub fn list_jails() -> Vec<String> {
    if !which("jls") {
        return Vec::new();
    }
    let Ok(out) = Command::new("jls").output() else {
        return Vec::new();
    };
    if !out.status.success() {
        return Vec::new();
    }
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .skip(1) // real jls header row
        .filter(|l| !l.trim().is_empty())
        .map(str::to_string)
        .collect()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Runtime {
    Docker,
    Podman,
}

impl Runtime {
    pub fn bin(&self) -> &'static str {
        match self {
            Runtime::Docker => "docker",
            Runtime::Podman => "podman",
        }
    }
}

/// manage - real `<runtime> start|stop|restart <name>`.
pub fn manage(runtime: Runtime, action: &str, name: &str) -> Result<(), String> {
    let out = Command::new(runtime.bin())
        .arg(action)
        .arg(name)
        .output()
        .map_err(|e| e.to_string())?;
    if out.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string())
    }
}
