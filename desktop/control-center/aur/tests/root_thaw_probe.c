/* Adversarial setup test only: a new namespace must not create a writable
 * alias of the frozen superblock. No host pathname is ever written. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>
static int text(const char *p,const char *s) {
    int fd=open(p,O_WRONLY|O_CLOEXEC); if(fd<0)return -1;
    ssize_t n=write(fd,s,strlen(s));close(fd);return n==(ssize_t)strlen(s)?0:-1;
}
int main(int argc,char **argv) {
    if(argc!=2 || getuid()==0)return 2;
    int root=atoi(argv[1]);uid_t uid=getuid();gid_t gid=getgid();char map[64];
    if(unshare(CLONE_NEWUSER)<0 || text("/proc/self/setgroups","deny")<0)return 1;
    snprintf(map,sizeof(map),"%u %u 1",uid,uid);if(text("/proc/self/uid_map",map)<0)return 1;
    snprintf(map,sizeof(map),"%u %u 1",gid,gid);if(text("/proc/self/gid_map",map)<0 || unshare(CLONE_NEWNS)<0)return 1;
    int clone=open_tree(root,"",AT_EMPTY_PATH|OPEN_TREE_CLONE|OPEN_TREE_CLOEXEC);
    if(clone<0)return 1;
    struct mount_attr attr={.attr_clr=MOUNT_ATTR_RDONLY};
    int cleared=mount_setattr(clone,"",AT_EMPTY_PATH,&attr,sizeof(attr));
    if(cleared<0 && errno!=EPERM)return 1;
    int context=fspick(clone,"",FSPICK_EMPTY_PATH|FSPICK_CLOEXEC);
    if(context>=0) {
        if(fsconfig(context,FSCONFIG_SET_FLAG,"rw",NULL,0)==0 &&
           fsconfig(context,FSCONFIG_CMD_RECONFIGURE,NULL,NULL,0)==0)return 1;
        close(context);
    } else if(errno!=EPERM)return 1;
    int fd=openat(clone,".forbidden-write-alias",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd>=0 || errno!=EROFS)return 1;
    puts("PASS: readonly clone cannot reconfigure or write frozen superblock");
    close(clone);return 0;
}
