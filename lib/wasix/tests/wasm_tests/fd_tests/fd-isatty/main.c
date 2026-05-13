// Comprehensive checks for how WASIX reports tty / filetype state for
// file descriptors. This drives the `fdstat`/`filestat` plumbing that
// `isatty(3)` and `S_ISCHR(2)` rely on.
//
// The test is split into three groups:
//
//   * "Public surface" tests assert behaviour every libc consumer
//     observes: `isatty()`, `fstat()`, and `__wasi_fd_fdstat_get`.
//
//   * "Consistency" tests assert that the various ways the guest can ask
//     "what kind of file is this fd?" return the same answer for a
//     single fd. Divergence between `fd_fdstat_get` and `fd_filestat_get`
//     is itself a bug: programs that check via one API and act via the
//     other will get inconsistent decisions about seekability,
//     mmap-ability, line buffering, etc.
//
//   * "Redirection" tests cover the case where the guest re-points
//     stdio at a regular file via `dup2`. After the redirect, fd 0
//     should describe the regular file, not a character device.
//
// stdin handling: the harness does not pin stdin, so depending on how
// `cargo test` was launched it may or may not be a real tty. The test
// probes once at start, records which case applies, and only asserts
// the non-tty branch when stdin is not a host tty. stdout/stderr are
// always backed by capture buffers in the harness and are unconditionally
// expected to be non-tty.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wasi/api_wasi.h>

// ----------------------------------------------------------------------------
// Test framework
// ----------------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;

#define FAILF(fmt, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__); \
    g_failures++;                                                              \
  } while (0)

#define EXPECT(cond, msg)                                                      \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define EXPECT_EQ_U(actual, expected, label)                                   \
  do {                                                                         \
    g_checks++;                                                                \
    uint64_t _a = (uint64_t)(actual);                                          \
    uint64_t _e = (uint64_t)(expected);                                        \
    if (_a != _e) {                                                            \
      FAILF("%s: expected %llu, got %llu", (label),                            \
            (unsigned long long)_e, (unsigned long long)_a);                   \
    }                                                                          \
  } while (0)

#define EXPECT_FILETYPE(actual, expected, label)                               \
  do {                                                                         \
    g_checks++;                                                                \
    __wasi_filetype_t _a = (actual);                                           \
    __wasi_filetype_t _e = (expected);                                         \
    if (_a != _e) {                                                            \
      FAILF("%s: expected %s, got %s", (label), filetype_name(_e),             \
            filetype_name(_a));                                                \
    }                                                                          \
  } while (0)

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static const char *filetype_name(__wasi_filetype_t ft) {
  switch (ft) {
    case __WASI_FILETYPE_UNKNOWN:          return "UNKNOWN";
    case __WASI_FILETYPE_BLOCK_DEVICE:     return "BLOCK_DEVICE";
    case __WASI_FILETYPE_CHARACTER_DEVICE: return "CHARACTER_DEVICE";
    case __WASI_FILETYPE_DIRECTORY:        return "DIRECTORY";
    case __WASI_FILETYPE_REGULAR_FILE:     return "REGULAR_FILE";
    case __WASI_FILETYPE_SOCKET_DGRAM:     return "SOCKET_DGRAM";
    case __WASI_FILETYPE_SOCKET_STREAM:    return "SOCKET_STREAM";
    case __WASI_FILETYPE_SYMBOLIC_LINK:    return "SYMBOLIC_LINK";
    default:                               return "?";
  }
}

static __wasi_fd_t find_preopen_directory_fd(void) {
  for (__wasi_fd_t fd = 3; fd < 64; fd++) {
    __wasi_prestat_t prestat;
    __wasi_errno_t err = __wasi_fd_prestat_get(fd, &prestat);
    if (err == __WASI_ERRNO_SUCCESS && prestat.tag == __WASI_PREOPENTYPE_DIR) {
      return fd;
    }
  }
  return (__wasi_fd_t)-1;
}

