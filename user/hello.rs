#![no_std]
#![no_main]
use core::panic::PanicInfo;

#[inline(always)]
fn sys(n: u64, a0: u64, a1: u64, a2: u64) -> i64 {
    let r: i64;
    unsafe {
        core::arch::asm!("svc #0", in("x8") n, inlateout("x0") a0 => r,
            in("x1") a1, in("x2") a2, options(nostack));
    }
    r
}
fn write(s: &[u8]) { sys(1, 1, s.as_ptr() as u64, s.len() as u64); }
fn exit() -> ! { sys(4,0,0,0); loop {} }

// AAPCS64 entry: x0=argc, x1=argv, x2=envp.
#[no_mangle]
pub extern "C" fn _start(argc: u64, argv: *const *const u8) -> ! {
    write(b"hello from a disk-loaded ELF at EL0!\n");
    // Print argc and each argv string.
    let mut nb = [0u8; 24];
    let n = render(&mut nb, argc);
    write(b"argc="); write(&nb[..n]); write(b"\n");
    let mut i = 0u64;
    while i < argc {
        let p = unsafe { *argv.add(i as usize) };
        if p.is_null() { break; }
        // strlen
        let mut len = 0usize;
        unsafe { while *p.add(len) != 0 { len += 1; } }
        let s = unsafe { core::slice::from_raw_parts(p, len) };
        write(b"  arg: "); write(s); write(b"\n");
        i += 1;
    }
    exit();
}
fn render(buf: &mut [u8], mut v: u64) -> usize {
    if v == 0 { buf[0]=b'0'; return 1; }
    let mut t=[0u8;24]; let mut n=0;
    while v>0 { t[n]=b'0'+(v%10) as u8; v/=10; n+=1; }
    for j in 0..n { buf[j]=t[n-1-j]; }
    n
}
#[panic_handler]
fn p(_:&PanicInfo)->!{ loop{} }
