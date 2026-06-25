//! Built-in device nodes under /dev.
//!
//! Port of `src/devices/devices.c`.

use super::vfs::{self, DevOps};

pub fn register() {
    vfs::register_chardev("console", DevOps::Console);
    vfs::register_chardev("null", DevOps::Null);
    vfs::register_chardev("zero", DevOps::Zero);
    vfs::register_chardev("rng", DevOps::Rng);
    vfs::register_chardev("vcons", DevOps::Vcons);
    vfs::register_blockdev("blk", DevOps::Blk);
}
