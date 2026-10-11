//! Real interface IP addresses via the real `getifaddrs` syscall - std
//! doesn't expose this either, same real-FFI-only rationale as
//! fsstat.rs.

use std::collections::HashMap;
use std::ffi::CStr;
use std::net::{Ipv4Addr, Ipv6Addr};

/// real_addrs - real interface name -> real IP address strings (both
/// v4 and v6, in real CIDR form e.g. "192.168.1.5/24"). Link-local
/// v6 filtering happens in the caller (procfs.rs), not here - this
/// function reports everything real getifaddrs gives back, honestly.
pub fn real_addrs() -> HashMap<String, Vec<String>> {
    let mut out: HashMap<String, Vec<String>> = HashMap::new();
    let mut head: *mut libc::ifaddrs = std::ptr::null_mut();
    if unsafe { libc::getifaddrs(&mut head) } != 0 {
        return out;
    }
    let mut cur = head;
    while !cur.is_null() {
        let ifa = unsafe { &*cur };
        cur = ifa.ifa_next;
        if ifa.ifa_addr.is_null() {
            continue;
        }
        let name = unsafe { CStr::from_ptr(ifa.ifa_name) }
            .to_string_lossy()
            .into_owned();
        let family = unsafe { (*ifa.ifa_addr).sa_family } as i32;
        let entry = out.entry(name).or_default();
        unsafe {
            if family == libc::AF_INET {
                let sa = &*(ifa.ifa_addr as *const libc::sockaddr_in);
                let ip = Ipv4Addr::from(u32::from_be(sa.sin_addr.s_addr));
                let prefix = netmask_v4_prefix(ifa.ifa_netmask);
                entry.push(format!("{ip}/{prefix}"));
            } else if family == libc::AF_INET6 {
                let sa = &*(ifa.ifa_addr as *const libc::sockaddr_in6);
                let ip = Ipv6Addr::from(sa.sin6_addr.s6_addr);
                let prefix = netmask_v6_prefix(ifa.ifa_netmask);
                entry.push(format!("{ip}/{prefix}"));
            }
        }
    }
    unsafe { libc::freeifaddrs(head) };
    out
}

unsafe fn netmask_v4_prefix(mask_ptr: *mut libc::sockaddr) -> u32 {
    if mask_ptr.is_null() {
        return 32;
    }
    let sa = &*(mask_ptr as *const libc::sockaddr_in);
    u32::from_be(sa.sin_addr.s_addr).count_ones()
}

unsafe fn netmask_v6_prefix(mask_ptr: *mut libc::sockaddr) -> u32 {
    if mask_ptr.is_null() {
        return 128;
    }
    let sa = &*(mask_ptr as *const libc::sockaddr_in6);
    sa.sin6_addr.s6_addr.iter().map(|b| b.count_ones()).sum()
}
