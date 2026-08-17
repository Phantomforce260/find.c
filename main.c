#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "str.h"
#include "arr.h"
#include "bytes.h"
#include "files.h"

typedef enum {
    STATE_TYPE,
    STATE_PATHS,
    STATE_WHERE,
    STATE_CONDITION,
    STATE_ACTION,
    STATE_DONE,
    STATE_ERROR
} ParserState;

typedef struct {
    EntryType type;
    StringArray paths;
    ConditionArray conditions;
    ActionType action;
    char* action_value;
    StringArray command_template;
    bool recursive;
} FindCommand;

StringArray search(FindCommand* command);

static bool path_covered(StringArray* paths, const char* path);
static void search_recursive(const char* path, FindCommand* command, StringArray* results);
static int exec_action(FindCommand* command, StringArray* results);

StringArray search(FindCommand* command) {
    StringArray results = {
        .items = NULL,
        .count = 0,
        .capacity = 0
    };

    StringArray searched = {
        .items = NULL,
        .count = 0,
        .capacity = 0
    };

    for (int i = 0; i < command->paths.count; i++) {
        if (!path_covered(&searched, command->paths.items[i])) {
            AddString(&searched, command->paths.items[i]);
            search_recursive(command->paths.items[i], command, &results);
        }
    }

    FreeList(&searched);
    return results;
}

static bool path_covered(StringArray* paths, const char* path) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return false;

    for (size_t i = 0; i < paths->count; i++) {
        char existing[PATH_MAX];
        if (!realpath(paths->items[i], existing))
            continue;

        if (str_equals(existing, resolved))
            return true;

        size_t elen = strlen(existing);
        if (elen > 0 && existing[elen - 1] == '/')
            elen--;

        if (strncmp(resolved, existing, elen) == 0 && resolved[elen] == '/')
            return true;
    }
    return false;
}

static void search_recursive(const char* path, FindCommand* command, StringArray* results) {
    DIR* dir = opendir(path);
    if (!dir)
        return;

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (str_equals(entry->d_name, ".") || str_equals(entry->d_name, ".."))
            continue;

        char fullpath[4096];

        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;

        if (lstat(fullpath, &st) != 0)
            continue;

        bool condition_met = command->conditions.count == 0;

        for (size_t c = 0; c < command->conditions.count; c++) {
            Condition cond = command->conditions.items[c];
            bool met = false;

            switch (cond.type) {
                case CONDITION_NONE:
                    met = true;
                    break;
                case CONDITION_NAME_CONTAINS:
                    met = strstr(entry->d_name, cond.value) != NULL;
                    break;
                case CONDITION_STARTSWITH:
                    met = startswith(entry->d_name, cond.value);
                    break;
                case CONDITION_ENDSWITH:
                    met = endswith(entry->d_name, cond.value);
                    break;
                case CONDITION_GREATERTHAN:
                    met = byte_count(fullpath) > parse_bytes(cond.value);
                    break;
                case CONDITION_LESSTHAN:
                    met = byte_count(fullpath) < parse_bytes(cond.value);
                    break;
                case CONDITION_EXECUTABLE:
                    met = is_executable(fullpath);
                    break;
                case CONDITION_PERM_BITS:
                    met = has_permission(fullpath, cond.value);
                    break;
                case CONDITION_FILE_CONTAINS:
                    met = file_contains(fullpath, cond.value);
                    break;
            }

            if (cond.negated)
                met = !met;

            if (!met) {
                condition_met = false;
                break;
            }

            condition_met = true;
        }

        if (condition_met) {
            if (command->type == TYPE_FILES && S_ISREG(st.st_mode))
                AddString(results, fullpath);
            else if (command->type == TYPE_FOLDERS && S_ISDIR(st.st_mode))
                AddString(results, fullpath);
        }

        if (S_ISDIR(st.st_mode) && command->recursive)
            search_recursive(fullpath, command, results);
    }

    closedir(dir);
}