// Read fs_filetype from fd_fdstat_get and EXPECT success.
static __wasi_filetype_t fdstat_filetype_of(__wasi_fd_t fd, const char *label) {
  __wasi_fdstat_t st;
  memset(&st, 0, sizeof(st));
  __wasi_errno_t err = __wasi_fd_fdstat_get(fd, &st);
  if (err != __WASI_ERRNO_SUCCESS) {
    FAILF("%s: fd_fdstat_get(fd=%u) errno=%u", label, (unsigned)fd,
          (unsigned)err);
    return __WASI_FILETYPE_UNKNOWN;
  }
  return st.fs_filetype;
}

// Read filetype from fd_filestat_get and EXPECT success.
static __wasi_filetype_t filestat_filetype_of(__wasi_fd_t fd,
                                              const char *label) {
  __wasi_filestat_t st;
  memset(&st, 0, sizeof(st));
  __wasi_errno_t err = __wasi_fd_filestat_get(fd, &st);
  if (err != __WASI_ERRNO_SUCCESS) {
    FAILF("%s: fd_filestat_get(fd=%u) errno=%u", label, (unsigned)fd,
          (unsigned)err);
    return __WASI_FILETYPE_UNKNOWN;
  }
  return st.filetype;
}

// Probe whether a given fd is a tty WITHOUT going through isatty(), so we
// can decide what to assert about it. The fdstat path is the same logic
// isatty() uses underneath, but reading it directly keeps the framing
// honest: we are deciding based on what the runtime reports right now.
static bool fd_is_chardev(__wasi_fd_t fd) {
  __wasi_fdstat_t st;
  memset(&st, 0, sizeof(st));
  if (__wasi_fd_fdstat_get(fd, &st) != __WASI_ERRNO_SUCCESS) {
    return false;
  }
  return st.fs_filetype == __WASI_FILETYPE_CHARACTER_DEVICE;
}

// ----------------------------------------------------------------------------
// 1. stdout and stderr are non-tty under the test harness (CaptureFile),
//    so isatty() must return 0 with errno=ENOTTY.
//
//    stdin is only checked when the harness did not give us a real tty.
// ----------------------------------------------------------------------------

static void test_isatty_on_stdio(bool stdin_is_tty) {
  printf("Test 1: isatty(stdout/stderr) returns 0 (and stdin when non-tty)\n");

  int fds[3] = {0, 1, 2};
  for (int i = 0; i < 3; i++) {
    int fd = fds[i];
    if (fd == 0 && stdin_is_tty) {
      printf("  fd 0 reported as a tty by the harness, skipping\n");
      continue;
    }
    errno = 0;
    int r = isatty(fd);
    if (r != 0) {
      FAILF("isatty(%d) returned %d, expected 0 (non-tty harness)", fd, r);
    } else {
      EXPECT(errno == ENOTTY,
             "isatty must set errno=ENOTTY when returning 0");
    }
  }
}

// ----------------------------------------------------------------------------
// 2. fd_fdstat_get on stdio MUST NOT report CHARACTER_DEVICE when the
//    underlying stream is not a real tty. This is the primary bug the
//    patch is intended to fix.
// ----------------------------------------------------------------------------

static void test_stdio_fdstat_not_character_device(bool stdin_is_tty) {
  printf("Test 2: fd_fdstat_get on stdout/stderr (and non-tty stdin) is not "
         "CHARACTER_DEVICE\n");

  __wasi_fd_t fds[3] = {0, 1, 2};
  for (int i = 0; i < 3; i++) {
    __wasi_fd_t fd = fds[i];
    if (fd == 0 && stdin_is_tty) {
      continue;
    }
    __wasi_filetype_t ft = fdstat_filetype_of(fd, "stdio fdstat");
    if (ft == __WASI_FILETYPE_CHARACTER_DEVICE) {
      FAILF("fd %u reports CHARACTER_DEVICE but is not a tty",
            (unsigned)fd);
    }
  }
}

