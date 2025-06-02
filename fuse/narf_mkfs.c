#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

void usage(const char *progname) {
    fprintf(stderr, "Usage: %s <size>[K|M|G] <target.img>\n", progname);
    fprintf(stderr, "Example: %s 16M narf.img\n", progname);
    exit(1);
}

off_t parse_size(const char *arg) {
    char *endptr;
    off_t size = strtoull(arg, &endptr, 10);
    if (size <= 0) return -1;

    switch (*endptr) {
        case 'K': case 'k': return size * 1024;
        case 'M': case 'm': return size * 1024 * 1024;
        case 'G': case 'g': return size * 1024 * 1024 * 1024;
        case '\0':          return size; // assume bytes
        default:            return -1;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3)
        usage(argv[0]);

    off_t size = parse_size(argv[1]);
    if (size <= 0) {
        fprintf(stderr, "Invalid size: %s\n", argv[1]);
        return 1;
    }

    const char *target = argv[2];

    // Check if file already exists
    if (access(target, F_OK) == 0) {
        fprintf(stderr, "Error: file '%s' already exists\n", target);
        return 1;
    }

    // Create and initialize file
    int fd = open(target, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Stretch file to desired size
    if (ftruncate(fd, size) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    // (Optional) Write filesystem header here
    // e.g., write(fd, "NARF", 4);

    printf("Created %s with size %lld bytes\n", target, (long long)size);
    close(fd);
    return 0;
}
