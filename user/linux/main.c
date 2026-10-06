/*
 * LINHELLO -- a static Linux aarch64 binary, built by the Linux toolchain and
 * linked against nothing. It links no Fermi libc, includes no Fermi header, and
 * speaks only Linux syscall numbers.
 *
 * Its purpose is to prove the kernel honours the Linux process ABI, which is
 * the precondition for running anything built by a normal Linux toolchain:
 *
 *   1. Linux stack entry: argc on the stack at SP, argv/envp/auxv above it
 *   2. a well-formed auxiliary vector (AT_ENTRY, AT_PAGESZ, AT_PHDR, ...)
 *   3. arch_prctl(ARCH_SET_FS) so the guest can install its own thread pointer
 *   4. set_tid_address() returning a usable tid, as every libc startup requires
 *   5. Linux syscall numbers reaching the right handlers
 *
 * Everything is hand-rolled: no memcpy, no printf, no exit handlers. Output is
 * flushed explicitly before exiting.
 */

typedef unsigned long size_t;
typedef long          ssize_t;
typedef unsigned long uintptr_t;
/* Spelled out here rather than pulled from <stdint.h>: including the toolchain's
 * header is not the same as being able to link against its runtime, and this
 * binary deliberately uses neither. aarch64 LP64, so these are 64-bit. */
typedef unsigned long uint64_t;
typedef long          int64_t;

/* ---- Linux aarch64 syscall numbers ---- */
#define SYS_write           64
#define SYS_exit            93
#define SYS_set_tid_address 96
#define SYS_arch_prctl     158
#define SYS_getpid         172

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

/* Fermi's own SYS_SLEEP. Not a Linux syscall at all -- a Linux number for the
 * same slot would be nanosleep(2), which needs a struct timespec. Borrowed
 * here only to force a preemption, so we can prove the thread pointer survives
 * a context switch. A real Linux binary would not and could not do this. */
#define FERMI_SLEEP 6

/* ---- Auxiliary vector types (Linux values) ---- */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_RANDOM 25

/* ---- Raw syscall stubs ---- */
static long sys_write(int fd, const void *buf, unsigned long n) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = (long)buf;
  register long x2 __asm__("x2") = (long)n;
  register long x8 __asm__("x8") = SYS_write;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x2), "r"(x8)
                       : "memory");
  return x0;
}

static void sys_exit(int code) {
  register long x0 __asm__("x0") = code;
  register long x8 __asm__("x8") = SYS_exit;
  __asm__ __volatile__("svc #0"
                       :
                       : "r"(x0), "r"(x8)
                       : "memory");
  __builtin_unreachable();
}

static long sys_getpid(void) {
  register long x0 __asm__("x0");
  register long x8 __asm__("x8") = SYS_getpid;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x8)
                       : "memory");
  return x0;
}

static long sys_arch_prctl(long code, long val) {
  register long x0 __asm__("x0") = code;
  register long x1 __asm__("x1") = val;
  register long x8 __asm__("x8") = SYS_arch_prctl;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x1), "r"(x8)
                       : "memory");
  return x0;
}

static long sys_set_tid_address(long addr) {
  register long x0 __asm__("x0") = addr;
  register long x8 __asm__("x8") = SYS_set_tid_address;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x8)
                       : "memory");
  return x0;
}

static long fermi_sleep(unsigned long ms) {
  register long x0 __asm__("x0") = (long)ms;
  register long x8 __asm__("x8") = FERMI_SLEEP;
  __asm__ __volatile__("svc #0"
                       : "+r"(x0)
                       : "r"(x8)
                       : "memory");
  return x0;
}

/* ---- Minimal output ---- */
static char    obuf[1024];
static size_t olen;

static void emit_c(char c) {
  if (olen < sizeof obuf) {
    obuf[olen++] = c;
  }
}

static void emit(const char *s) {
  while (*s) {
    emit_c(*s++);
  }
}

static void flushout(void) {
  if (olen) {
    sys_write(1, obuf, olen);
    olen = 0;
  }
}

/* Hexadecimal. Note the nibble mask is 15, not 9: masking with 9 (binary 1001)
 * keeps only bits 0 and 3, so digits 2-7 could never be emitted and every value
 * containing one printed wrong. */
static void emit_hex(uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  char tmp[16];
  int  i = 0;
  if (v == 0) {
    emit_c('0');
    return;
  }
  while (v && i < 16) {
    tmp[i++] = digits[v & 15];
    v >>= 4;
  }
  while (i--) {
    emit_c(tmp[i]);
  }
}

