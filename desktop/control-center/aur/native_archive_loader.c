/* Disposable-guest Archive native loader mechanism; NO installation authority.
 * The native domain is fixed at process startup, never supplied by argv/env.
 * libalpm may invoke GPGME internally: run the real binary only in a separately
 * approved disposable guest with reviewed public trust snapshots. Workspace
 * tests replace ALPM entirely and never execute GPG or read private keys.
 */
#define _GNU_SOURCE
#include <alpm.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/xattr.h>
#include <unistd.h>

#define ARCHIVE_DOMAIN "/etc/zethropol-update/aur-native-trust"
#define PACKAGE_LIMIT (256ULL*1024*1024)
#define SIGNATURE_LIMIT (1024ULL*1024)
static int questioned;
static void deny_question(void *context,alpm_question_t *question) {
    (void)context;questioned=1;question->any.answer=0;
}
static int no_acl(int fd) {
    char names[8192];ssize_t count=flistxattr(fd,names,sizeof(names));
    if(count<0)return 0;
    for(ssize_t i=0;i<count;) {
        const char *name=names+i;
        if(!strcmp(name,"system.posix_acl_access") || !strcmp(name,"system.posix_acl_default"))return 0;
        i+=(ssize_t)strlen(name)+1;
    }
    return 1;
}
static int private_dir(const char *path) {
    char resolved[PATH_MAX];
    if(!realpath(path,resolved) || strcmp(path,resolved))return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat s;
    if(fd<0)return -1;
    if(fstat(fd,&s)<0 || !S_ISDIR(s.st_mode) || s.st_uid!=getuid() ||
       (s.st_mode&07777)!=0700 || !no_acl(fd)) {close(fd);return -1;}
    return fd;
}
static int linked_directory(int fd,const char *path) {
    struct stat pinned,linked;
    return fstat(fd,&pinned)==0 && lstat(path,&linked)==0 && S_ISDIR(linked.st_mode) &&
           pinned.st_dev==linked.st_dev && pinned.st_ino==linked.st_ino &&
           pinned.st_uid==getuid() && (pinned.st_mode&07777)==0700 && no_acl(fd);
}
static int input_fd(const char *text,unsigned long long limit) {
    char *end=NULL;errno=0;long value=strtol(text,&end,10);
    if(errno || !*text || !end || *end || value<3 || value>INT_MAX)return -1;
    int fd=(int)value;struct stat s;
    const int required=F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
    int seals=fcntl(fd,F_GET_SEALS);
    if(fstat(fd,&s)<0 || !S_ISREG(s.st_mode) || s.st_size<=0 ||
       (unsigned long long)s.st_size>limit || seals<0 || (seals&required)!=required)return -1;
    return fd;
}
static int copy_input(int source,int directory,const char *name) {
    int output=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400);
    if(output<0)return -1;
    struct stat info;int ok=0;off_t offset=0;char buffer[65536];
    if(fstat(source,&info)<0)goto done;
    while(offset<info.st_size) {
        ssize_t n=pread(source,buffer,sizeof(buffer),offset);if(n<=0)goto done;
        for(ssize_t written=0;written<n;) {
            ssize_t count=write(output,buffer+written,(size_t)(n-written));
            if(count<=0)goto done;
            written+=count;
        }
        offset+=n;
    }
    ok=fsync(output)==0;
