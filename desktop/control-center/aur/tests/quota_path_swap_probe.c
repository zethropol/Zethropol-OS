/* Test-only LD_PRELOAD hook: replace an ancestor after the helper pins its
 * mountpoint, immediately before fsopen. All paths are private /tmp fixtures. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int fsopen(const char *name,unsigned int flags) {
    static int swapped;
    const char *from=getenv("QUOTA_TEST_FROM"), *moved=getenv("QUOTA_TEST_MOVED"), *victim=getenv("QUOTA_TEST_VICTIM");
    if(!swapped && !strcmp(name,"tmpfs") && from && moved && victim) {
        swapped=1;
        if(rename(from,moved)<0 || symlink(victim,from)<0) return -1;
    }
    int (*real_fsopen)(const char *,unsigned int)=dlsym(RTLD_NEXT,"fsopen");
    if(!real_fsopen) { errno=ENOSYS; return -1; }
    return real_fsopen(name,flags);
}
