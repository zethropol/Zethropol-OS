/* Unprivileged, detached tmpfs: one mount FD, no host staging path.
 * Protocol: S + mount FD; caller closes every writer and sends F;
 * mount_setattr makes that same mount readonly; R; helper exits.
 * Never exports namespace/context FDs or accepts a caller filesystem type.
 * --run attaches a readonly clone privately and execs only /usr/bin/bwrap.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>

static int text(const char *path, const char *value) {
    int fd=open(path,O_WRONLY|O_CLOEXEC); if(fd<0) return -1;
    size_t n=strlen(value); ssize_t r=write(fd,value,n); close(fd);
    return r==(ssize_t)n?0:-1;
}
static unsigned long long number(const char *s) {
    char *end; errno=0; unsigned long long n=strtoull(s,&end,10);
    if(errno || !*s || *end || *s=='-') return 0;
    return n;
}
int main(int argc,char **argv) {
    if(getuid()==0 || geteuid()==0) return 2;
    int run=argc>=7 && !strcmp(argv[1],"--run");
    unsigned long long bytes=0,nodes=0,channel=0;
    int input=-1;
    if(run) {
        unsigned long long handle=number(argv[2]);
        if(handle<3 || handle>1048576 || strcmp(argv[3],"--") || strcmp(argv[4],"/usr/bin/bwrap")) return 2;
        input=(int)handle;
        struct statvfs fs;
        if(fstatvfs(input,&fs)<0 || (fs.f_flag&(ST_RDONLY|ST_NOSUID|ST_NODEV))!=(ST_RDONLY|ST_NOSUID|ST_NODEV)) return 2;
    } else {
        if(argc!=4) return 2;
        bytes=number(argv[1]);nodes=number(argv[2]);channel=number(argv[3]);
        if(bytes<4096 || bytes>2147483648ULL || nodes<16 || nodes>65536 || channel<3 || channel>1048576) return 2;
    }
    uid_t uid=getuid(); gid_t gid=getgid(); char value[64];
    pid_t parent=getppid();
    if(prctl(PR_SET_PDEATHSIG,SIGKILL)<0 || getppid()!=parent) return 1;
    alarm(300);
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)<0 || unshare(CLONE_NEWUSER)<0) goto error;
    if(text("/proc/self/setgroups","deny")<0) goto error;
    snprintf(value,sizeof(value),"%u %u 1",uid,uid);
    if(text("/proc/self/uid_map",value)<0) goto error;
    snprintf(value,sizeof(value),"%u %u 1",gid,gid);
    if(text("/proc/self/gid_map",value)<0 || unshare(CLONE_NEWNS)<0) goto error;
    if(prctl(PR_SET_DUMPABLE,0)<0) goto error;
    if(run) {
        /* A detached FD resolves as / in procfs. Attach a readonly clone only
         * inside this private namespace so bwrap's dirfd race check can resolve
         * and verify its path. Never expose a namespace FD to the payload. */
        if(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)<0) goto error;
        int clone=open_tree(input,"",AT_EMPTY_PATH|OPEN_TREE_CLONE|OPEN_TREE_CLOEXEC);
        if(clone<0) { perror("clone detached root"); return 1; }
        if(mount("tmpfs","/mnt","tmpfs",MS_NOSUID|MS_NODEV,"size=65536,nr_inodes=16,mode=0700")<0) { perror("private attach tmpfs"); return 1; }
        if(mkdir("/mnt/aur-root",0700)<0 || move_mount(clone,"",AT_FDCWD,"/mnt/aur-root",MOVE_MOUNT_F_EMPTY_PATH)<0) { perror("attach root clone"); return 1; }
        close(clone);
        int attached=open("/mnt/aur-root",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(attached<0 || dup2(attached,input)<0) goto error;
        if(attached!=input) close(attached);
        alarm(0);
        execv(argv[4],argv+4); goto error;
    }
    int context=fsopen("tmpfs",FSOPEN_CLOEXEC); if(context<0) goto error;
    snprintf(value,sizeof(value),"%llu",bytes);
    if(fsconfig(context,FSCONFIG_SET_STRING,"size",value,0)<0) goto error;
    snprintf(value,sizeof(value),"%llu",nodes);
    if(fsconfig(context,FSCONFIG_SET_STRING,"nr_inodes",value,0)<0 ||
       fsconfig(context,FSCONFIG_SET_STRING,"mode","0700",0)<0 ||
       fsconfig(context,FSCONFIG_CMD_CREATE,NULL,NULL,0)<0) goto error;
    int root=fsmount(context,FSMOUNT_CLOEXEC,MOUNT_ATTR_NOSUID|MOUNT_ATTR_NODEV);
    if(root<0) goto error;
    char state='S'; struct iovec iov={.iov_base=&state,.iov_len=1};
    union { char bytes[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } control;
    memset(&control,0,sizeof(control));
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *cmsg=CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level=SOL_SOCKET; cmsg->cmsg_type=SCM_RIGHTS; cmsg->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg),&root,sizeof(root));
    if(sendmsg((int)channel,&msg,MSG_NOSIGNAL)!=1) goto error;
    if(recv((int)channel,&state,1,MSG_TRUNC)!=1 || state!='F') return 1;
    /* Freeze the superblock too: a readonly clone must not be thawable into a
     * writable alias of the same bytes in another namespace. The private
     * reconfiguration context is never exported. No writable file remains. */
    if(fsconfig(context,FSCONFIG_SET_FLAG,"ro",NULL,0)<0 ||
       fsconfig(context,FSCONFIG_CMD_RECONFIGURE,NULL,NULL,0)<0) goto error;
    close(context);
    struct mount_attr attr={.attr_set=MOUNT_ATTR_RDONLY|MOUNT_ATTR_NOSUID|MOUNT_ATTR_NODEV};
    if(mount_setattr(root,"",AT_EMPTY_PATH,&attr,sizeof(attr))<0) goto error;
    close(root);
    state='R'; if(send((int)channel,&state,1,MSG_NOSIGNAL)!=1) goto error;
    return 0;
error:
    perror("detached root staging"); return 1;
}
