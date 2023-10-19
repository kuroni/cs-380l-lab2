#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 1) {
        return -1;
    }
    char *filename = argv[1];
    int fd = open(filename, O_RDONLY);
    char buf[2];
    read(fd, buf, 1);
    close(fd);
}