done:
    close(output);return ok?0:-1;
}
int main(int argc,char **argv) {
    if(getuid()==0 || geteuid()==0 || argc!=5) {
        fputs("unprivileged disposable root/db and two sealed input FDs required\n",stderr);return 2;
    }
    /* These are ONLY disposable loader storage paths. Neither may select trust. */
    int root=private_dir(argv[1]),db=private_dir(argv[2]);
    int package=input_fd(argv[3],PACKAGE_LIMIT),signature=input_fd(argv[4],SIGNATURE_LIMIT);
    if(root<0 || db<0 || package<0 || signature<0 || package==signature) {
        if(root>=0)close(root);
        if(db>=0)close(db);
        fputs("invalid private storage or immutable input\n",stderr);return 2;
    }
    struct stat a,b,package_info,signature_info;
    if(fstat(package,&package_info)<0 || fstat(signature,&signature_info)<0 ||
       (package_info.st_dev==signature_info.st_dev && package_info.st_ino==signature_info.st_ino)) {
        close(root);close(db);return 2;
    }
    if(fstat(root,&a)<0 || fstat(db,&b)<0 || (a.st_dev==b.st_dev && a.st_ino==b.st_ino)) {
        close(root);close(db);return 2;
    }
    if(clearenv()!=0 || setenv("PATH","/usr/bin",1)!=0 || setenv("LC_ALL","C",1)!=0 ||
       setenv("GNUPGHOME",ARCHIVE_DOMAIN,1)!=0) {close(root);close(db);return 2;}
    /* Public evidence must bind these fixed snapshots independently. This
     * mechanism never imports a key or upgrades a trust value. */
    unsigned char nonce[16];char staging[64]=".native-archive-";
    if(getrandom(nonce,sizeof(nonce),0)!=(ssize_t)sizeof(nonce)) {close(root);close(db);return 2;}
    for(size_t i=0;i<sizeof(nonce);i++)snprintf(staging+16+2*i,3,"%02x",nonce[i]);
    /* Keep all test/guest artifacts on failure; no cleanup/deletion authority. */
    if(mkdirat(root,staging,0700)<0) {close(root);close(db);return 2;}
    int stage=openat(root,staging,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(stage<0 || copy_input(package,stage,"package.pkg.tar")<0 ||
       copy_input(signature,stage,"package.pkg.tar.sig")<0 || fsync(stage)<0) {
        if(stage>=0)close(stage);
        close(root);close(db);return 2;
    }
    char root_path[64],db_path[64],package_path[96];
    snprintf(root_path,sizeof(root_path),"/proc/self/fd/%d",root);
    snprintf(db_path,sizeof(db_path),"/proc/self/fd/%d",db);
    snprintf(package_path,sizeof(package_path),"/proc/self/fd/%d/package.pkg.tar",stage);
    alpm_errno_t error;alpm_handle_t *h=alpm_initialize(root_path,db_path,&error);
    if(!h) {close(stage);close(root);close(db);return 2;}
    int result=1;alpm_pkg_t *loaded=NULL;
    /* Exactly one assignment, before any signature load; no process switch. */
    const int policy=ALPM_SIG_PACKAGE;
    if(alpm_option_set_gpgdir(h,ARCHIVE_DOMAIN)<0 ||
       alpm_option_set_default_siglevel(h,policy)<0 || alpm_option_set_local_file_siglevel(h,policy)<0 || alpm_option_set_questioncb(h,deny_question,NULL)<0)goto done;
    if(alpm_pkg_load(h,package_path,1,policy,&loaded)<0)goto done;
    if(questioned || !(alpm_pkg_get_validation(loaded)&ALPM_PKG_VALIDATION_SIGNATURE))goto done;
    struct stat stage_info,linked_stage;
    if(!linked_directory(root,argv[1]) || !linked_directory(db,argv[2]) ||
       fstat(stage,&stage_info)<0 || fstatat(root,staging,&linked_stage,AT_SYMLINK_NOFOLLOW)<0 ||
       !S_ISDIR(linked_stage.st_mode) || stage_info.st_dev!=linked_stage.st_dev ||
       stage_info.st_ino!=linked_stage.st_ino)goto done;
    puts("NATIVE-LOADER-VERIFIED-ONLY: no transaction, no official dependency authority, no install grant");
    result=0;
done:
    if(loaded)alpm_pkg_free(loaded);
    alpm_release(h);close(stage);close(root);close(db);return result;
}
