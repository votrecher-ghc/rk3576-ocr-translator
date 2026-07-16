#include "iio_discovery.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define IIO_SYSFS_ROOT "/sys/bus/iio/devices"

static int is_iio_device_name(const char *name)
{
    static const char prefix[] = "iio:device";
    const char *cursor;

    if (!name || strncmp(name, prefix, sizeof(prefix) - 1) != 0) return 0;
    cursor = name + sizeof(prefix) - 1;
    if (*cursor == '\0') return 0;
    while (*cursor != '\0') {
        if (!isdigit((unsigned char)*cursor)) return 0;
        ++cursor;
    }
    return 1;
}

static int read_iio_name(const char *entry, char *name, size_t name_size)
{
    char path[256];
    int length;
    FILE *file;

    length = snprintf(path, sizeof(path), "%s/%s/name", IIO_SYSFS_ROOT, entry);
    if (length < 0 || (size_t)length >= sizeof(path)) return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    if (!fgets(name, (int)name_size, file)) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) return -1;
    name[strcspn(name, "\r\n")] = '\0';
    return name[0] != '\0' ? 0 : -1;
}

int ocr_iio_find_device(const char *iio_name,
                        char *sysfs_path, size_t sysfs_size,
                        char *dev_path, size_t dev_size)
{
    DIR *directory;
    struct dirent *entry;
    char selected_entry[64] = "";
    int matches = 0;
    int scan_error = 0;

    if (!iio_name || iio_name[0] == '\0' ||
        (!sysfs_path && !dev_path) ||
        (sysfs_path && sysfs_size == 0) ||
        (dev_path && dev_size == 0)) {
        return -1;
    }
    if (sysfs_path) sysfs_path[0] = '\0';
    if (dev_path) dev_path[0] = '\0';

    directory = opendir(IIO_SYSFS_ROOT);
    if (!directory) return -2;

    for (;;) {
        char discovered_name[128];

        errno = 0;
        entry = readdir(directory);
        if (!entry) {
            if (errno != 0) scan_error = 1;
            break;
        }

        if (!is_iio_device_name(entry->d_name) ||
            read_iio_name(entry->d_name, discovered_name,
                          sizeof(discovered_name)) != 0 ||
            strcmp(discovered_name, iio_name) != 0) {
            continue;
        }
        ++matches;
        if (matches == 1) {
            int length = snprintf(selected_entry, sizeof(selected_entry),
                                  "%s", entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(selected_entry)) {
                scan_error = 1;
                break;
            }
        } else {
            break;
        }
    }

    if (closedir(directory) != 0) scan_error = 1;
    if (scan_error) return -2;
    /* An ambiguous name must be resolved explicitly in the runtime config. */
    if (matches == 0) return -4;
    if (matches != 1) return -5;

    if (sysfs_path) {
        int length = snprintf(sysfs_path, sysfs_size, "%s/%s",
                              IIO_SYSFS_ROOT, selected_entry);
        if (length < 0 || (size_t)length >= sysfs_size) goto too_long;
    }
    if (dev_path) {
        int length = snprintf(dev_path, dev_size, "/dev/%s", selected_entry);
        if (length < 0 || (size_t)length >= dev_size) goto too_long;
    }
    return 0;

too_long:
    if (sysfs_path) sysfs_path[0] = '\0';
    if (dev_path) dev_path[0] = '\0';
    return -3;
}