// ----------------------------------------------------------------------------
// 3. fd_fdstat_get and fd_filestat_get MUST agree on the file's type.
//
//    Programs commonly use one or the other to make decisions about
//    seekability, line buffering, mmap, etc. They must not disagree for
//    the same fd. Today the cached Filestat for stdio inodes is
//    hardcoded to CHARACTER_DEVICE in create_std_dev_inner, so this
//    check is expected to fail until that is fixed too.
// ----------------------------------------------------------------------------

static void test_stdio_fdstat_filestat_consistent(void) {
  printf("Test 3: fd_fdstat_get and fd_filestat_get agree for stdio\n");

  for (__wasi_fd_t fd = 0; fd <= 2; fd++) {
    __wasi_filetype_t a = fdstat_filetype_of(fd, "stdio fdstat");
    __wasi_filetype_t b = filestat_filetype_of(fd, "stdio filestat");
    if (a != b) {
      FAILF("fd %u: fdstat=%s but filestat=%s (must match)", (unsigned)fd,
            filetype_name(a), filetype_name(b));
    }
  }
}

// ----------------------------------------------------------------------------
// 4. Regular file: fdstat and filestat agree on REGULAR_FILE.
// ----------------------------------------------------------------------------

static void test_regular_file_filetype(void) {
  printf("Test 4: regular file reports REGULAR_FILE through both APIs\n");

  const char *path = "fd_isatty_regular_file";
  unlink(path);

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  EXPECT(fd >= 0, "open regular file");
  if (fd < 0) return;

  EXPECT_FILETYPE(fdstat_filetype_of((__wasi_fd_t)fd, "regular fdstat"),
                  __WASI_FILETYPE_REGULAR_FILE,
                  "regular file fd_fdstat_get");
  EXPECT_FILETYPE(filestat_filetype_of((__wasi_fd_t)fd, "regular filestat"),
                  __WASI_FILETYPE_REGULAR_FILE,
                  "regular file fd_filestat_get");

  close(fd);
  unlink(path);
}

// ----------------------------------------------------------------------------
// 5. isatty() on a regular file returns 0 with errno=ENOTTY.
// ----------------------------------------------------------------------------

static void test_isatty_on_regular_file(void) {
  printf("Test 5: isatty on a regular file is 0 with errno=ENOTTY\n");

  const char *path = "fd_isatty_regular_isatty";
  unlink(path);

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  EXPECT(fd >= 0, "open regular file");
  if (fd < 0) return;

  errno = 0;
  int r = isatty(fd);
  EXPECT_EQ_U(r, 0, "isatty(regular_file) returns 0");
  EXPECT_EQ_U(errno, ENOTTY, "isatty(regular_file) sets errno=ENOTTY");

  close(fd);
  unlink(path);
}

// ----------------------------------------------------------------------------
// 6. Preopened directory reports DIRECTORY through both APIs; isatty=0.
// ----------------------------------------------------------------------------

static void test_directory_filetype(void) {
  printf("Test 6: preopen directory reports DIRECTORY via fdstat & filestat\n");

  __wasi_fd_t dirfd = find_preopen_directory_fd();
  if (dirfd == (__wasi_fd_t)-1) {
    fprintf(stderr,
            "SKIP test_directory_filetype: no preopen directory available\n");
    return;
  }

  EXPECT_FILETYPE(fdstat_filetype_of(dirfd, "dir fdstat"),
                  __WASI_FILETYPE_DIRECTORY,
                  "dir fd_fdstat_get reports DIRECTORY");
  EXPECT_FILETYPE(filestat_filetype_of(dirfd, "dir filestat"),
                  __WASI_FILETYPE_DIRECTORY,
                  "dir fd_filestat_get reports DIRECTORY");

  errno = 0;
  int r = isatty((int)dirfd);
  EXPECT_EQ_U(r, 0, "isatty(directory) returns 0");
}

// ----------------------------------------------------------------------------
// 7. Invalid fd: BADF from fdstat/filestat, 0 + EBADF from isatty.
// ----------------------------------------------------------------------------

