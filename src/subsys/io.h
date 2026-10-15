#pragma once
#include <cstddef>
#include <sys/stat.h>
#include <poll.h>
#include <cstdint>

// One virtual descriptor boundary for files, stdio, pipes, sockets and epoll.
long emu_read(int fd, void* destination, size_t count);
long emu_write(int fd, const void* source, size_t count);
long emu_close(int fd);
long emu_fstat(int fd, struct stat* destination);
int emu_fcntl(int fd, int command, long argument);
int emu_fsync(int fd);
int emu_fdatasync(int fd);
int emu_ioctl(int fd, unsigned long request, void* argument);

long emu_poll(struct pollfd* fds, uint32_t count, int timeout);
