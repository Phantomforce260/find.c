#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "str.h"
#include "bytes.h"

typedef enum {
    STATE_TYPE,
    STATE_PATHS,
    STATE_WHERE,
    STATE_CONDITION,
    STATE_ACTION,
    STATE_DONE,
    STATE_ERROR
} ParserState;

typedef enum {
    TYPE_FILES,
    TYPE_FOLDERS,
} EntryType;

typedef enum {
    CONDITION_NONE,
    CONDITION_CONTAINS,
    CONDITION_STARTSWITH,
    CONDITION_ENDSWITH,
    CONDITION_LESSTHAN,
    CONDITION_GREATERTHAN,
    CONDITION_EXECUTABLE
} ConditionType;

typedef enum {
    ACTION_PRINT,
    ACTION_MOVE,
    ACTION_DELETE,
    ACTION_COPY,
    ACTION_COMMAND
} ActionType;

typedef struct {
    char** items;
    size_t count;
    size_t capacity;
} StringArray;

typedef struct {
    EntryType type;
    StringArray paths;
    ConditionType condition;
    char* value;
    ActionType action;
    char* action_arg;
} FindCommand;

void AddString(StringArray* array, const char* string);
void FreeList(StringArray* array);

StringArray search(FindCommand* command);
void search_recursive(int i, FindCommand* command, StringArray* results);

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
        search_recursive(i, command, &results);

    return results;
}

void search_recursive(int i, FindCommand* command, StringArray* results) {
    const char* path = command->paths.items[i];
    DIR* dir = opendir(path);
    if (!dir)
        return;

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (str_equals(entry->d_name, ".") || str_equals(entry->d_name, ".."))
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

        bool condition_met = false;

        if (
            (command->condition == CONDITION_NONE) ||
            (command->condition == CONDITION_CONTAINS && strstr(entry->d_name, command->value) != NULL) ||
            (command->condition == CONDITION_STARTSWITH && startswith(entry->d_name, command->value)) ||
            (command->condition == CONDITION_ENDSWITH && endswith(entry->d_name, command->value))
        ) {
            if (command->type == TYPE_FILES && S_ISREG(st.st_mode))
                AddString(results, fullpath);
            else if (command->type == TYPE_FOLDERS && S_ISDIR(st.st_mode))
                AddString(results, fullpath);
        }

        if (S_ISDIR(st.st_mode))
            AddString(&command->paths, fullpath);
    }

    closedir(dir);
}

int help(int code) {
    switch (code) {
        case 0:
            puts(
                "find.c - Find and operate on files with readable syntax.\n\n"
                "SYNOPSIS\n"
                "    find [type]\n"
                "    in [directory]\n"
                "    where [condition]\n"
                "    then [action]\n"
            );
            return EXIT_SUCCESS;
        case 1:
            puts("find [type]: [type] must be \"files\" or \"folders\".");
            return EXIT_FAILURE;
        case 2:
            puts("find [type] in [path]: expects a path after \"in\".");
            return EXIT_FAILURE;
        case 3:
            puts("find [type] where [condition]: Incomplete Condition.");
            return EXIT_FAILURE;
        case 4:
            puts("find [type] where [condition]: Unknown Condition");
            return EXIT_FAILURE;
        default:
            return EXIT_FAILURE;
    }
}

int main(int argc, char* argv[]) {

    /* If only binary name passed, display help. */
    if (argc == 1)
        return help(0);

    int i = 1;
    FindCommand command = {0};
    ParserState state = STATE_TYPE;

    while (state != STATE_DONE && state != STATE_ERROR) {
        switch (state) {
            case STATE_TYPE:
                if (str_equals(argv[i], "files"))
                    command.type = TYPE_FILES;
                else if (str_equals(argv[i], "folders"))
                    command.type = TYPE_FOLDERS;
                else
                    return help(1);

                // Increment i to check the next word: find [type] ____
                if (++i >= argc) {
                    AddString(&command.paths, ".");
                    command.action = ACTION_PRINT;
                    command.condition = CONDITION_NONE;
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "in"))
                    state = STATE_PATHS;
                else if (str_equals(argv[i], "where"))
                    state = STATE_CONDITION;
                else if (str_equals(argv[i], "then"))
                    state = STATE_ACTION;
                else
                    return help(0);

                break;
            case STATE_PATHS:
                i++;
                while (i < argc && !str_equals(argv[i], "where") && !str_equals(argv[i], "then")) {

                    if (str_equals(argv[i], "and")) {
                        i++;
                        continue;
                    }

                    AddString(&command.paths, argv[i++]);
                }

                if (command.paths.count == 0)
                    return help(2);
                else if (i >= argc) {
                    command.action = ACTION_PRINT;
                    command.condition = CONDITION_NONE;
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "where"))
                    state = STATE_CONDITION;
                else if (str_equals(argv[i], "then"))
                    state = STATE_ACTION;

                break;
            case STATE_CONDITION:

                if (++i >= argc)
                    return help(3);
                else if (str_equals(argv[i], "name")) {
                    if (++i >= argc)
                        return help(3);
                    else if (str_equals(argv[i], "contains"))
                        command.condition = CONDITION_CONTAINS;
                    else if (str_equals(argv[i], "endswith"))
                        command.condition = CONDITION_ENDSWITH;
                    else if (str_equals(argv[i], "startswith"))
                        command.condition = CONDITION_STARTSWITH;
                    else
                        return help(4);
                }
                else if (str_equals(argv[i], "size")) {
                    if (++i >= argc)
                        return help(3);
                    else if (str_equals(argv[i], "lessthan"))
                        command.condition = CONDITION_LESSTHAN;
                    else if (str_equals(argv[i], "greaterthan"))
                        command.condition = CONDITION_GREATERTHAN;
                    else
                        return help(4);
                }

                if (++i >= argc)
                    return help(3);

                command.value = argv[i];

                if (++i >= argc)
                    state = STATE_DONE;
                else if (str_equals(argv[i], "then"))
                    state = STATE_ACTION;
                else {
                    fprintf(stderr, "Expected \"then\" or end of command.\n");
                    state = STATE_ERROR;
                }

                break;
            case STATE_ACTION:
                if (++i >= argc || strcmp(argv[i], "move") != 0) {
                    fprintf(stderr, "Expected action.\n");
                    state = STATE_ERROR;
                    break;
                }

                if (++i >= argc || strcmp(argv[i], "to") != 0) {
                    fprintf(stderr, "Expected \"to\" after \"move\".\n");
                    state = STATE_ERROR;
                    break;
                }

                if (++i >= argc) {
                    fprintf(stderr, "Expected destination path.\n");
                    state = STATE_ERROR;
                    break;
                }

                //command.has_action = true;
                //command.action = argv[i];

                if (++i != argc) {
                    fprintf(stderr,
                        "Unexpected argument after action: %s\n",
                        argv[i]
                    );

                    state = STATE_ERROR;
                }
                else
                    state = STATE_DONE;

                break;

            case STATE_DONE:
            case STATE_ERROR:
                break;
        }
    }

    StringArray results = search(&command);
    if (command.action == ACTION_PRINT)
        for (size_t j = 0; j < results.count; j++)
            printf("%s\n", results.items[j]);

    FreeList(&results);
    return EXIT_SUCCESS;
}
