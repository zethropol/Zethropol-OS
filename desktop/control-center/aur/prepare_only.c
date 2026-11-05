/* Development-only ALPM preflight. There is deliberately no commit, shell,
 * makepkg, PKGBUILD, PolicyKit or root execution path in this executable.
 * Default compilation requires package + database signatures. The unsigned
 * macro is used ONLY by temporary fixture binaries in tests.
 */
#define _GNU_SOURCE
#include <alpm.h>
#include <alpm_list.h>
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
    questioned=1;
    question->any.answer=0;
    if(question->type==ALPM_QUESTION_SELECT_PROVIDER) question->select_provider.use_index=-1;
}
static int private_directory(const char *path) {
    struct stat s;
    return lstat(path,&s)==0 && S_ISDIR(s.st_mode) && s.st_uid==getuid() && (s.st_mode&077)==0;
}
static int sealed_fd(int fd) {
    struct stat s; int required=F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
    int seals=fcntl(fd,F_GET_SEALS);
    return fstat(fd,&s)==0 && S_ISREG(s.st_mode) && s.st_size>0 && seals>=0 && (seals&required)==required;
}
static int copy_sealed(int source,const char *path,size_t limit) {
    struct stat info; if(!sealed_fd(source) || fstat(source,&info)<0 || (unsigned long long)info.st_size>limit) return -1;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400); if(fd<0) return -1;
    char buffer[65536];off_t offset=0;int ok=0;
    while(offset<info.st_size) {
        ssize_t n=pread(source,buffer,sizeof(buffer),offset);if(n<=0) goto done;
        for(ssize_t written=0;written<n;) { ssize_t r=write(fd,buffer+written,(size_t)(n-written));if(r<=0) goto done;written+=r; }
        offset+=n;
    }
    if(fsync(fd)==0) ok=1;
