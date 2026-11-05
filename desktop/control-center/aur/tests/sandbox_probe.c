#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(x, msg) do { if(!(x)) { fprintf(stderr,"FAIL: %s errno=%d\n",msg,errno); return 1; } puts(msg); } while(0)
int main(int argc, char **argv) {
    CHECK(argc==4,"probe arguments");
    CHECK(getuid()!=0 && geteuid()!=0, "non-root uid");
    for(int fd=3;fd<4096;fd++) { if(fcntl(fd,F_GETFD)!=-1 || errno!=EBADF) { fputs("inherited FD leaked\n",stderr); return 1; } }
    puts("no inherited host FDs");
    CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1,"no_new_privs inherited");
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3,.pid=0};
    struct __user_cap_data_struct d[2];
    CHECK(syscall(SYS_capget,&h,d)==0 && !(d[0].effective|d[1].effective|d[0].permitted|d[1].permitted),"no effective/permitted capabilities");
    CHECK(setuid(0)==-1,"setuid root refused");
    CHECK(open("/etc/shadow",O_RDONLY)==-1,"host shadow absent");
    CHECK(open(argv[1],O_RDONLY)==-1,"host sentinel absent");
    CHECK(open("/run/dbus/system_bus_socket",O_RDONLY)==-1,"host bus absent");
    CHECK(open("/usr/bin/probe",O_WRONLY)==-1,"build root readonly");
    CHECK(open("/escape",O_WRONLY|O_CREAT,0600)==-1,"root mount readonly");
    CHECK(open("/build/allowed",O_WRONLY|O_CREAT,0600)>=0,"workspace writable");
    CHECK(mount("none","/tmp","tmpfs",0,0)==-1,"mount refused");
    CHECK(unshare(CLONE_NEWUSER)==-1,"nested user namespace refused");
    int s=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);
    struct sockaddr_in remote={.sin_family=AF_INET,.sin_port=htons(443)};
    inet_pton(AF_INET,"192.0.2.1",&remote.sin_addr);
    CHECK(s>=0 && connect(s,(void*)&remote,sizeof(remote))==-1 && errno==ENETUNREACH,"external network unroutable");
    close(s);
    remote.sin_addr.s_addr=htonl(INADDR_LOOPBACK); remote.sin_port=htons(atoi(argv[3]));
    s=socket(AF_INET,SOCK_STREAM,0);
    CHECK(s>=0 && connect(s,(void*)&remote,sizeof(remote))==-1,"host loopback listener unreachable");
    close(s);
    char path[128]; snprintf(path,sizeof(path),"/proc/%s/status",argv[2]);
    CHECK(access(path,F_OK)==-1,"host PID absent");
    CHECK(kill(atoi(argv[2]),0)==-1 && errno==ESRCH,"host PID inaccessible");
    return 0;
}
