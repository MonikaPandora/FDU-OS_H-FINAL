#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char buf[512];
void cat(int fd){
    int len;
    while((len = read(fd, buf, sizeof(buf))) > 0){
        write(STDOUT_FILENO, buf, len);
    }
    if(len < 0){
        printf("cat: failed to read\n");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    // TODO
    int fd;
    if(argc <= 1){
        cat(0);
        exit(0);
    }
    for(int i = 1; i < argc; ++i){
        if ((fd = open(argv[i], 0)) < 0) {
            printf("cat: unable to open %s\n", argv[i]);
            exit(0);
        }
        cat(fd);
        write(STDOUT_FILENO, "\n\n", 2);
        close(fd);
    }
    exit(0);
}