static int exec_action(FindCommand* command, StringArray* results) {
    if (command->action == ACTION_PRINT || results->count == 0) {
        for (size_t j = 0; j < results->count; j++)
            printf("%s\n", results->items[j]);
        return EXIT_SUCCESS;
    }

    if (command->action == ACTION_DELETE) {
        printf("Delete %zu %s? [y/N] ", results->count, command->type == TYPE_FILES ? "files" : "folders");
        fflush(stdout);

        char line[16];
        if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y')) {
            puts("Aborted.");
            return EXIT_SUCCESS;
        }

        for (size_t j = 0; j < results->count; j++) {
            if (remove(results->items[j]) != 0)
                fprintf(stderr, "Failed to delete: %s\n", results->items[j]);
        }

        return EXIT_SUCCESS;
    }

    if (command->action == ACTION_MOVE || command->action == ACTION_COPY) {
        mkdir(command->action_value, 0755);

        for (size_t j = 0; j < results->count; j++) {
            char dest[4096];
            snprintf(dest, sizeof(dest), "%s/%s", command->action_value, basename(results->items[j]));

            bool ok = command->action == ACTION_MOVE
                ? rename(results->items[j], dest) == 0
                : copy_file(results->items[j], dest);

            if (!ok)
                fprintf(stderr, "Failed to %s: %s\n",
                    command->action == ACTION_MOVE ? "move" : "copy",
                    results->items[j]);
        }

        return EXIT_SUCCESS;
    }

    if (command->action == ACTION_COMMAND) {
        for (size_t j = 0; j < results->count; j++) {
            const char* filepath = results->items[j];

            char** args = malloc((command->command_template.count + 1) * sizeof(char*));
            if (!args) {
                perror("malloc");
                continue;
            }

            for (size_t k = 0; k < command->command_template.count; k++) {
                const char* templ = command->command_template.items[k];
                const char* marker = strstr(templ, "{file}");

                if (!marker)
                    args[k] = strdup(templ);
                else {
                    size_t prefix_len = marker - templ;
                    size_t suffix_len = strlen(marker + 6);
                    size_t file_len = strlen(filepath);
                    char* expanded = malloc(prefix_len + file_len + suffix_len + 1);
                    memcpy(expanded, templ, prefix_len);
                    memcpy(expanded + prefix_len, filepath, file_len);
                    memcpy(expanded + prefix_len + file_len, marker + 6, suffix_len + 1);
                    args[k] = expanded;
                }
            }

            args[command->command_template.count] = NULL;

            pid_t pid = fork();
            if (pid == 0) {
                execvp(args[0], args);
                fprintf(stderr, "Failed to exec: %s\n", args[0]);
                _exit(127);
            }
            else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
            }
            else
                perror("fork");

            for (size_t k = 0; k < command->command_template.count; k++)
                free(args[k]);

            free(args);
        }

        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}

