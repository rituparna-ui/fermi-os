//! PL031 real-time clock (QEMU virt @ 0x0901_0000).
//!
//! RTCDR (offset 0) holds the current time as seconds since the Unix epoch.

use crate::mmio;
use alloc::string::String;
use core::fmt::Write;

const PL031_BASE: usize = 0x0901_0000;
const RTC_DR: usize = PL031_BASE + 0x00;

/// Current wall-clock time in seconds since 1970-01-01 UTC.
pub fn epoch_secs() -> u64 {
    mmio::read32(RTC_DR) as u64
}

/// Convert a day count since the epoch to (year, month, day) — civil calendar
/// (Howard Hinnant's algorithm).
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u64; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32; // [1, 12]
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// Format the current RTC time as "YYYY-MM-DD HH:MM:SS UTC".
pub fn format_now() -> String {
    let secs = epoch_secs();
    let days = (secs / 86400) as i64;
    let tod = secs % 86400;
    let (y, mo, d) = civil_from_days(days);
    let h = tod / 3600;
    let mi = (tod % 3600) / 60;
    let s = tod % 60;
    let mut out = String::new();
    let _ = write!(
        out,
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC (epoch {})",
        y, mo, d, h, mi, s, secs
    );
    out
}
