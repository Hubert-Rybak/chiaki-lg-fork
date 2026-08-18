#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "bt_patch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define PATCH_BYTE_OFFSET 2
#define EXIT_TEMPORARY_UNAVAILABLE 75
#define EXIT_UNSUPPORTED 77

static int read_mapping(int fd, uint8_t *data, size_t size, off_t address)
{
    size_t done = 0;
    while (done < size) {
        ssize_t count = pread(fd, data + done, size - done,
                              address + (off_t)done);
        if (count > 0) {
            done += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}
static int path_is_bluetooth_library(const char *path)
{
    static const char suffix[] = "/libbluetooth.default.so";
    size_t path_len = strlen(path);
    size_t suffix_len = sizeof(suffix) - 1;
    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
}

int main(int argc, char **argv)
{
    int restore = 0;
    if (argc == 3 && strcmp(argv[2], "--restore") == 0)
        restore = 1;
    else if (argc != 2) {
        fprintf(stderr, "usage: %s PID [--restore]\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    errno = 0;
    long pid = strtol(argv[1], &end, 10);
    if (errno || !end || *end || pid <= 0 || pid > INT_MAX) {
        fprintf(stderr, "invalid PID: %s\n", argv[1]);
        return 2;
    }

    char maps_path[64];
    char mem_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%ld/maps", pid);
    snprintf(mem_path, sizeof(mem_path), "/proc/%ld/mem", pid);

    FILE *maps = fopen(maps_path, "r");
    if (!maps) {
        fprintf(stderr, "Bluetooth service PID %ld is unavailable: %s\n",
                pid, strerror(errno));
        return errno == ENOENT ? EXIT_TEMPORARY_UNAVAILABLE : 1;
    }
    int mem = open(mem_path, O_RDWR);
    if (mem < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", mem_path, strerror(errno));
        fclose(maps);
        return errno == ENOENT ? EXIT_TEMPORARY_UNAVAILABLE : 1;
    }

    unsigned long match_address = 0;
    ChiakiBtPatchState match_state = CHIAKI_BT_PATCH_NOT_FOUND;
    size_t total_matches = 0;
    size_t mapped_libraries = 0;
    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long finish = 0;
        unsigned long file_offset = 0;
        char permissions[5] = { 0 };
        char path[512] = { 0 };
        int fields = sscanf(line, "%lx-%lx %4s %lx %*s %*s %511s",
                            &start, &finish, permissions, &file_offset, path);
        (void)file_offset;
        if (fields != 5 || permissions[0] != 'r' ||
            !strchr(permissions, 'x') || !path_is_bluetooth_library(path) ||
            finish <= start)
            continue;

        ++mapped_libraries;
        size_t size = (size_t)(finish - start);
        uint8_t *data = malloc(size);
        if (!data) {
            fprintf(stderr, "Cannot allocate %zu bytes for %s\n", size, path);
            close(mem);
            fclose(maps);
            return 1;
        }
        if (read_mapping(mem, data, size, (off_t)start) != 0) {
            fprintf(stderr, "Cannot read %lx-%lx from PID %ld: %s\n",
                    start, finish, pid, strerror(errno));
            free(data);
            close(mem);
            fclose(maps);
            return 1;
        }

        ChiakiBtPatchMatch match = chiaki_bt_patch_find(data, size);
        if (match.state == CHIAKI_BT_PATCH_AMBIGUOUS) {
            total_matches += match.count;
        } else if (match.count == 1) {
            ++total_matches;
            match_address = start + match.offset + PATCH_BYTE_OFFSET;
            match_state = match.state;
        }
        free(data);
    }
    close(mem);
    fclose(maps);

    if (total_matches != 1) {
        fprintf(stderr,
                "No unique compatible LG Bluetooth signature in PID %ld "
                "(mappings=%zu, matches=%zu); no changes made.\n",
                pid, mapped_libraries, total_matches);
        return EXIT_UNSUPPORTED;
    }

    uint8_t desired = restore ? 3 : 2;
    ChiakiBtPatchState desired_state = restore ? CHIAKI_BT_PATCH_UNPATCHED
                                                : CHIAKI_BT_PATCH_ACTIVE;
    if (match_state == desired_state) {
        printf("LG Bluetooth output-report correction is already %s in PID %ld.\n",
               restore ? "restored" : "active", pid);
        return 0;
    }

    mem = open(mem_path, O_RDWR);
    if (mem < 0) {
        fprintf(stderr, "Cannot reopen %s: %s\n", mem_path, strerror(errno));
        return 1;
    }
    ssize_t written;
    do {
        written = pwrite(mem, &desired, 1, (off_t)match_address);
    } while (written < 0 && errno == EINTR);
    if (written != 1) {
        fprintf(stderr, "Cannot patch PID %ld at 0x%lx: %s\n",
                pid, match_address, written < 0 ? strerror(errno) : "short write");
        close(mem);
        return 1;
    }

    uint8_t verified = 0;
    ssize_t received;
    do {
        received = pread(mem, &verified, 1, (off_t)match_address);
    } while (received < 0 && errno == EINTR);
    close(mem);
    if (received != 1 || verified != desired) {
        fprintf(stderr, "Verification failed at 0x%lx in PID %ld.\n",
                match_address, pid);
        return 1;
    }

    printf("LG Bluetooth HID report type changed %u -> %u in PID %ld "
           "at 0x%lx (memory only).\n",
           restore ? 2u : 3u, (unsigned)desired, pid, match_address);
    return 0;
}
