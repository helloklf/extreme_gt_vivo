#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
int main(int argc, char **argv) {
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { printf("open failed %d\n", errno); return 1; }
    long r = syscall(SYS_finit_module, fd, "", 0);
    printf("finit_module ret=%ld errno=%d (%s)\n", r, errno, strerror(errno));
    return 0;
}
