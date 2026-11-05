/* LAB ONLY: real libalpm transactions in unprivileged disposable /tmp roots.
 * Never packaged, invoked by the broker, or usable as production acceptance.
 * Each invocation establishes exactly one native trust domain before loading.
 */
#define _GNU_SOURCE
#ifndef ZETHROPOL_LAB_ONLY
#error "This probe must be explicitly compiled for the disposable LAB tests"
#endif
#include <alpm.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int questioned;
static void deny_question(void *context, alpm_question_t *question) {
    (void)context;
    questioned = 1;
    question->any.answer = 0;
}
static int lab_directory(const char *path) {
    char resolved[PATH_MAX];
    struct stat info;
    const char *prefix = "/tmp/zethropol-domain-lab-";
    return realpath(path, resolved) && !strcmp(path, resolved)
        && !strncmp(resolved, prefix, strlen(prefix))
        && lstat(path, &info) == 0 && S_ISDIR(info.st_mode)
        && info.st_uid == getuid() && (info.st_mode & 07777) == 0700;
}
static int copy_sealed(const char *number, const char *destination, off_t maximum) {
    char *end;
    errno = 0;
    long value = strtol(number, &end, 10);
    if (errno || !*number || *end || value < 3 || value > INT_MAX) return -1;
    int source = (int)value;
    struct stat info;
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl(source, F_GET_SEALS);
    if (fstat(source, &info) < 0 || !S_ISREG(info.st_mode)
        || info.st_size <= 0 || info.st_size > maximum || seals < 0
        || (seals & required) != required) return -1;
    int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0400);
    if (output < 0) return -1;
    char buffer[65536];
    int result = -1;
    for (off_t offset = 0; offset < info.st_size;) {
        ssize_t count = pread(source, buffer, sizeof(buffer), offset);
        if (count <= 0) goto done;
        for (ssize_t written = 0; written < count;) {
            ssize_t n = write(output, buffer + written, (size_t)(count - written));
            if (n <= 0) goto done;
            written += n;
        }
        offset += count;
    }
    result = fsync(output);
done:
    close(output);
    return result;
}
int main(int argc, char **argv) {
    if (getuid() == 0 || geteuid() == 0 || getuid() != geteuid() || argc != 9) return 2;
    if (!lab_directory(argv[1]) || !lab_directory(argv[2]) || !lab_directory(argv[3])) return 2;
    char local_db[PATH_MAX];
    if (!strcmp(argv[1], argv[2]) || !strcmp(argv[1], argv[3]) || !strcmp(argv[2], argv[3])
        || snprintf(local_db, sizeof(local_db), "%s/local", argv[2]) >= (int)sizeof(local_db)
        || !lab_directory(local_db)) return 2;
    char package_path[PATH_MAX], signature_path[PATH_MAX], log_path[PATH_MAX];
    if (snprintf(package_path, sizeof(package_path), "%s/.lab.pkg.tar", argv[1]) >= (int)sizeof(package_path)
        || snprintf(signature_path, sizeof(signature_path), "%s.sig", package_path) >= (int)sizeof(signature_path)
        || snprintf(log_path, sizeof(log_path), "%s/lab.log", argv[1]) >= (int)sizeof(log_path)) return 2;
    if (copy_sealed(argv[4], package_path, 16 * 1024 * 1024) < 0
        || copy_sealed(argv[5], signature_path, 1024 * 1024) < 0) return 2;
    if (clearenv() || setenv("PATH", "/usr/bin", 1) || setenv("LC_ALL", "C", 1)
        || setenv("GNUPGHOME", argv[3], 1)) return 2;
    alpm_errno_t error;
    alpm_handle_t *handle = alpm_initialize(argv[1], argv[2], &error);
    if (!handle) return 2;
    int result = 1, transaction = 0;
    alpm_pkg_t *package = NULL;
    /* Required/TrustedOnly: no Optional/MarginalOK/UnknownOK or domain switch. */
    if (alpm_option_set_gpgdir(handle, argv[3]) < 0
        || alpm_option_set_default_siglevel(handle, ALPM_SIG_PACKAGE) < 0
        || alpm_option_set_local_file_siglevel(handle, ALPM_SIG_PACKAGE) < 0
        || alpm_option_set_questioncb(handle, deny_question, NULL) < 0
        || alpm_option_set_hookdirs(handle, NULL) < 0
        || alpm_option_set_logfile(handle, log_path) < 0
        || alpm_option_add_architecture(handle, "x86_64") < 0) goto done;
    if (alpm_pkg_load(handle, package_path, 1, ALPM_SIG_PACKAGE, &package) < 0) goto done;
    if (questioned || !(alpm_pkg_get_validation(package) & ALPM_PKG_VALIDATION_SIGNATURE)
        || strcmp(alpm_pkg_get_name(package), argv[6])
        || strcmp(alpm_pkg_get_version(package), argv[7])
        || !alpm_pkg_get_arch(package) || strcmp(alpm_pkg_get_arch(package), argv[8])) goto done;
    /* No registered sync repositories: no implicit downloads/dependency installs. */
    if (alpm_trans_init(handle, 0) < 0) goto done;
    transaction = 1;
    if (alpm_add_pkg(handle, package) < 0) goto done;
    package = NULL; /* transaction owns it */
    alpm_list_t *details = NULL;
    if (alpm_trans_prepare(handle, &details) < 0 || questioned
        || alpm_list_count(alpm_trans_get_add(handle)) != 1
        || alpm_trans_get_remove(handle)) goto done;
    if (alpm_trans_commit(handle, &details) < 0) goto done;
    alpm_pkg_t *installed = alpm_db_get_pkg(alpm_get_localdb(handle), argv[6]);
    if (!installed || strcmp(alpm_pkg_get_version(installed), argv[7])) goto done;
    printf("LAB-COMMITTED %s %s\n", argv[6], argv[7]);
    result = 0;
done:
    if (result) fprintf(stderr, "LAB native rejection: %s\n", alpm_strerror(alpm_errno(handle)));
    if (package) alpm_pkg_free(package);
    if (transaction) alpm_trans_release(handle);
    alpm_release(handle);
    return result;
}