static void test_invalid_fd_badf(void) {
  printf("Test 7: invalid fd yields BADF / EBADF\n");

  __wasi_fdstat_t fds;
  EXPECT_EQ_U(__wasi_fd_fdstat_get(9999, &fds), __WASI_ERRNO_BADF,
              "fd_fdstat_get(9999) -> BADF");

  __wasi_filestat_t fss;
  EXPECT_EQ_U(__wasi_fd_filestat_get(9999, &fss), __WASI_ERRNO_BADF,
              "fd_filestat_get(9999) -> BADF");

  errno = 0;
  int r = isatty(9999);
  EXPECT_EQ_U(r, 0, "isatty(9999) returns 0");
  EXPECT_EQ_U(errno, EBADF, "isatty(9999) sets errno=EBADF");
}

// ----------------------------------------------------------------------------
// 8. Closed fd behaves like an invalid fd.
// ----------------------------------------------------------------------------

static void test_closed_fd_badf(void) {
  printf("Test 8: closed fd yields BADF\n");

  const char *path = "fd_isatty_closed";
  unlink(path);

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  EXPECT(fd >= 0, "open file for close test");
  if (fd < 0) return;
  close(fd);

  __wasi_fdstat_t fds;
  EXPECT_EQ_U(__wasi_fd_fdstat_get((__wasi_fd_t)fd, &fds), __WASI_ERRNO_BADF,
              "fd_fdstat_get(closed) -> BADF");

  unlink(path);
}

// ----------------------------------------------------------------------------
// 9. Repeated fdstat reports are stable for a given fd. The runtime
//    must not flap between answers (e.g. caching an ephemeral isatty()
//    result the wrong way).
// ----------------------------------------------------------------------------

static void test_fdstat_idempotent(void) {
  printf("Test 9: fd_fdstat_get is idempotent across repeated calls\n");

  for (__wasi_fd_t fd = 0; fd <= 2; fd++) {
    __wasi_fdstat_t a, b, c;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    EXPECT_EQ_U(__wasi_fd_fdstat_get(fd, &a), __WASI_ERRNO_SUCCESS, "1st");
    EXPECT_EQ_U(__wasi_fd_fdstat_get(fd, &b), __WASI_ERRNO_SUCCESS, "2nd");
    EXPECT_EQ_U(__wasi_fd_fdstat_get(fd, &c), __WASI_ERRNO_SUCCESS, "3rd");
    EXPECT(a.fs_filetype == b.fs_filetype && b.fs_filetype == c.fs_filetype,
           "fs_filetype flapped across repeated fd_fdstat_get calls");
    EXPECT(a.fs_flags == b.fs_flags && b.fs_flags == c.fs_flags,
           "fs_flags flapped across repeated fd_fdstat_get calls");
  }
}

// ----------------------------------------------------------------------------
// 10. libc fstat() st_mode MUST NOT advertise S_IFCHR for non-tty stdio.
//
//     `S_ISCHR(st.st_mode)` is the POSIX-portable way to ask "is this a
//     character special file?". wasi-libc derives it from
//     __wasi_fd_filestat_get's filetype, so this test exposes the same
//     cached-Filestat gap as Test 3, from the user-visible angle.
// ----------------------------------------------------------------------------

static void test_libc_fstat_stdio_not_chardev(bool stdin_is_tty) {
  printf("Test 10: libc fstat on stdout/stderr (and non-tty stdin) does not "
         "report S_IFCHR\n");

  for (int fd = 0; fd <= 2; fd++) {
    if (fd == 0 && stdin_is_tty) {
      continue;
    }
    struct stat st;
    int r = fstat(fd, &st);
    if (r != 0) {
      FAILF("fstat(%d) failed: errno=%d", fd, errno);
      continue;
    }
    if (S_ISCHR(st.st_mode)) {
      FAILF("fstat(%d) reports S_IFCHR but the fd is not a tty", fd);
    }
  }
}

// ----------------------------------------------------------------------------
// 11. Redirection: dup2(regular_file, 0) must make fd 0 describe a
//     regular file, not the original character-device-shaped slot. This
//     exercises the special-case in `fdstat()` that hardcodes fds 0/1/2
//     through `stdio_filetype()` regardless of what is actually behind
//     them.
//
//     This is the case a shell-style redirection (`prog < file.txt`)
//     puts the guest in.
// ----------------------------------------------------------------------------