int help(int code) {
    switch (code) {
        case 0:
            puts(
                "find.c - Find and operate on files with readable syntax.\n\n"

                "SYNOPSIS\n"
                "    find <type> [shallow] [in <paths>] [where <conditions>] [then <action>]\n\n"

                "TYPES\n"
                "    files       Search for files\n"
                "    folders     Search for directories\n\n"

                "FLAGS\n"
                "    shallow     Only search the specified directory (no recursion)\n\n"

                "PATHS\n"
                "    in <dir> [and <dir>...]\n"
                "        Directories to search. Defaults to \".\" if omitted.\n\n"

                "CONDITIONS\n"
                "    All conditions can be prefixed with \"not\" to negate them.\n"
                "    Multiple conditions can be joined with \"and\".\n\n"
                "    name contains <str>       Filename contains <str>\n"
                "    name startswith <str>     Filename starts with <str>\n"
                "    name endswith <str>       Filename ends with <str>\n"
                "    contents contains <str>   File contents contain <str>\n"
                "    size greaterthan <size>   File size greater than <size>\n"
                "    size lessthan <size>      File size less than <size>\n"
                "    perms is <octal>          Permission bits match <octal> (e.g. 644)\n"
                "    perms exec                File is executable\n\n"
                "    <size> can be: 100, 10kb, 5mb, 2gb, 1tb\n\n"

                "ACTIONS\n"
                "    moveto <dir>              Move matching files to <dir>\n"
                "    copyto <dir>              Copy matching files to <dir>\n"
                "    delete                    Delete matching files (asks for confirmation)\n"
                "    command <args>            Run command for each match ({file} expands to path)\n\n"

                "EXAMPLES\n"
                "    find files in /home\n"
                "    find files shallow in .\n"
                "    find files where name contains .c and perms is 644\n"
                "    find files where not name endswith .o\n"
                "    find files where name contains .h then copyto backup/\n"
                "    find files in src where contents contains TODO then command grep -n {file}\n"
            );
            return EXIT_SUCCESS;
        case 1:
            fputs("find: <type> must be \"files\" or \"folders\".\n", stderr);
            return EXIT_FAILURE;
        case 2:
            fputs("find: expected a path after \"in\".\n", stderr);
            return EXIT_FAILURE;
        case 3:
            fputs("find: incomplete or missing condition after \"where\".\n", stderr);
            return EXIT_FAILURE;
        case 4:
            fputs("find: unknown condition. Expected: name, size, perms, or contents.\n", stderr);
            return EXIT_FAILURE;
        case 5:
            fputs("find: expected \"then\" or \"and\" after condition.\n", stderr);
            return EXIT_FAILURE;
        case 6:
            fputs("find: expected an action: moveto, copyto, delete, or command.\n", stderr);
            return EXIT_FAILURE;
        case 7:
            fputs("find: incomplete action. moveto/copyto require a destination, command requires arguments.\n", stderr);
            return EXIT_FAILURE;
        case 8:
            fputs("find: Unexpected argument after action.\n", stderr);
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

                command.recursive = true;

                if (++i >= argc) {
                    AddString(&command.paths, ".");
                    command.action = ACTION_PRINT;
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "shallow")) {
                    command.recursive = false;
                    if (++i >= argc) {
                        AddString(&command.paths, ".");
                        command.action = ACTION_PRINT;
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
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "where"))
                    state = STATE_CONDITION;
                else if (str_equals(argv[i], "then"))
                    state = STATE_ACTION;

                break;
            case STATE_CONDITION:
                for (;;) {
                    bool contains_keyword = true;
                    bool negated = false;
                    ConditionType cond_type = CONDITION_NONE;
                    const char* cond_value = NULL;

                    if (++i >= argc)
                        return help(3);

                    if (str_equals(argv[i], "not")) {
                        negated = true;
                        if (++i >= argc)
                            return help(3);
                    }

                    if (str_equals(argv[i], "name")) {
                        if (++i >= argc)
                            return help(3);
                        else if (str_equals(argv[i], "contains"))
                            cond_type = CONDITION_NAME_CONTAINS;
                        else if (str_equals(argv[i], "endswith"))
                            cond_type = CONDITION_ENDSWITH;
                        else if (str_equals(argv[i], "startswith"))
                            cond_type = CONDITION_STARTSWITH;
                        else
                            return help(4);
                    }
                    else if (str_equals(argv[i], "size")) {
                        if (++i >= argc)
                            return help(3);
                        else if (str_equals(argv[i], "lessthan"))
                            cond_type = CONDITION_LESSTHAN;
                        else if (str_equals(argv[i], "greaterthan"))
                            cond_type = CONDITION_GREATERTHAN;
                        else
                            return help(4);
                    }
                    else if (str_equals(argv[i], "perms")) {
                        if (++i >= argc)
                            return help(3);
                        else if (str_equals(argv[i], "exec")) {
                            cond_type = CONDITION_EXECUTABLE;
                            contains_keyword = false;
                        }
                        else if (str_equals(argv[i], "is"))
                            cond_type = CONDITION_PERM_BITS;
                        else
                            return help(4);
                    }
                    else if (str_equals(argv[i], "contents")) {
                        if (++i >= argc)
                            return help(3);
                        else if (str_equals(argv[i], "contains"))
                            cond_type = CONDITION_FILE_CONTAINS;
                        else
                            return help(4);
                    }

                    if (contains_keyword) {
                        if (++i >= argc)
                            return help(3);
                        else
                            cond_value = argv[i];
                    }

                    AddCondition(&command.conditions, cond_type, cond_value, negated);

                    if (++i >= argc) {
                        state = STATE_DONE;
                        break;
                    }
                    else if (str_equals(argv[i], "and"))
                        continue;
                    else if (str_equals(argv[i], "then")) {
                        state = STATE_ACTION;
                        break;
                    }
                    else
                        return help(5);
                }

                break;
            case STATE_ACTION:
                if (++i >= argc)
                    return help(6);
                else if (str_equals(argv[i], "moveto"))
                    command.action = ACTION_MOVE;
                else if (str_equals(argv[i], "copyto"))
                    command.action = ACTION_COPY;
                else if (str_equals(argv[i], "delete")) {
                    command.action = ACTION_DELETE;
                    if (++i != argc)
                        return help(5);
                    state = STATE_DONE;
                    break;
                }
                else if (str_equals(argv[i], "command")) {
                    command.action = ACTION_COMMAND;
                    i++;
                    while (i < argc)
                        AddString(&command.command_template, argv[i++]);
                    if (command.command_template.count == 0)
                        return help(7);
                    state = STATE_DONE;
                    break;
                }
                else
                    return help(6);

                if (++i >= argc)
                    return help(7);
                else
                    command.action_value = argv[i];

                if (++i != argc)
                    return help(8);
                else
                    state = STATE_DONE;

                break;

            case STATE_DONE:
            case STATE_ERROR:
                break;
        }
    }

    StringArray results = search(&command);
    int exit_code = exec_action(&command, &results);

    FreeList(&results);
    FreeConditions(&command.conditions);
    FreeList(&command.command_template);
    return exit_code;
}
