#include <unistd.h>

ssize_t read_full(int fd, void *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, (char*)buf + total, size - total);
        if (n <= 0) return -1;
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

int read_line(int file, char* buffer){
    char c;
    int i = 0;
    while (read(file, &c, 1) > 0 && c != '\n' && c!= '\0') {
        buffer[i++] = c;
    }

    if(i > 0){
        buffer[i] = '\0';
    }
    return i;
}