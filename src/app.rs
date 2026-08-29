//! Real application state, tick refresh, and key handling.

use std::time::{Duration, Instant};

use crate::containers::{self, ContainerInfo, Runtime};
use crate::gpu::GpuCollector;
use crate::procfs::{aggregate_partition_usage, read_mounts, CpuCollector, DiskCollector, MemInfo, NetCollector, real_listening_ports};
use crate::proc_list::ProcCollector;

pub const TICK: Duration = Duration::from_secs(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Focus {
    Proc,
    Containers,
}

#[derive(Debug, Clone)]
pub struct ContainerEntry {
    pub runtime: Runtime,
    pub name: String,
}

pub struct App {
    pub cpu: CpuCollector,
    pub mem: MemInfo,
    pub disk: DiskCollector,
    pub net: NetCollector,
    pub gpu: GpuCollector,
    pub tpu: Option<String>,
    pub npu: Option<String>,

    pub procs: ProcCollector,

    pub docker: Vec<ContainerInfo>,
    pub podman: Vec<ContainerInfo>,
    pub qemu: Vec<String>,
    pub jails: Vec<String>,
    pub ports: Vec<u16>,
    pub container_entries: Vec<ContainerEntry>,
    pub container_cursor: usize,
    pub container_action_msg: String,
    pub container_action_at: Option<Instant>,
    pub proc_action_msg: String,
    pub proc_action_at: Option<Instant>,

    pub focus: Focus,

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
            docker: Vec::new(),
            podman: Vec::new(),
            qemu: Vec::new(),
            jails: Vec::new(),
            ports: Vec::new(),
            container_entries: Vec::new(),
            container_cursor: 0,
            container_action_msg: String::new(),
            container_action_at: None,
            proc_action_msg: String::new(),
            proc_action_at: None,
            focus: Focus::Proc,
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

        self.procs.refresh(self.mem.mem_total);

        self.docker = containers::list_docker();
        self.podman = containers::list_podman();
        self.qemu = containers::list_qemu_vms();
        self.jails = containers::list_jails();
        self.ports = real_listening_ports();

        let mut entries = Vec::new();
        for c in &self.docker {
            entries.push(ContainerEntry {
                runtime: Runtime::Docker,
                name: c.name.clone(),
            });
        }
        for c in &self.podman {
            entries.push(ContainerEntry {
                runtime: Runtime::Podman,
                name: c.name.clone(),
            });
        }
        self.container_entries = entries;
        if self.container_cursor >= self.container_entries.len() {
            self.container_cursor = self.container_entries.len().saturating_sub(1);
        }
    }

    pub fn manage_selected_container(&mut self, action: &str) {
        let Some(entry) = self.container_entries.get(self.container_cursor).cloned() else {
            return;
        };
        let msg = match containers::manage(entry.runtime, action, &entry.name) {
            Ok(()) => format!("{}: {action} {}", entry.runtime.bin(), entry.name),
            Err(e) => format!("{}: {action} {} failed: {e}", entry.runtime.bin(), entry.name),
        };
        self.container_action_msg = msg;
        self.container_action_at = Some(Instant::now());
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
            K::Tab => {
                self.focus = match self.focus {
                    Focus::Proc => Focus::Containers,
                    Focus::Containers => Focus::Proc,
                };
            }
            K::Left => {
                if self.focus == Focus::Proc {
                    let m = self.procs.mode.prev();
                    self.procs.set_mode(m);
                }
            }
            K::Right => {
                if self.focus == Focus::Proc {
                    let m = self.procs.mode.next();
                    self.procs.set_mode(m);
                }
            }
            K::Up => match self.focus {
                Focus::Proc => self.procs.move_up(),
                Focus::Containers => {
                    if self.container_cursor > 0 {
                        self.container_cursor -= 1;
                    }
                }
            },
            K::Down => match self.focus {
                Focus::Proc => self.procs.move_down(),
                Focus::Containers => {
                    if self.container_cursor + 1 < self.container_entries.len() {
                        self.container_cursor += 1;
                    }
                }
            },
            K::Char(' ') => {
                if self.focus == Focus::Proc {
                    self.procs.toggle_mark();
                }
            }
            K::Char('k') => {
                if self.focus == Focus::Proc {
                    let msg = self.procs.kill_selected();
                    if !msg.is_empty() {
                        self.proc_action_msg = msg;
                        self.proc_action_at = Some(Instant::now());
                    }
                }
            }
            K::Char('s') => {
                if self.focus == Focus::Containers {
                    self.manage_selected_container("start");
                }
            }
            K::Char('x') => {
                if self.focus == Focus::Containers {
                    self.manage_selected_container("stop");
                }
            }
            K::Char('r') => {
                if self.focus == Focus::Containers {
                    self.manage_selected_container("restart");
                }
            }
            K::Char('/') => {
                if self.focus == Focus::Proc {
                    self.palette_open = true;
                    self.palette_input.clear();
                }
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
