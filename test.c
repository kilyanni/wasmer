/* stdin_flush_test.c
 *
 * Build:  cc -Wall -O2 stdin_flush_test.c -o stdin_flush_test
 *
 * Try:
 *   ./stdin_flush_test                       # tty
 *   echo hello | ./stdin_flush_test          # pipe
 *   ./stdin_flush_test < /etc/os-release     # regular file
 *   ./stdin_flush_test < /dev/zero           # char device
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>

static const char *kind(mode_t m) {
    if (S_ISREG(m))  return "regular file";
    if (S_ISDIR(m))  return "directory";
    if (S_ISCHR(m))  return "char device";
    if (S_ISBLK(m))  return "block device";
    if (S_ISFIFO(m)) return "fifo/pipe";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static void try_call(const char *label, int rc) {
    if (rc == 0)
        printf("  %-22s -> ok\n", label);
    else
        printf("  %-22s -> -1, errno=%d (%s)\n",
               label, errno, strerror(errno));
}

int main(void) {
    struct stat st;
    if (fstat(STDIN_FILENO, &st) < 0) {
        perror("fstat");
        return 1;
    }

    int fl = fcntl(STDIN_FILENO, F_GETFL);
    const char *acc = "?";
    switch (fl & O_ACCMODE) {
        case O_RDONLY: acc = "O_RDONLY"; break;
        case O_WRONLY: acc = "O_WRONLY"; break;
        case O_RDWR:   acc = "O_RDWR";   break;
    }

    printf("stdin: %s, access=%s, isatty=%d\n",
           kind(st.st_mode), acc, isatty(STDIN_FILENO));
    printf("------------------------------------------------\n");

    /* 1. fflush(stdin) -- UB per POSIX, but glibc has a defined extension */
    errno = 0;
    int r = fflush(stdin);
    try_call("fflush(stdin)", r);

    /* 2. fsync(0) */
    errno = 0;
    r = fsync(STDIN_FILENO);
    try_call("fsync(0)", r);

    /* 3. fdatasync(0) */
    errno = 0;
    r = fdatasync(STDIN_FILENO);
    try_call("fdatasync(0)", r);

    /* 4. tcflush(0, TCIFLUSH) -- only meaningful on a tty */
    errno = 0;
    r = tcflush(STDIN_FILENO, TCIFLUSH);
    try_call("tcflush(0,TCIFLUSH)", r);

    /* 5. lseek(0, 0, SEEK_CUR) -- can stdin even report a position? */
    errno = 0;
    off_t off = lseek(STDIN_FILENO, 0, SEEK_CUR);
    if (off == (off_t)-1)
        printf("  %-22s -> -1, errno=%d (%s)\n",
               "lseek(0,0,SEEK_CUR)", errno, strerror(errno));
    else
        printf("  %-22s -> %lld\n",
               "lseek(0,0,SEEK_CUR)", (long long)off);

    /* 6. posix_fadvise on stdin (DONTNEED) -- only sensible on regular files */
    errno = 0;
    r = posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_DONTNEED);
    /* posix_fadvise returns the errno value directly, not -1 */
    if (r == 0)
        printf("  %-22s -> ok\n", "posix_fadvise DONTNEED");
    else
        printf("  %-22s -> err=%d (%s)\n",
               "posix_fadvise DONTNEED", r, strerror(r));

    return 0;
}
