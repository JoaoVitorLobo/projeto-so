#ifndef UTILS_H
#define UTILS_H

#include <unistd.h>
#include <stddef.h>

ssize_t read_full(int fd, void *buf, size_t size);
ssize_t write_full(int fd, const void *buf, size_t size);
int read_line(int file, char* buffer);

#endif
