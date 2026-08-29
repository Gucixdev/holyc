//! Real filesystem usage via the real `statvfs` syscall - there is no
//! /proc/sys-only way to get this, it genuinely requires the syscall.
//! std doesn't expose statvfs at all, so this is real, minimal libc FFI
//! (not a monitoring abstraction crate).

use std::ffi::CString;
use std::mem::MaybeUninit;

/// statvfs_usage - real (used_bytes, total_bytes) for the filesystem
/// mounted at `path`, or None if the real syscall fails (e.g. path
/// doesn't exist / isn't actually mounted right now).
pub fn statvfs_usage(path: &str) -> Option<(u64, u64)> {
    let c_path = CString::new(path).ok()?;
    let mut stat = MaybeUninit::<libc::statvfs>::uninit();
    let rc = unsafe { libc::statvfs(c_path.as_ptr(), stat.as_mut_ptr()) };
    if rc != 0 {
        return None;
    }
    let stat = unsafe { stat.assume_init() };
    let frsize = stat.f_frsize as u64;
    let total = stat.f_blocks as u64 * frsize;
    let free = stat.f_bfree as u64 * frsize;
    let used = total.saturating_sub(free);
    Some((used, total))
}
