#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define BUF_SIZE 1048576

int main(int argc, char **argv) {
    if (argc == 1) {
        return -1;
    }
    char *filename = argv[1];
    int hold = open(filename, O_RDONLY); // hold here so that sshfs doesn't sync
    int rd = open("/dev/urandom", O_RDONLY);
    char buf[BUF_SIZE];
    read(hold, buf, BUF_SIZE);
    for (int it = 0; it < 10; it++) {
        // rewrite random large data
        int fd = open(filename, O_RDWR);
        read(rd, buf, BUF_SIZE);
        write(fd, buf, BUF_SIZE);
        read(fd, buf, BUF_SIZE);
        close(fd);
    }
    close(rd);
    close(hold);
}