done:
    close(fd);return ok?0:-1;
}
static int error(alpm_handle_t *handle, const char *phase) {
    fprintf(stderr,"%s: %s\n",phase,alpm_strerror(alpm_errno(handle)));
    return 1;
}
int main(int argc,char **argv) {
    if(getuid()==0 || geteuid()==0 || argc<6 || argc>69) { fputs("desktop-user preflight only\n",stderr); return 2; }
    /* root database architecture comma-separated-repositories archive-FD... */
    char root[PATH_MAX],database[PATH_MAX];
    if(!realpath(argv[1],root) || !realpath(argv[2],database) ||
       !private_directory(root) || !private_directory(database) ||
       strncmp(database,root,strlen(root)) || database[strlen(root)]!='/') {
        fputs("private build/lab root and nested database required\n",stderr); return 2;
    }
    alpm_errno_t code;
    alpm_handle_t *h=alpm_initialize(root,database,&code);
    if(!h) { fprintf(stderr,"initialize: %s\n",alpm_strerror(code)); return 1; }
    int result=1,transaction=0;
    char staging[PATH_MAX+64];snprintf(staging,sizeof(staging),"%s/.aur-intake-XXXXXX",root);
    if(!mkdtemp(staging)) { alpm_release(h);return 1; }
    char gpgdir[PATH_MAX+32];snprintf(gpgdir,sizeof(gpgdir),"%s/gnupg",root);
    if((mkdir(gpgdir,0700)<0 && errno!=EEXIST) || !private_directory(gpgdir) || alpm_option_set_gpgdir(h,gpgdir)<0) {
        fputs("private signature keyring required\n",stderr);goto done;
    }
#ifdef ZETHROPOL_TEST_UNSIGNED
    const int policy=0; /* Never shipped; synthetic unsigned fixtures only. */
#else
    const int policy=ALPM_SIG_PACKAGE|ALPM_SIG_DATABASE;
#endif
    if(alpm_option_set_default_siglevel(h,policy)<0 ||
       alpm_option_set_local_file_siglevel(h,policy)<0 ||
       alpm_option_add_architecture(h,argv[3])<0 ||
       alpm_option_set_questioncb(h,deny_question,NULL)<0) goto done;
    char *repositories=strdup(argv[4]);
    if(!repositories) goto done;
    for(char *r=strtok(repositories,",");r;r=strtok(NULL,",")) {
        if(!strcmp(r,"-")) continue;
        for(char *c=r;*c;c++) if(!((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_')) {
            free(repositories); goto done;
        }
        alpm_db_t *db=alpm_register_syncdb(h,r,policy);
        if(!db || alpm_db_get_valid(db)<0) { free(repositories); error(h,"repository verification"); goto done; }
    }
    free(repositories);
    if(alpm_trans_init(h,0)<0) { error(h,"transaction lock"); goto done; }
    transaction=1;
    if(alpm_sync_sysupgrade(h,0)<0 || questioned) { error(h,"official compatibility"); goto done; }
    unsigned long long intake_bytes=0;
    for(int i=5;i<argc;i++) {
        char *end;
        long fd=strtol(argv[i],&end,10);
        if(fd<3 || fd>INT_MAX || !sealed_fd((int)fd)) { fputs("sealed archive FD required\n",stderr); goto done; }
        struct stat archive_info;
        if(fstat((int)fd,&archive_info)<0) goto done;
        intake_bytes+=(unsigned long long)archive_info.st_size;
        if(intake_bytes>320ULL*1024*1024) { fputs("aggregate intake quota exceeded\n",stderr); goto done; }
        char archive[PATH_MAX+128];
        if(*end==':') {
            char *sig_end;long signature=strtol(end+1,&sig_end,10);
            if(*sig_end || signature<3 || signature>INT_MAX || !sealed_fd((int)signature)) goto done;
            struct stat signature_info;
            if(fstat((int)signature,&signature_info)<0) goto done;
            intake_bytes+=(unsigned long long)signature_info.st_size;
            if(intake_bytes>320ULL*1024*1024) { fputs("aggregate intake quota exceeded\n",stderr); goto done; }
            snprintf(archive,sizeof(archive),"%s/%d.pkg.tar",staging,i);
            char sigpath[PATH_MAX+132];snprintf(sigpath,sizeof(sigpath),"%s.sig",archive);
            if(copy_sealed((int)fd,archive,256*1024*1024)<0 || copy_sealed((int)signature,sigpath,1024*1024)<0) goto done;
        } else {
            if(*end) goto done;
            snprintf(archive,sizeof(archive),"/proc/self/fd/%ld",fd);
        }
        alpm_pkg_t *pkg=NULL;
        if(alpm_pkg_load(h,archive,1,policy,&pkg)<0) { error(h,"archive metadata/signature"); goto done; }
        for(alpm_list_t *p=alpm_get_syncdbs(h);p;p=p->next) {
            if(alpm_db_get_pkg(p->data,alpm_pkg_get_name(pkg))) {
                fputs("AUR target belongs to official repository\n",stderr); alpm_pkg_free(pkg); goto done;
            }
        }
        if(alpm_add_pkg(h,pkg)<0) { alpm_pkg_free(pkg); error(h,"add archive"); goto done; }
    }
    alpm_list_t *details=NULL;
    if(alpm_trans_prepare(h,&details)<0 || questioned) {
        /* Process exits after release; details contain error-specific objects. */
        error(h,"dependency/conflict prepare"); goto done;
    }
    if(alpm_trans_get_remove(h)) { fputs("unreviewed removals refused\n",stderr); goto done; }
    int official=0;
    for(alpm_list_t *p=alpm_trans_get_add(h);p;p=p->next) {
        alpm_pkg_t *pkg=p->data;
        if(alpm_pkg_get_origin(pkg)==ALPM_PKG_FROM_SYNCDB) official=1;
        printf("ADD\t%s\t%s\t%s\t%s\n",alpm_pkg_get_name(pkg),alpm_pkg_get_version(pkg),
               alpm_pkg_get_arch(pkg),alpm_pkg_get_origin(pkg)==ALPM_PKG_FROM_SYNCDB?"official":"archive");
    }
    if(official) { fputs("official upgrade/dependency transaction must complete and be reviewed first\n",stderr); goto done; }
    puts("PREPARED-ONLY: no install authorized or performed");
    result=0;
done:
    if(transaction) alpm_trans_release(h);
    alpm_release(h);
    for(int i=5;i<argc;i++) {
        char path[PATH_MAX+132];snprintf(path,sizeof(path),"%s/%d.pkg.tar.sig",staging,i);unlink(path);
        snprintf(path,sizeof(path),"%s/%d.pkg.tar",staging,i);unlink(path);
    }
    rmdir(staging);
    return result;
}