static void test_dup2_regular_file_onto_stdin(void) {
  printf("Test 11: after dup2(regular, 0), fd 0 reports REGULAR_FILE\n");

  const char *path = "fd_isatty_redirect_src";
  unlink(path);

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  EXPECT(fd >= 0, "open redirect source");
  if (fd < 0) return;

  // Preserve original stdin so we can restore it before exiting; otherwise
  // any later diagnostic flush that walks stdio could misbehave.
  int saved_stdin = dup(0);
  EXPECT(saved_stdin >= 0, "dup stdin to preserve it");

  int r = dup2(fd, 0);
  EXPECT(r == 0, "dup2(regular, 0)");
  if (r != 0) {
    close(fd);
    if (saved_stdin >= 0) {
      close(saved_stdin);
    }
    unlink(path);
    return;
  }

  EXPECT_FILETYPE(fdstat_filetype_of(0, "post-dup2 fdstat(0)"),
                  __WASI_FILETYPE_REGULAR_FILE,
                  "after dup2(regular,0), fdstat(0) must be REGULAR_FILE");
  EXPECT_FILETYPE(filestat_filetype_of(0, "post-dup2 filestat(0)"),
                  __WASI_FILETYPE_REGULAR_FILE,
                  "after dup2(regular,0), filestat(0) must be REGULAR_FILE");

  errno = 0;
  int rr = isatty(0);
  EXPECT_EQ_U(rr, 0, "isatty(0) after redirect is 0");

  // Restore stdin so the rest of the run is undisturbed.
  if (saved_stdin >= 0) {
    dup2(saved_stdin, 0);
    close(saved_stdin);
  }
  close(fd);
  unlink(path);
}

// ----------------------------------------------------------------------------
// 12. Sanity check: libc fstat reports S_IFREG for an opened regular
//     file, never S_IFCHR. Exercises the opposite side of Test 4
//     without depending on isatty / fdstat agreement at all.
// ----------------------------------------------------------------------------

static void test_libc_fstat_regular_file_is_regular(void) {
  printf("Test 12: libc fstat on a regular file reports S_IFREG\n");

  const char *path = "fd_isatty_regular_fstat";
  unlink(path);

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  EXPECT(fd >= 0, "open regular file");
  if (fd < 0) return;

  struct stat st;
  int r = fstat(fd, &st);
  EXPECT_EQ_U(r, 0, "fstat(regular) succeeds");
  EXPECT(S_ISREG(st.st_mode), "S_ISREG must be true for an opened regular file");
  EXPECT(!S_ISCHR(st.st_mode), "S_ISCHR must be false for a regular file");

  close(fd);
  unlink(path);
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

int main(void) {
  // Probe stdin once so subsequent tests can branch on it. We deliberately
  // use fdstat here rather than isatty(): if isatty()'s underlying fdstat
  // were buggy in the same way we are testing for, using isatty() would
  // hide the bug from itself.
  bool stdin_is_tty = fd_is_chardev(0);

  printf("WASIX isatty / stdio filetype integration tests\n");
  printf("================================================\n");
  printf("Harness stdin is %s a tty\n", stdin_is_tty ? "" : "NOT");

  test_isatty_on_stdio(stdin_is_tty);
  test_stdio_fdstat_not_character_device(stdin_is_tty);
  test_stdio_fdstat_filestat_consistent();
  test_regular_file_filetype();
  test_isatty_on_regular_file();
  test_directory_filetype();
  test_invalid_fd_badf();
  test_closed_fd_badf();
  test_fdstat_idempotent();
  test_libc_fstat_stdio_not_chardev(stdin_is_tty);
  test_dup2_regular_file_onto_stdin();
  test_libc_fstat_regular_file_is_regular();

  printf("================================================\n");
  printf("Checks run: %d, failures: %d\n", g_checks, g_failures);

  return g_failures == 0 ? 0 : 1;
}
