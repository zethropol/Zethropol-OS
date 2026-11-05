#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* No system/popen/exec calls, no package APIs. Only proc reads before sealing;
 * thereafter openat is allowed exclusively on our already-open result dir. */
static void die(const char *what) { perror(what); _exit(70); }
static int result_fd;
static const char *invocation;
static const char *label;

static void seal(void) {
    struct sock_filter code[160]; unsigned n = 0;
#define ADD(x) do { code[n++] = (struct sock_filter)x; } while (0)
    ADD(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)));
    ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0));
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS));
    ADD(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)));
    ADD(BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K, 0x40000000U, 0, 1));
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS));
    const int denied[] = {SYS_execve, SYS_execveat, SYS_open, SYS_openat2,
        SYS_creat, SYS_socket, SYS_socketpair, SYS_connect, SYS_sendmsg, SYS_sendto,
        SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_unshare, SYS_setns,
        SYS_ptrace, SYS_process_vm_writev, SYS_bpf, SYS_io_uring_setup,
        SYS_unlink, SYS_unlinkat, SYS_rename, SYS_renameat, SYS_renameat2,
        SYS_link, SYS_linkat, SYS_symlink, SYS_symlinkat, SYS_mkdir, SYS_mkdirat,
        SYS_rmdir, SYS_truncate, SYS_memfd_create};
    for (unsigned i = 0; i < sizeof(denied)/sizeof(*denied); i++) {
        ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (unsigned)denied[i], 0, 1));
        ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM));
    }
    ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_openat, 0, 4));
    ADD(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[0])));
    ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (unsigned)result_fd, 1, 0));
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM));
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW));
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW));
    struct sock_fprog program = {(unsigned short)n, code};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) die("seccomp");
#undef ADD
}
static int output(const char *prefix, int per_invocation) {
    char name[160];
    if (per_invocation) snprintf(name, sizeof(name), "%s-%s-%s.txt", prefix, label, invocation);
    else snprintf(name, sizeof(name), "%s-%s.txt", prefix, label);
    int fd = openat(result_fd, name, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (fd < 0) die("result output");
    return fd;
}
static const char *env(const char *key) { const char *v = getenv(key); return v ? v : "unset"; }
int main(int argc, char **argv) {
    if (argc != 4 || geteuid() != 0) return 64;
    const char *mode = argv[1]; label = argv[3]; invocation = env("INVOCATION_ID");
    if (strspn(label, "abcdefghijklmnopqrstuvwxyz") != strlen(label) || strlen(label) > 24) return 64;
    if (strlen(invocation) != 32 || strspn(invocation, "0123456789abcdef") != 32) return 64;
    const char *prefix = "/run/zethropol-host-worker-test-";
    if (strncmp(argv[2], prefix, strlen(prefix)) != 0 || strstr(argv[2], "..")) return 64;
    result_fd = open(argv[2], O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if (result_fd < 0 || fstat(result_fd, &st) || st.st_uid != 0 || (st.st_mode & 0777) != 0700) return 65;
    if (!strcmp(mode, "post")) {
        seal(); int fd = output("post", 1);
        dprintf(fd, "SERVICE_RESULT=%s\nEXIT_CODE=%s\nEXIT_STATUS=%s\n", env("SERVICE_RESULT"), env("EXIT_CODE"), env("EXIT_STATUS"));
        close(fd); return 0;
    }
    if (strcmp(mode, "probe") && strcmp(mode, "hold") && strcmp(mode, "children")) return 64;
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) die("read status");
    char *line = NULL; size_t size = 0; int nnp = 0;
    while (getline(&line, &size, status) >= 0) if (!strcmp(line, "NoNewPrivs:\t1\n")) nnp = 1;
    fclose(status); free(line);
    if (!nnp) return 66; /* Record unit NNP, not merely our own later seal. */
    char mount_ns[128];
    ssize_t mount_ns_len = readlink("/proc/self/ns/mnt", mount_ns, sizeof(mount_ns) - 1);
    if (mount_ns_len < 0) die("read mount namespace id");
    mount_ns[mount_ns_len] = '\0';
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts) die("read mountinfo");
    char snapshot[24000] = ""; size_t used = 0; line = NULL; size = 0;
    while (getline(&line, &size, mounts) >= 0) {
        char point[1024], opts[1024];
        if (sscanf(line, "%*s %*s %*s %*s %1023s %1023s", point, opts) == 2 &&
            (!strcmp(point,"/") || !strcmp(point,"/boot") || !strcmp(point,"/usr") || !strcmp(point,"/efi") || !strcmp(point,"/etc") || !strcmp(point,"/tmp"))) {
            int count = snprintf(snapshot + used, sizeof(snapshot) - used, "MOUNTINFO %s", line);
            if (count < 0 || (size_t)count >= sizeof(snapshot) - used) return 67;
            used += (size_t)count;
        }
    }
    fclose(mounts); free(line);
    const char *paths[] = {"/usr", "/boot", "/efi"};
    for (unsigned i = 0; i < 3; i++) {
        struct statvfs vfs;
        int r = statvfs(paths[i], &vfs);
        int count = snprintf(snapshot + used, sizeof(snapshot) - used, "VFS %s %s\n", paths[i],
            r == 0 ? ((vfs.f_flag & ST_RDONLY) ? "ro" : "rw") : (errno == ENOENT ? "absent" : "error"));
        if (count < 0 || (size_t)count >= sizeof(snapshot) - used) return 67;
        used += (size_t)count;
    }
    seal();
    errno = 0;
    if (syscall(SYS_openat, AT_FDCWD, "/usr/bin/pacman", O_RDONLY, 0) != -1 || errno != EPERM) return 68;
    errno = 0;
    if (syscall(SYS_execve, "/usr/bin/true", NULL, NULL) != -1 || errno != EPERM) return 69;
    int fd = output("start", 1);
    dprintf(fd, "PID=%ld\nINVOCATION_ID=%s\nMNT_NS=%s\nNoNewPrivs=1\nPACKAGE_ACCESS_DENIED=1\nEXEC_DENIED=1\n%s", (long)getpid(), invocation, mount_ns, snapshot);
    close(fd);
    fd = output("ready", 0); dprintf(fd, "%ld\n%s\n", (long)getpid(), invocation); close(fd);
    alarm(80);
    if (!strcmp(mode, "children")) {
        pid_t pid = fork();
        if (pid < 0) die("fork fixture child");
        if (pid == 0) {
            signal(SIGTERM, SIG_IGN); alarm(80);
            fd = output("child", 0); dprintf(fd, "%ld\n", (long)getpid()); close(fd);
            for (;;) pause();
        }
    }
    if (!strcmp(mode, "probe")) {
        for (int i = 0; i < 2000; i++) {
            fd = openat(result_fd, "finish-probe", O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            if (fd >= 0) { close(fd); return 0; }
            usleep(20000);
        }
        return 71;
    }
    for (;;) pause();
}
