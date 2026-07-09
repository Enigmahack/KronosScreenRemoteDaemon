/* delmod <name> - unload a module via delete_module(2) directly, avoiding
 * busybox rmmod (which reads /proc/modules and oopses the Kronos when OA is
 * loaded). Static i386 for the Kronos. */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"usage: delmod <module>\n"); return 2; }
    if(syscall(__NR_delete_module, argv[1], 0)){
        fprintf(stderr,"delete_module(%s): %s\n", argv[1], strerror(errno));
        return 1;
    }
    printf("unloaded %s\n", argv[1]);
    return 0;
}
