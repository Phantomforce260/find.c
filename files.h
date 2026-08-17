#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char* basename(const char* path) {
    const char* last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static inline bool file_contains(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return false;
    }

    char* content = malloc(file_size + 1);
    if (!content) {
        fclose(f);
        return false;
    }

    size_t n = fread(content, 1, file_size, f);
    content[n] = '\0';

    bool found = strstr(content, needle) != NULL;

    free(content);
    fclose(f);
    return found;
}

static inline bool copy_file(const char* src, const char* dest) {
    FILE* in = fopen(src, "rb");
    if (!in)
        return false;

    FILE* out = fopen(dest, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    char buf[8192];
    size_t n;
    bool ok = true;

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }

    fclose(in);
    fclose(out);
    return ok;
}

