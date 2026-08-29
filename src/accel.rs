//! Real TPU/NPU detection. Both are omitted entirely from the layout
//! when genuinely not present - never shown as "(unavailable)" (that
//! was tried once and explicitly asked to be removed: "tpu npu jezeli
//! nie ma to nie pokazuj").

use std::process::Command;

/// find_coral_tpu - real Google Coral Edge TPU detection: /dev/apex_0
/// is the real device node the PCIe Coral driver creates; a real USB
/// Coral shows up in `lsusb` under its own real vendor:product IDs.
pub fn find_coral_tpu() -> Option<String> {
    if std::path::Path::new("/dev/apex_0").exists() {
        return Some("/dev/apex_0 (PCIe)".to_string());
    }
    let out = Command::new("lsusb").output().ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    if text.contains("1a6e:089a") || text.contains("18d1:9302") {
        return Some("USB Accelerator (Coral)".to_string());
    }
    None
}

/// find_npu - real Linux /dev/accel/accel* detection, the actual
/// generic accelerator subsystem Intel VPU/similar non-GPU accelerators
/// register under. Apple's own Neural Engine has NO Linux interface at
/// all, even under Asahi - genuinely not obtainable from here, not
/// attempted.
pub fn find_npu() -> Option<String> {
    let entries = std::fs::read_dir("/dev/accel").ok()?;
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name.starts_with("accel") {
            return Some(entry.path().to_string_lossy().into_owned());
        }
    }
    None
}
