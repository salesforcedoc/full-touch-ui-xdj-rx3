/* rbp_process.h — shared "am I rbp?" test for the RX3-native shims (loaded from
 * /etc/ld.so.preload into every process). Match on argv[0] only — never on a
 * substring of the whole command line, or a shell running "... /root/pdj/rbp ..."
 * would activate the shim (and crash reading rbp-only addresses).
 * rbp:  argv[0] basename == "rbp"  (apl_start: /root/pdj/rbp; manual: ./rbp)
 *   or  argv[0] basename starts "ld-linux" and argv[1] basename == "rbp". */
static int rbp_argv_is_rbp(const char *a)
{
    const char *b = strrchr(a, '/');
    return strcmp(b ? b + 1 : a, "rbp") == 0;
}
static int rbp_process_check(void)
{
    char cmd[256];
    int fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = syscall(SYS_read, fd, cmd, sizeof cmd - 1);
    syscall(SYS_close, fd);
    if (n <= 0) return 0;
    cmd[n] = 0;
    const char *a0 = cmd, *a1 = cmd + strlen(cmd) + 1;
    if (rbp_argv_is_rbp(a0)) return 1;
    const char *b = strrchr(a0, '/');
    if (strncmp(b ? b + 1 : a0, "ld-linux", 8) == 0 && a1 < cmd + n && rbp_argv_is_rbp(a1)) return 1;
    return 0;
}
