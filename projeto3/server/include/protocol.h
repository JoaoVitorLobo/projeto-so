#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <errno.h>
#include <unistd.h>

#define MAX_PIPE_PATH_LENGTH 40
#define QUEUE_SIZE 64

enum {
  OP_CODE_CONNECT = 1,
  OP_CODE_DISCONNECT = 2,
  OP_CODE_PLAY = 3,
  OP_CODE_BOARD = 4,
};

ssize_t read_full(int fd, void *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, (char*)buf + total, size - total);
        if (n <= 0){ 
            if(errno == EINTR){
                if (total>0){
                    continue;
                }
                else
                    return -1;
            }
            return -1;
        }
        total += n;
    }
    return total;
}

ssize_t write_full(int fd, const void *buf, size_t size) {
    size_t total = 0;
    const char *ptr = buf;

    while (total < size) {
        ssize_t n = write(fd, ptr + total, size - total);
        if (n <= 0) {
            return -1; // erro ou pipe fechado
        }
        total += n;
    }
    return total;
}

#endif
