#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <linux/limits.h>
#include <ctype.h>
#include <unistd.h>

static inline long long byte_count(const char* path) {
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;

    if (S_ISREG(st.st_mode))
        return (long long)st.st_size;

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir)
            return -1;

        long long total = 0;
        struct dirent* entry;

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char child_path[PATH_MAX];

            snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);

            long long size = byte_count(child_path);

            if (size < 0) {
                closedir(dir);
                return -1;
            }

            total += size;
        }

        closedir(dir);
        return total;
    }

    // Unsupported type (e.g. symbolic link, device, socket)
    return -1;
}

static inline long long parse_bytes(const char* str) {
    char* end;
    double value;
    long long multiplier = 1;

    // Parse numbers
    value = strtod(str, &end);

    // If number could not be found
    if (end == str)
        return -1;

    // Skip whitespace between number and unit
    while (isspace((unsigned char)* end))
        end++;

    char unit[3] = {0};
    int i = 0;

    while (*end && i < 2) {
        unit[i++] = (char)tolower((unsigned char)* end);
        end++;
    }

    // Make sure there is nothing after the unit
    while (isspace((unsigned char)* end))
        end++;

    if (*end != '\0')
        return -1;

    if (strcmp(unit, "b") == 0)
        multiplier = 1;
    else if (strcmp(unit, "kb") == 0)
        multiplier = 1024LL;
    else if (strcmp(unit, "mb") == 0)
        multiplier = 1024LL * 1024;
    else if (strcmp(unit, "gb") == 0)
        multiplier = 1024LL * 1024 * 1024;
    else if (strcmp(unit, "tb") == 0)
        multiplier = 1024LL * 1024 * 1024 * 1024;
    else if (unit[0] == '\0')
        multiplier = 1;
    else
        return -1;  // Unknown unit

    return (long long)(value * multiplier);
}

static inline bool is_executable(const char* path) {
    return access(path, X_OK) == 0;
}

static inline bool has_permission(const char* path, char* permissions) {
    struct stat st;
    char* end;
    long mode;

    mode = strtol(permissions, &end, 8);

    if (*permissions == '\0' || *end != '\0' || mode < 0 || mode > 0777) {
        return false;
    }

    if (stat(path, &st) != 0) {
        return false;
    }

    return (st.st_mode & 0777) == (mode & 0777);
}