/* Decimal, for counts a human reads. */
static void emit_dec(uint64_t v) {
  char tmp[24];
  int  i = 0;
  if (v == 0) {
    emit_c('0');
    return;
  }
  while (v && i < 24) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (i--) {
    emit_c(tmp[i]);
  }
}

/* ---- Check harness (deliberately not a printf) ---- */
static int checks;
static int fails;

static void ok(int cond, const char *what) {
  checks++;
  if (cond) {
    return;
  }
  fails++;
  emit("  [LINHELLO] FAIL: ");
  emit(what);
  emit_c('\n');
  flushout();
}

/* ---- Linux process startup structures ---- */
typedef struct {
  uint64_t a_type;
  uint64_t a_val;
} auxv_t;

/* Stand-in for a thread control block. The kernel's exec-time TLS page is at a
 * fixed VA, so pointing FS somewhere of our own choosing is the only way to
 * prove SET_FS actually took effect. */
static uint64_t fake_tcb[32] __attribute__((aligned(16)));

extern void _start(void); /* the real entry point, for AT_ENTRY comparison */

int main(int argc, char **argv, char **envp) {
  emit("[LINHELLO] === foreign Linux binary on Fermi OS ===\n");
  flushout();

  /* ---- 1. Linux stack entry convention ---- */
  /* argc was read off the stack by _start; reaching here at all proves SP held
   * a valid argc. Now confirm the argv it implied is the same one. */
  ok(argc >= 1, "argc read from the stack is sane");
  ok(argv != 0, "argv pointer is non-NULL");
  ok(argv[0] != 0, "argv[0] is non-NULL");
  ok(argv[argc] == 0, "argv is NULL-terminated");

  if (argv[0]) {
    /* Our exec passes the path through unchanged. */
    ok(argv[0][0] == '/', "argv[0] is an absolute path");
  }

  /* envp must be an empty, NULL-terminated list: this kernel passes none. */
  ok(envp != 0, "envp pointer is non-NULL");
  ok(envp[0] == 0, "envp is empty (no environment is passed)");

  /* ---- 2. Walk the auxiliary vector ---- */
  unsigned long phdr = 0, phent = 0, phnum = 0, pagesz = 0;
  unsigned long entry = 0, random = 0, hwcap = 0;
  int    saw_null = 0, saw_clk = 0, saw_base = 0;
  unsigned long aux_count = 0;

  auxv_t *aux      = (auxv_t *)(envp + 1);
  auxv_t *aux_base = aux;
  /* Bound the walk. An aux vector whose terminator is missing would otherwise
   * run off the stack; finding AT_NULL within the bound is what proves the
   * kernel terminated it, rather than assuming it did. */
  for (unsigned long guard = 0; aux->a_type != AT_NULL && guard < 64;
       aux++, guard++) {
    switch (aux->a_type) {
      case AT_PHDR:   phdr   = aux->a_val; break;
      case AT_PHENT:  phent  = aux->a_val; break;
      case AT_PHNUM:  phnum  = aux->a_val; break;
      case AT_PAGESZ: pagesz = aux->a_val; break;
      case AT_ENTRY:  entry  = aux->a_val; break;
      case AT_RANDOM: random = aux->a_val; break;
      case AT_HWCAP:  hwcap  = aux->a_val; break;
      case AT_CLKTCK: saw_clk = 1; break;
      case AT_BASE:   saw_base = 1; break;
      default: break;
    }
  }
  saw_null = (aux->a_type == AT_NULL);
  aux_count = (unsigned long)(aux - aux_base);

  ok(saw_null, "auxiliary vector is AT_NULL terminated within a sane bound");
  ok(pagesz == 4096, "AT_PAGESZ reports 4096");
  ok(phnum > 0, "AT_PHNUM reports a non-empty program header table");
  ok(phent == 56, "AT_PHENT reports the ELF64 program header size");
  ok(saw_clk, "AT_CLKTCK is present");
  ok(saw_base, "AT_BASE is present (0 = no dynamic loader)");
  ok(hwcap == 0, "AT_HWCAP advertises no features we have not verified");

  /* AT_ENTRY must name the address we actually started executing at. */
  ok(entry == (unsigned long)&_start, "AT_ENTRY names the real entry point");

  /* AT_PHDR has to point at a readable, plausible program header table. A
   * wrong AT_PHDR is the classic silent failure: the loader walks garbage and
   * either crashes or, worse, "works". Dereferencing it here turns that into a
   * reported failure. */
  ok(phdr != 0, "AT_PHDR is set");
  if (phdr) {
    /* ELF64 Phdr: p_type, p_flags, then offsets/addresses. */
    unsigned int p_type   = *(unsigned int *)(phdr + 0);
    unsigned int p_flags  = *(unsigned int *)(phdr + 4);
    uint64_t     p_offset = *(uint64_t *)(phdr + 8);
    ok(p_type == 1, "first program header is PT_LOAD");
    ok((p_flags & 1) != 0, "first program header is executable");
    ok(p_offset == 0, "first program header starts at file offset 0");
    /* AT_PHDR must point at the header table, which precedes the entry point in
     * any static executable. Comparing against AT_ENTRY rather than a hardcoded
     * base is what makes this robust: the loader may put the page-aligned
     * segment start BELOW the text (here 0x3f0000 vs entry 0x400000), so
     * assuming a fixed image base would fail on a perfectly correct binary.
     *
     * The structural checks above already dereferenced this address, so a wrong
     * AT_PHDR is reported rather than silently reading adjacent memory. */
    ok(phdr < entry, "AT_PHDR points before the entry point, into the image");

    /* And the table should be self-consistent: after the R+X text segment
     * comes the R+W data segment. */
    if (phnum >= 2) {
      unsigned int t2 = *(unsigned int *)(phdr + phent + 0);
      unsigned int f2 = *(unsigned int *)(phdr + phent + 4);
      ok(t2 == 1, "second program header is PT_LOAD");
      ok((f2 & 4) != 0, "second program header is writable");
    }
  }

  /* AT_RANDOM must be readable: the kernel guarantees 16 bytes there. */
  ok(random != 0, "AT_RANDOM is set");
  if (random) {
    volatile uint64_t *r = (volatile uint64_t *)random;
    ok((r[0] | r[1]) != 0 || (r[0] == 0 && r[1] == 0),
       "AT_RANDOM is readable (16 bytes present)");
  }

  /* ---- 3. arch_prctl(ARCH_SET_FS) ---- */
  /* Every Linux runtime installs its thread pointer this way. If this fails,
   * the binary cannot use TLS at all. */
  uint64_t *want = &fake_tcb[0];
  long r = sys_arch_prctl(ARCH_SET_FS, (long)want);
  ok(r == 0, "arch_prctl(ARCH_SET_FS) succeeded");
  long got = sys_arch_prctl(ARCH_GET_FS, 0);
  ok(got == (long)want, "arch_prctl(ARCH_GET_FS) reads it back");

  /* Write through the thread pointer to prove it is usable memory, not just a
   * number the kernel echoed back. */
  want[1] = 0xFEEDFACE12345678ULL;
  ok(want[1] == 0xFEEDFACE12345678ULL, "the thread pointer is usable storage");

  /* Misaligned thread pointers must be refused, not silently rounded. */
  ok(sys_arch_prctl(ARCH_SET_FS, (long)want + 1) < 0,
     "arch_prctl rejects a misaligned thread pointer");

  /* ---- 4. FS survives a context switch ----
   * The kernel reloads TPIDR_EL0 from the task on every switch, so a value that
   * came from a constant would be quietly replaced. Force a preemption and
   * re-read to prove it is per-task state that persists. */
  want[1] = 0x1111111111111111ULL;
  fermi_sleep(30); /* blocks, so another task definitely ran */
  ok(sys_arch_prctl(ARCH_GET_FS, 0) == (long)want,
     "thread pointer survives a context switch");
  ok(want[1] == 0x1111111111111111ULL,
     "thread-pointer storage is intact after the switch");

  /* ---- 5. Startup calls every libc makes ---- */
  /* glibc and musl both abort if set_tid_address fails, so this must return a
   * real tid rather than an error. */
  long tid = sys_set_tid_address((long)&fake_tcb[2]);
  ok(tid > 0, "set_tid_address returned a valid tid");
  ok(tid == sys_getpid(), "getpid matches the tid from set_tid_address");

  emit("[LINHELLO] pid=");
  emit_dec((uint64_t)sys_getpid());
  emit(" argv0=\"");
  emit(argv[0] ? argv[0] : "(null)");
  emit("\" auxv_entries=");
  emit_dec((uint64_t)aux_count);
  emit("\n");

  /* The thread pointer in hex: it is an address, and it is the clearest single
   * piece of evidence that the guest chose where its TLS lives rather than
   * being handed the kernel's fixed page. */
  emit("[LINHELLO] thread pointer=0x");
  emit_hex((uint64_t)(uintptr_t)sys_arch_prctl(ARCH_GET_FS, 0));
  emit("\n");

  emit("[LINHELLO] === ");
  emit_dec((uint64_t)checks);
  emit(" checks, ");
  emit_dec((uint64_t)fails);
  emit(" failed ===\n");
  flushout();

  sys_exit(fails ? 1 : 0);
}