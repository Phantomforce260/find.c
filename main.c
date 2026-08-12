#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct {
    char** items;
    size_t count;
    size_t capacity;
} StringArray;

static void add_item(StringArray* results, const char* path) {

    if (results->count == results->capacity) {
        size_t new_capacity = results->capacity == 0
            ? 16
            : results->capacity * 2;

        char** new_items = realloc(
            results->items,
            new_capacity * sizeof(char*)
        );

        if (!new_items) {
            puts("Error in reallocation.");
            exit(EXIT_FAILURE);
        }

        results->items = new_items;
        results->capacity = new_capacity;
    }

    results->items[results->count] = strdup(path);

    if (!results->items[results->count]) {
        puts("Error in strdup");
        exit(EXIT_FAILURE);
    }

    results->count++;
}

static void search_recursive(
    const char* path,
    int want_files,
    StringArray* results
) {
    DIR* dir = opendir(path);
    if (!dir)
        return;

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char fullpath[4096];

        snprintf(
            fullpath,
            sizeof(fullpath),
            "%s/%s",
            path,
            entry->d_name
        );

        struct stat st;

        if (lstat(fullpath, &st) != 0)
            continue;

        if (want_files && S_ISREG(st.st_mode)) {
            add_item(results, fullpath);
        } else if (!want_files && S_ISDIR(st.st_mode)) {
            add_item(results, fullpath);
        }

        if (S_ISDIR(st.st_mode)) {
            search_recursive(fullpath, want_files, results);
        }
    }

    closedir(dir);
}

static StringArray search(const char* type) {
    StringArray results = {
        .items = NULL,
        .count = 0,
        .capacity = 0
    };

    int want_files;

    if (strcmp(type, "files") == 0)
        want_files = 1;
    else if (strcmp(type, "folders") == 0)
        want_files = 0;
    else
        return results;

    search_recursive(".", want_files, &results);

    return results;
}

static void free_results(StringArray* results) {
    for (size_t i =0; i < results->count; i++) {
        free(results->items[i]);
    }

    free(results->items);

    results->items = NULL;
    results->count = 0;
    results->capacity = 0;
}

void help() {
    puts(
        "find.c - Find and operate on files with readable syntax.\n\n"
        "SYNOPSIS\n"
        "    find [type]\n"
        "    in [directory]\n"
        "    where [condition]\n"
        "    then [action]\n"
    );
}

int main(int argc, char* argv[]) {

    /* If only binary name passed, display help. */
    if (argc == 1) {
        help();
        return EXIT_SUCCESS;
    }

    /* If only [type] passed, recursive search for all [type]s in cwd */
    if (argc == 2) {

        if (strcmp(argv[1], "files") != 0 && strcmp(argv[1], "folders") != 0) {
            puts("Type must be \"files\" or \"folders\".");
            return EXIT_FAILURE;
        }

        StringArray results = search(argv[1]);

        for (size_t i = 0; i < results.count; i++) {
            printf("%s\n", results.items[i]);
        }

        free_results(&results);

        return EXIT_SUCCESS;
    }
}
