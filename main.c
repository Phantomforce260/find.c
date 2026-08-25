// ================================================================================================
// Headers
// ================================================================================================

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/limits.h>
#include <ctype.h>
#include <unistd.h>

// ================================================================================================
// Enums
// ================================================================================================

typedef enum {
    TYPE_FILES,
    TYPE_FOLDERS,
    TYPE_ITEMS
} EntryType;

typedef enum {
    CONDITION_NONE,
    CONDITION_NAME_CONTAINS,
    CONDITION_FILE_CONTAINS,
    CONDITION_FOLDER_CONTAINS,

    CONDITION_STARTSWITH,
    CONDITION_ENDSWITH,
    CONDITION_LESSTHAN,
    CONDITION_GREATERTHAN,
    CONDITION_EXECUTABLE,
    CONDITION_PERM_BITS
} ConditionType;

typedef enum {
    CMP_EQ,
    CMP_GT,
    CMP_GTE,
    CMP_LT,
    CMP_LTE,
} CompareOp;

typedef enum {
    COUNT_ITEMS,
    COUNT_FILES,
    COUNT_FOLDERS,
} CountWhat;

typedef enum {
    ACTION_PRINT,
    ACTION_MOVE,
    ACTION_DELETE,
    ACTION_COPY,
    ACTION_COMMAND
} ActionType;

typedef enum {
    STATE_TYPE,
    STATE_PATHS,
    STATE_WHERE,
    STATE_CONDITION,
    STATE_ACTION,
    STATE_DONE,
    STATE_ERROR
} ParserState;

// ================================================================================================
// Structs
// ================================================================================================

typedef struct {
    char** items;
    size_t count;
    size_t capacity;
} StringArray;

typedef struct {
    ConditionType type;
    char* value;
    bool negated;
    CompareOp compare_op;
    long count_target;
    CountWhat count_what;
    bool count_shallow;
} Condition;

typedef struct {
    Condition* items;
    size_t count;
    size_t capacity;
} ConditionArray;

typedef struct {
    EntryType type;
    StringArray paths;
    ConditionArray conditions;
    ActionType action;
    char* action_value;
    StringArray command_template;
    bool recursive;
} FindCommand;

// ================================================================================================
// Function Headers
// ================================================================================================

StringArray search(FindCommand* command);

static bool path_covered(StringArray* paths, const char* path);
static void search_recursive(const char* path, FindCommand* command, StringArray* results);
static int exec_action(FindCommand* command, StringArray* results);

// ================================================================================================
// String Helpers
// ================================================================================================

// str_equals: returns true if str1 == str2.
bool str_equals(const char* str1, const char* str2) {
    return strcmp(str1, str2) == 0;
}

