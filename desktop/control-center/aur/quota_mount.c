/* Desktop-user helper: private user/mount namespace, bounded detached tmpfs FD.
 * Never root, no arbitrary command, no host-visible mount, no package access.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ROOT_STAGE_PATH
#define ROOT_STAGE_PATH "/usr/lib/zethropol/aur-root-stage"
#endif
static int write_text(const char *path,const char *text) {
    int fd=open(path,O_WRONLY|O_CLOEXEC); if(fd<0) return -1;
    size_t n=strlen(text); ssize_t written=write(fd,text,n); close(fd); return written==(ssize_t)n?0:-1;
}
/* Reject signs, whitespace, overflow and int truncation before any namespace
 * action. Namespace/channel handles must never wrap onto another open FD. */
static int number(const char *s,unsigned long long max,unsigned long long *out) {
    if(!*s) return -1;
    for(const char *p=s;*p;p++) if(*p<'0' || *p>'9') return -1;
    char *end; errno=0; unsigned long long n=strtoull(s,&end,10);
    if(errno || *end || n>max) return -1;
    *out=n; return 0;
}
/* Pin all components without following symlinks, including ancestors. Relative
 * paths remain supported, but dot, dot-dot and empty components are rejected. */
static int directory(const char *path) {
    if(!*path || strlen(path)>4096) return -1;
    int fd=open(*path=='/'?"/":".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return -1;
    char *copy=strdup(path+(*path=='/')); if(!copy) { close(fd); return -1; }
    char *cursor=copy,*part;
    while((part=strsep(&cursor,"/"))) {
        if(!*part || !strcmp(part,".") || !strcmp(part,"..")) { close(fd); free(copy); return -1; }
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd); if(next<0) { free(copy); return -1; } fd=next;
    }
    free(copy); return fd;
}
int main(int argc,char **argv) {
    if(getuid()==0 || geteuid()==0) return 2;
    if(argc>=7 && !strcmp(argv[1],"--enter")) {
        unsigned long long user,mounts;
        if(number(argv[2],1048576,&user)<0 || user<3 ||
           number(argv[3],1048576,&mounts)<0 || mounts<3) return 2;
        if(strcmp(argv[4],"--") || (strcmp(argv[5],"/usr/bin/bwrap") && strcmp(argv[5],ROOT_STAGE_PATH))) return 2;
        if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)<0 || setns((int)user,CLONE_NEWUSER)<0 || setns((int)mounts,CLONE_NEWNS)<0) goto error;
        /* Setup authority must never reach bwrap or recipe descendants. */
        close((int)user); close((int)mounts);
        execv(argv[5],argv+5); goto error;
    }
    if(argc!=5) return 2;
    unsigned long long bytes,nodes,handle;
    if(number(argv[2],1024ULL*1024*1024,&bytes)<0 || bytes<4096 ||
       number(argv[3],65536,&nodes)<0 || nodes<16 ||
       number(argv[4],1048576,&handle)<0 || handle<3) return 2;
    int channel=(int)handle,type=0; socklen_t length=sizeof(type);
    if(getsockopt(channel,SOL_SOCKET,SO_TYPE,&type,&length)<0 || type!=SOCK_SEQPACKET) return 2;
    int target=directory(argv[1]); struct stat info;
    if(target<0 || fstat(target,&info)<0 || info.st_uid!=getuid() || (info.st_mode&077)) return 2;
    uid_t uid=getuid(); gid_t gid=getgid(); char map[128];
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)<0 || unshare(CLONE_NEWUSER)<0) goto error;
    if(write_text("/proc/self/setgroups","deny")<0) goto error;
    snprintf(map,sizeof(map),"%u %u 1",uid,uid);
    if(write_text("/proc/self/uid_map",map)<0) goto error;
    snprintf(map,sizeof(map),"%u %u 1",gid,gid);
    if(write_text("/proc/self/gid_map",map)<0 || unshare(CLONE_NEWNS)<0) goto error;
    if(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)<0) goto error;
    /* Re-pin in the cloned mount namespace (pre-unshare mount FDs belong to
     * the original namespace) and require the exact previously checked inode.
     * No pathname is used again for mount attachment or root FD selection. */
    int pinned=directory(argv[1]); struct stat again;
    if(pinned<0 || fstat(pinned,&again)<0 || again.st_dev!=info.st_dev ||
       again.st_ino!=info.st_ino || again.st_uid!=uid || (again.st_mode&077)) goto error;
    close(target); target=pinned;
    int context=fsopen("tmpfs",FSOPEN_CLOEXEC); if(context<0) goto error;
    char value[64]; snprintf(value,sizeof(value),"%llu",bytes);
    if(fsconfig(context,FSCONFIG_SET_STRING,"size",value,0)<0) goto error;
    snprintf(value,sizeof(value),"%llu",nodes);
    if(fsconfig(context,FSCONFIG_SET_STRING,"nr_inodes",value,0)<0 ||
       fsconfig(context,FSCONFIG_SET_STRING,"mode","0700",0)<0) goto error;
    snprintf(value,sizeof(value),"%u",uid);
    if(fsconfig(context,FSCONFIG_SET_STRING,"uid",value,0)<0) goto error;
    snprintf(value,sizeof(value),"%u",gid);
    if(fsconfig(context,FSCONFIG_SET_STRING,"gid",value,0)<0 ||
       fsconfig(context,FSCONFIG_CMD_CREATE,NULL,NULL,0)<0) goto error;
    int root=fsmount(context,FSMOUNT_CLOEXEC,MOUNT_ATTR_NOSUID|MOUNT_ATTR_NODEV);
    if(root<0) goto error;
    close(context);
    if(move_mount(root,"",target,"",MOVE_MOUNT_F_EMPTY_PATH|MOVE_MOUNT_T_EMPTY_PATH)<0) goto error;
    close(target);
    int handles[3]={root,open("/proc/self/ns/user",O_RDONLY|O_CLOEXEC),open("/proc/self/ns/mnt",O_RDONLY|O_CLOEXEC)};
    if(handles[1]<0 || handles[2]<0) goto error;
    char data='Q'; struct iovec iov={.iov_base=&data,.iov_len=1};
    union { char buffer[CMSG_SPACE(3*sizeof(int))]; struct cmsghdr align; } control;
    memset(&control,0,sizeof(control));
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.buffer,.msg_controllen=sizeof(control.buffer)};
    struct cmsghdr *cmsg=CMSG_FIRSTHDR(&msg); cmsg->cmsg_level=SOL_SOCKET; cmsg->cmsg_type=SCM_RIGHTS; cmsg->cmsg_len=CMSG_LEN(3*sizeof(int));
    memcpy(CMSG_DATA(cmsg),handles,sizeof(handles));
    if(sendmsg(channel,&msg,MSG_NOSIGNAL)!=1) goto error;
    close(root); return 0;
error:
    perror("private quota tmpfs"); return 1;
}
