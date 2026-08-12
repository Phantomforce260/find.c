#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "find.h"

void AddString(StringArray* array, const char* string) {

    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity == 0
            ? 16
            : array->capacity * 2;

        char** new_items = realloc(
            array->items,
            new_capacity * sizeof(char*)
        );

        if (!new_items) {
            puts("Error: Could not expand StringArray!");
            exit(EXIT_FAILURE);
        }

        array->items = new_items;
        array->capacity = new_capacity;
    }

    array->items[array->count] = strdup(string);

    if (!array->items[array->count]) {
        puts("Error: Could not duplicate input string!");
        exit(EXIT_FAILURE);
    }

    array->count++;
}

void FreeList(StringArray* array) {
    for (size_t i = 0; i < array->count; i++)
        free(array->items[i]);

    free(array->items);

    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

StringArray search(FindCommand* command) {
    // Create result array
    StringArray results = {
        .items = NULL,
        .count = 0,
        .capacity = 0
    };

    // Start recursive search.
    // If type == files, want_type = 1,
    // else for folders, want_type = 0.
    for (int i = 0; i < command->paths.count; i++)
        search_recursive(
            command->paths.items[i], 
            command->type == TYPE_FILES ? 1 : 0, 
            &results
        );

    return results;
}

void search_recursive(const char* path, int want_files, StringArray* results) {

    DIR* dir = opendir(path);
    if (!dir)
        return;

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
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

        if (want_files && S_ISREG(st.st_mode))
            AddString(results, fullpath);
        else if (!want_files && S_ISDIR(st.st_mode))
            AddString(results, fullpath);

        if (S_ISDIR(st.st_mode))
            search_recursive(fullpath, want_files, results);
    }

    closedir(dir);
}
