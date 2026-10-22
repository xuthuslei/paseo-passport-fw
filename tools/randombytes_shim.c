/* Host shim for tweetnacl's randombytes (device uses esp_fill_random).
 * Only needed to satisfy the linker; decrypt path does not consume it. */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

void randombytes(unsigned char *x, unsigned long long xlen)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) exit(2);
    unsigned long long off = 0;
    while (off < xlen) {
        ssize_t r = read(fd, x + off, (size_t)(xlen - off));
        if (r <= 0) exit(2);
        off += (unsigned long long)r;
    }
    close(fd);
}