// startswith: returns true if str starts with prefix.
bool startswith(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

// endswith: returns true if str ends with suffix.
bool endswith(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    return (suffix_len <= str_len) && strcmp(str + str_len - suffix_len, suffix) == 0;
}

// ================================================================================================
// Array Helpers
// ================================================================================================

// AddString: Adds a string to a StringArray struct.
// Note: Memory errors terminate the program.
void AddString(StringArray* array, const char* string) {

    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity == 0 ? 16 : array->capacity * 2;
        char** new_items = realloc(array->items, new_capacity * sizeof(char*));

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

// FreeList: Frees a StringArray.
void FreeList(StringArray* array) {
    for (size_t i = 0; i < array->count; i++)
        free(array->items[i]);

    free(array->items);

    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

// AddCondition: Adds a Condition to a ConditionArray struct.
// Note: Memory errors terminate the program.
void AddCondition(ConditionArray* array, Condition cond) {
    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity == 0 ? 16 : array->capacity * 2;
        Condition* new_items = realloc(array->items, new_capacity * sizeof(Condition));

        if (!new_items) {
            puts("Error: Could not expand ConditionArray!");
            exit(EXIT_FAILURE);
        }

        array->items = new_items;
        array->capacity = new_capacity;
    }

    if (cond.value)
        cond.value = strdup(cond.value);

    array->items[array->count] = cond;
    array->count++;
}

void FreeConditions(ConditionArray* array) {
    for (size_t i = 0; i < array->count; i++)
        free(array->items[i].value);

    free(array->items);

    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

// ================================================================================================
// I/O Operations
// ================================================================================================

/* byte_count:
 *     - path: A path to a file or folder as a string.
 *
 * If the path is a file, returns the size of the file.
 * If the path is a folder, recurses through the folder and returns the size of all nested files.
 * Special files are not counted. */
static long long byte_count(const char* path) {
    struct stat st;

    // Check if file exists
    if (stat(path, &st) != 0)
        return -1;

    // If file, return file size
    if (S_ISREG(st.st_mode))
        return (long long)st.st_size;

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir)
            return -1;

        long long total = 0;
        struct dirent* entry;

        // Sum each item in the folder, excluding "." and ".."
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

/* parse_bytes:
 *     - str: A string representing a size in bytes ("1GB", "50MB")
 *
 * Takes a string representing a metric byte count and returns the number of bytes as a long. */
static long long parse_bytes(const char* str) {
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

    if (str_equals(unit, "b"))
        multiplier = 1;
    else if (str_equals(unit, "kb"))
        multiplier = 1024LL;
    else if (str_equals(unit, "mb"))
        multiplier = 1024LL * 1024;
    else if (str_equals(unit, "gb"))
        multiplier = 1024LL * 1024 * 1024;
    else if (str_equals(unit, "tb"))
        multiplier = 1024LL * 1024 * 1024 * 1024;
    else if (unit[0] == '\0')
        multiplier = 1;
    else
        // Unknown unit
        return -1;

    return (long long)(value * multiplier);
}

// is_executable: returns true if a path is executable (+x bit)
static bool is_executable(const char* path) {
    return access(path, X_OK) == 0;
}

/* has_permission:
 *     - path: A string for the location of the file.
 *     - permissions: The octal permission bits to check (0644, 2770, etc.)
 * 
 * returns true if the given file/folder has the EXACT permission bits as the given pattern. */
static bool has_permission(const char* path, char* permissions) {
    struct stat st;
    char* end;
    long mode = strtol(permissions, &end, 8);

    return !(
        // permissions is a valid string
        *permissions == '\0' || 
        // end was initialized properly
        *end != '\0' || 
        // mode is a valid octal
        mode < 0 || mode > 0777 || 
        // file exists
        stat(path, &st) != 0
    ) && 
    // permission bits are equal
    (st.st_mode & 0777) == (mode & 0777);
}

// basename: returns the name of the file/folder with the parent path stripped out.
static const char* basename(const char* path) {
    const char* last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static bool file_contains(const char* path, const char* needle) {
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

    content[fread(content, 1, file_size, f)] = '\0';
    bool found = strstr(content, needle) != NULL;

    free(content);
    fclose(f);
    return found;
}

static bool copy_file(const char* src, const char* dest) {
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

// ================================================================================================
// Search Logic
// ================================================================================================

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

static long count_directory(const char* path, CountWhat what, bool shallow) {
    DIR* dir = opendir(path);
    if (!dir)
        return 0;

    long count = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        if (str_equals(entry->d_name, ".") || str_equals(entry->d_name, ".."))
            continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (lstat(fullpath, &st) != 0)
            continue;

        bool match = false;
        switch (what) {
            case COUNT_ITEMS:
                match = true;
                break;
            case COUNT_FILES:
                match = S_ISREG(st.st_mode);
                break;
            case COUNT_FOLDERS:
                match = S_ISDIR(st.st_mode);
                break;
        }

        if (match)
            count++;

        if (!shallow && S_ISDIR(st.st_mode))
            count += count_directory(fullpath, what, shallow);
    }

    closedir(dir);
    return count;
}

static bool match_count(long actual, CompareOp op, long target) {
    switch (op) {
        case CMP_EQ:
            return actual == target;
        case CMP_GT:
            return actual > target;
        case CMP_GTE:
            return actual >= target;
        case CMP_LT:
            return actual < target;
        case CMP_LTE:
            return actual <= target;
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
                case CONDITION_FOLDER_CONTAINS:
                    met = match_count(
                        count_directory(fullpath, cond.count_what, cond.count_shallow),
                        cond.compare_op,
                        cond.count_target
                    );
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
            else if (command->type == TYPE_ITEMS && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
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
        printf("Delete %zu %s? [y/N] ", results->count,
            command->type == TYPE_FILES ? "files" : command->type == TYPE_FOLDERS ? "folders" : "items");
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
                    results->items[j]
                );
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

// ================================================================================================
// Main Code
// ================================================================================================

int help(int code) {
    switch (code) {
        case 0:
            puts(
                "find.c - Find and operate on files with readable syntax.\n\n"

                "SYNOPSIS\n"
                "    find <type> [shallow] [in <paths>] [where <conditions>] [then <action>]\n\n"

                "TYPES\n"
                "    files       Search for files\n"
                "    folders     Search for directories\n"
                "    items       Search for both files and directories\n\n"

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
            fputs("find: <type> must be \"files\", \"folders\", or \"items\".\n", stderr);
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
        case 9:
            fputs("find: \"contents contains\" is not valid with type \"items\".\n", stderr);
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
                else if (str_equals(argv[i], "items"))
                    command.type = TYPE_ITEMS;
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
                    else if (str_equals(argv[i], "where")) {
                        AddString(&command.paths, ".");
                        state = STATE_CONDITION;
                    }
                    else if (str_equals(argv[i], "then")) {
                        AddString(&command.paths, ".");
                        state = STATE_ACTION;
                    }
                    else
                        return help(0);
                }
                else if (str_equals(argv[i], "in"))
                    state = STATE_PATHS;
                else if (str_equals(argv[i], "where")) {
                    AddString(&command.paths, ".");
                    state = STATE_CONDITION;
                }
                else if (str_equals(argv[i], "then")) {
                    AddString(&command.paths, ".");
                    state = STATE_ACTION;
                }
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
                        else if (!str_equals(argv[i], "contains"))
                            return help(4);

                        if (command.type == TYPE_ITEMS)
                            return help(9);

                        if (command.type == TYPE_FILES)
                            cond_type = CONDITION_FILE_CONTAINS;
                        else {
                            cond_type = CONDITION_FOLDER_CONTAINS;

                            if (++i >= argc)
                                return help(3);

                            CompareOp op = CMP_EQ;
                            const char* tok = argv[i];

                            if (str_equals(tok, ">") || str_equals(tok, ">=") ||
                                str_equals(tok, "<") || str_equals(tok, "<=")) {
                                if (str_equals(tok, ">"))
                                    op = CMP_GT;
                                if (str_equals(tok, ">="))
                                    op = CMP_GTE;
                                if (str_equals(tok, "<"))
                                    op = CMP_LT;
                                if (str_equals(tok, "<="))
                                    op = CMP_LTE;
                                if (++i >= argc)
                                    return help(3);
                                tok = argv[i];
                            }
                            else if (tok[0] == '>' || tok[0] == '<') {
                                if (tok[1] == '=')
                                    op = tok[0] == '>' ? CMP_GTE : CMP_LTE;
                                else
                                    op = tok[0] == '>' ? CMP_GT : CMP_LT;
                                tok += (tok[1] == '=') ? 2 : 1;
                            }

                            char* end;
                            long target = strtol(tok, &end, 10);
                            if (end == tok || target < 0 || *end != '\0')
                                return help(3);

                            if (++i >= argc)
                                return help(3);

                            CountWhat what;
                            if (str_equals(argv[i], "items"))
                                what = COUNT_ITEMS;
                            else if (str_equals(argv[i], "files"))
                                what = COUNT_FILES;
                            else if (str_equals(argv[i], "folders"))
                                what = COUNT_FOLDERS;
                            else
                                return help(4);

                            bool shallow = false;
                            if (i + 1 < argc && str_equals(argv[i + 1], "shallow")) {
                                shallow = true;
                                i++;
                            }

                            cond_value = NULL;
                            contains_keyword = false;

                            AddCondition(&command.conditions, (Condition){
                                .type = CONDITION_FOLDER_CONTAINS,
                                .value = NULL,
                                .negated = negated,
                                .compare_op = op,
                                .count_target = target,
                                .count_what = what,
                                .count_shallow = shallow,
                            });

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
                    }

                    if (contains_keyword) {
                        if (++i >= argc)
                            return help(3);
                        else
                            cond_value = argv[i];
                    }

                    AddCondition(&command.conditions, (Condition){
                        .type = cond_type,
                        .value = (char*)cond_value,
                        .negated = negated,
                    });

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
