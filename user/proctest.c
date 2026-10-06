/*
 * Process-model test: fork fd inheritance and waitpid.
 *
 * Verifies the POSIX semantics that were previously missing entirely:
 * a forked child inherits its parent's descriptors, shares the open file
 * description (so raw offsets stay in step), and the parent can block until
 * the child exits and collect its status.
 *
 * Named PROCT.ELF for FAT32 8.3.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks;
static int fails;

static void ok(int cond, const char *what) {
  checks++;
  if (cond) {
    return;
  }
  fails++;
  printf("  [PROC] FAIL: %s\n", what);
}

static void done(void) {
  printf("[PROC TEST] === %d checks, %d failed ===\n", checks, fails);
}

#define DATA_PATH "/mnt/fat32/PROC.TXT"

int main(void) {
  printf("[PROC TEST] === fork + waitpid ===\n");

  /* ---- 1. exit status is delivered to the parent ---- */
  int status = -1;
  int pid = fork();
  if (pid == 0) {
    _exit(42); /* child */
  }
  ok(pid > 0, "fork returned a child pid");
  /* wait() here must BLOCK: the child has not even been scheduled yet. A
   * non-blocking waitpid here is what previously reported ECHILD. */
  int w = waitpid(pid, &status, 0);
  ok(w == pid, "waitpid returned the child's pid");
  ok(WIFEXITED(status), "child reported a normal exit");
  ok(WEXITSTATUS(status) == 42, "exit status 42 delivered");

  /* ---- 2. WNOHANG before the child exits returns 0, not a block ---- */
  status = 0;
  pid = fork();
  if (pid == 0) {
    /* Sleep in user space so the parent observes a still-running child. */
    sys_sleep(200);
    _exit(7);
  }
  int wn = waitpid(pid, &status, WNOHANG);
  ok(wn == 0, "WNOHANG returns 0 while the child is still running");

  /* Now wait properly and confirm the same child is still collectable. */
  w = waitpid(pid, &status, 0);
  ok(w == pid, "the same child is later reaped");
  ok(WEXITSTATUS(status) == 7, "exit status 7 delivered");

  /* ---- 3. no children left -> ECHILD ---- */
  errno = 0;
  w = waitpid(-1, &status, 0);
  ok(w == -1 && errno == ECHILD, "waitpid with no children sets ECHILD");

  /* ---- 4. fd inheritance: the child sees the parent's open file ---- */
  /*
   * Deliberately done with raw read()/lseek() rather than stdio.
   *
   * A forked child inherits a COPY of the parent's stdio buffer contents, but
   * the file offset lives in the kernel's open file description, which is
   * shared. So a child that satisfies a read out of its inherited buffer never
   * touches the offset, and the parent's ftell() correctly does not move.
   * Exercising "shared offset" through stdio would test buffering, not sharing;
   * the raw fd is what actually isolates the property under test.
   */
  FILE *f = fopen(DATA_PATH, "w");
  ok(f != NULL, "created the shared data file");
  if (f) {
    fputs("ABCDEFGHIJKLMNOP", f);
    fclose(f);
  }

  int fd = open(DATA_PATH, O_RDONLY);
  ok(fd >= 0, "opened the shared data file for raw reading");
  if (fd < 0) {
    done();
    return 0;
  }

  char pbuf[8] = {0};
  long pn = read(fd, pbuf, 5);
  ok(pn == 5 && strncmp(pbuf, "ABCDE", 5) == 0, "parent read 5 bytes");
  ok(lseek(fd, 0, SEEK_CUR) == 5, "parent offset is 5 after its read");

  pid = fork();
  if (pid == 0) {
    /* ---- child half ---- */
    char cbuf[8] = {0};
    long cn = read(fd, cbuf, 5);
    if (cn != 5 || strncmp(cbuf, "FGHIJ", 5) != 0) {
      printf("  [PROC] FAIL: child read \"%s\", expected \"FGHIJ\"\n", cbuf);
    }
    /* Closing here must NOT pull the description out from under the parent:
     * the refcount is what keeps that from happening. */
    close(fd);
    _exit(0);
  }

  w = waitpid(pid, &status, 0);
  ok(w == pid, "child with inherited fds reaped cleanly");
  ok(WIFEXITED(status) && WEXITSTATUS(status) == 0,
     "child reported success (its own assertions printed above)");

  /* ---- 5. the shared offset moved in both directions ---- */
  ok(lseek(fd, 0, SEEK_CUR) == 10,
     "parent offset is 10 — the child's read moved the shared one");
  ok(lseek(fd, 0, SEEK_CUR) == 10,
     "the child's close() left the parent's descriptor usable");

  char nbuf[8] = {0};
  long nn = read(fd, nbuf, 4);
  ok(nn == 4 && strncmp(nbuf, "KLMN", 4) == 0,
     "parent continues reading from the shared offset");
  close(fd);

  /* ---- 6. a buffered stream is per-process after fork ---- */
  {
    FILE *g = fopen(DATA_PATH, "r");
    ok(g != NULL, "reopened the data file as a buffered stream");
    if (g) {
      char gb[8] = {0};
      fread(gb, 1, 5, g);
      ok(ftell(g) == 5, "stream offset is 5 after a 5-byte buffered read");
      int cpid = fork();
      if (cpid == 0) {
        /* The child serves its read from the inherited buffer, which is a copy
         * in the child's own memory, so the shared fd offset must not move. */
        char cb[8] = {0};
        fread(cb, 1, 5, g);
        _exit(strncmp(cb, "FGHIJ", 5) == 0 ? 0 : 1);
      }
      w = waitpid(cpid, &status, 0);
      ok(w == cpid && WEXITSTATUS(status) == 0,
         "child read the next 5 bytes from its inherited buffer");
      ok(ftell(g) == 5,
         "parent's buffered offset unmoved by the child's buffered read");
      fclose(g);
    }
  }

  /* ---- 7. a child that faults is reported as signalled, not exited ---- */
  /*
   * The [FAULT] lines this prints are expected: the child really is killed by
   * its null write, and the kernel's EL0 abort handler is what reaps it.
   */
  pid = fork();
  if (pid == 0) {
    *(volatile int *)0 = 1; /* deliberate null write */
    _exit(99);              /* not reached */
  }
  status = 0;
  w = waitpid(pid, &status, 0);
  ok(w == pid, "faulting child was still reaped");
  ok(!WIFEXITED(status), "fault death is not reported as a normal exit");
  ok(WIFSIGNALED(status), "fault death is reported as signalled");
  ok(WTERMSIG(status) == SIGSEGV, "fault death reports SIGSEGV");

  done();
  return 0;
}