#include "timer.h"
#include "gic/gic.h"
#include "sched/sched.h"
#include "uart/uart.h"

// All four are read/written from IRQ context as well as task context, so
// mark them volatile to keep the compiler honest.
static volatile uint64_t timer_freq = 0;
static volatile uint64_t timer_interval = 0;
static volatile uint64_t tick_count = 0;
static volatile timer_callback_t tick_callback = 0;

void timer_init() {
  uart_println("[TIMER] Initializing hardware timer");
  // initial value set by fw
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(timer_freq));

  uart_printf("[TIMER] Frequency: %u Hz (%u MHz)\n", timer_freq,
              timer_freq / 1000000);

  gic_enable_irq(TIMER_PPI_INTID);

  uart_println("[TIMER] Initialized!");
}

void timer_start(uint64_t interval_ms) {
  if (timer_freq == 0) {
    uart_errorln("[TIMER] Not initialized! Call timer_init() first");
    return;
  }

  // milliseconds to timer ticks
  timer_interval = timer_freq * interval_ms / 1000;
  tick_count = 0;

  uart_printf("[TIMER] Starting with interval: %u ms (%u ticks)\n", interval_ms,
              timer_interval);

  // Use absolute deadlines (CVAL) instead of relative countdowns (TVAL)
  // so IRQ latency does not accumulate over time. CVAL is compared against
  // CNTPCT_EL0 by the timer; once they cross, the IRQ fires.
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
  __asm__ __volatile__(
      "msr cntp_cval_el0, %0" ::"r"(now + timer_interval));    // Deadline
  __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(1ULL));    // Enable

  uart_println("[TIMER] Started!");
}

void timer_stop() {
  __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(0ULL));
  uart_printf("[TIMER] Stopped after %u ticks\n", tick_count);
}

void timer_handle_irq() {
  tick_count++;

  // Re-arm by advancing the absolute deadline. This drifts at most one
  // interval per IRQ no matter how long handler latency takes; using
  // CNTP_TVAL_EL0 here would reset the countdown from "now" and accumulate.
  uint64_t cval;
  __asm__ __volatile__("mrs %0, cntp_cval_el0" : "=r"(cval));
  cval += timer_interval;
  __asm__ __volatile__("msr cntp_cval_el0, %0" ::"r"(cval));

  // Wake any tasks whose sleep deadline has passed
  sched_wake_sleepers();

  if (tick_callback) {
    tick_callback();
  } else {
    // Only log every 100 ticks (1 second at 10ms interval) to avoid spam
    if (tick_count % 100 == 0) {
      uart_printf("[TIMER] tick %u\n", tick_count);
    }
  }
}

void timer_set_callback(timer_callback_t cb) { tick_callback = cb; }

uint64_t timer_get_frequency() { return timer_freq; }

uint64_t timer_get_count() {
  uint64_t count;
  // Use the *physical* counter to match the physical timer (CNTPCT_EL0).
  // CNTVCT_EL0 (virtual) can diverge from CNTPCT_EL0 under virtualization.
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(count));
  return count;
}

uint64_t timer_get_ticks() { return tick_count; }

uint64_t timer_uptime_ms(void) { return tick_count * TIMER_INTERVAL_MS; }
uint64_t timer_uptime_seconds(void) {
  return (tick_count * TIMER_INTERVAL_MS) / 1000;
}
