#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static inline bool str_equals(const char* str1, const char* str2) {
    return strcmp(str1, str2) == 0;
}

static inline bool startswith(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static inline bool endswith(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > str_len)
        return false;

    return strcmp(str + str_len - suffix_len, suffix) == 0;
}
