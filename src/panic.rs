//! Panic handling. A full register-dumping panic handler arrives with the
//! exception/panic work; for now we halt the CPU.

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {
        unsafe { core::arch::asm!("wfe") };
    }
}
