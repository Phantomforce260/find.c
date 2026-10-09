// ================================================================================================
// Headers
// ================================================================================================

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
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
#include <time.h>

// ================================================================================================
// Enums
// ================================================================================================

typedef enum { TYPE_FILES, TYPE_FOLDERS, TYPE_ITEMS } EntryType;

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
    CONDITION_PERM_BITS,

    CONDITION_CONTENTS_SIMILAR,
    CONDITION_OLDERTHAN,
    CONDITION_NEWERTHAN
} ConditionType;

typedef enum { CMP_EQ, CMP_GT, CMP_GTE, CMP_LT, CMP_LTE } CompareOp;

typedef enum { LOGIC_AND, LOGIC_OR } LogicOp;

typedef enum { COUNT_ITEMS, COUNT_FILES, COUNT_FOLDERS } CountWhat;

typedef enum {
    ACTION_PRINT,
    ACTION_MOVE,
    ACTION_DELETE,
    ACTION_COPY,
    ACTION_COMMAND
} ActionType;

typedef enum {
    STATE_TYPE,
    STATE_MODIFIER,
    STATE_PATHS,
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
    LogicOp logic_op;
    CompareOp compare_op;
    long count_target;
    CountWhat count_what;
    bool count_shallow;
    double similarity_threshold;
    char* reference_file;
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
    bool include_hidden;
    bool include_visible;
} FindCommand;

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
    const size_t str_len = strlen(str);
    const size_t suffix_len = strlen(suffix);
    return (suffix_len <= str_len) && str_equals(str + str_len - suffix_len, suffix);
}

bool folder_pointer(const char* str) {
    return str_equals(str, ".") || str_equals(str, "..");
}

// basename: returns the name of the file/folder with the parent path stripped out.
const char* basename(const char* path) {
    const char* last = strrchr(path, '/');
    return last ? last + 1 : path;
}

/* join_path:
 *     - dir: The parent directory.
 *     - name: The entry name to append to the directory.
 *
 * Joins a directory and an entry name with exactly one "/", so a directory that
 * was given with a trailing slash does not produce a doubled separator.
 * Returns a malloc'd string of exactly the necessary size that the caller must
 * free, or NULL on allocation failure (or size overflow). */
char* join_path(const char* dir, const char* name) {
    size_t len = strlen(dir);

    // Keep the root "/" intact, but drop any other trailing separators.
    while (len > 1 && dir[len - 1] == '/') len--;

    // The root already ends in a separator, so do not add a second one.
    size_t seplen = (len == 1 && dir[0] == '/') ? 0 : 1;
    size_t namelen = strlen(name);

    if (len > SIZE_MAX - seplen - namelen - 1) return NULL;
    char* dest = malloc(len + seplen + namelen + 1);
    if (!dest) return NULL;

    memcpy(dest, dir, len);
    if (seplen) dest[len] = '/';
    memcpy(dest + len + seplen, name, namelen + 1);
    return dest;
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

    if (cond.reference_file)
        cond.reference_file = strdup(cond.reference_file);

    array->items[array->count] = cond;
    array->count++;
}

// FreeList: Frees a ConditionArray.
void FreeConditions(ConditionArray* array) {
    for (size_t i = 0; i < array->count; i++) {
        free(array->items[i].value);
        free(array->items[i].reference_file);
    }

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
    if (lstat(path, &st) != 0) return -1;

    // If file, return file size
    if (S_ISREG(st.st_mode)) return (long long)st.st_size;

    // If directory, open and search contents
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return -1;

        long long total = 0;
        struct dirent* entry;

        // Sum each item in the folder, excluding "." and ".."
        while ((entry = readdir(dir)) != NULL) {
            if (folder_pointer(entry->d_name)) continue;

            char* child_path = join_path(path, entry->d_name);
            if (!child_path) {
                closedir(dir);
                return -1;
            }

            // Recursively search each item
            long long size = byte_count(child_path);
            free(child_path);

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
    if (end == str) return -1;

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
    while (isspace((unsigned char)* end)) end++;

    if (*end != '\0') return -1;

    if (str_equals(unit, "b") || unit[0] == '\0')
        multiplier = 1;
    else if (str_equals(unit, "kb"))
        multiplier = 1024LL;
    else if (str_equals(unit, "mb"))
        multiplier = 1024LL * 1024;
    else if (str_equals(unit, "gb"))
        multiplier = 1024LL * 1024 * 1024;
    else if (str_equals(unit, "tb"))
        multiplier = 1024LL * 1024 * 1024 * 1024;
    // Unknown unit
    else return -1;

    return (long long)(value * multiplier);
}

// is_executable: returns true if a path is executable (+x bit)
static bool is_executable(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 0111) != 0;
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
        mode < 0 || mode > 07777 || 
        // file exists
        stat(path, &st) != 0
    ) && 
    // permission bits are equal
    (st.st_mode & 07777) == (mode & 07777);
}

/* memsearch:
 *     - haystack: The buffer to search in (may contain NUL bytes).
 *     - haystack_len: Number of valid bytes in haystack.
 *     - needle: The byte string to search for (may contain NUL bytes).
 *     - needle_len: Number of bytes in needle.
 *
 * Byte-wise substring search (strstr stops at the first NUL, so it misses
 * matches past embedded NULs in binary files). An empty needle matches. */
static bool memsearch(const char* haystack, size_t haystack_len, const char* needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (haystack_len < needle_len) return false;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

/* file_contains:
 *     - path: The path to the file to evaluate.
 *     - needle: The string to check inside the file.
 *
 * Returns true if the file at the given path exists
 * and contains the exact substring of the needle. */
static bool file_contains(const char* path, const char* needle) {
    // Only regular files have searchable contents (skip dirs, symlinks, etc.).
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Reject empty files.
    if (file_size < 0) {
        fclose(f);
        return false;
    }

    // Allocate space to read content.
    char* content = malloc(file_size + 1);
    if (!content) {
        fclose(f);
        return false;
    }

    // Search the bytes actually read (buffer may contain NULs).
    size_t nread = fread(content, 1, file_size, f);
    bool found = memsearch(content, nread, needle, strlen(needle));

    free(content);
    fclose(f);
    return found;
}

/* parse_similarity:
 *     - str: A string representing a similarity value ("98", "98%", "0.98", "98.5%").
 *
 * Parses a similarity value and normalizes percentages into a [0,1] ratio.
 * Returns the ratio, or -1.0 if the input is invalid. */
static double parse_similarity(const char* str) {
    char* end;
    double value = strtod(str, &end);

    if (end == str) return -1.0;

    // Allow an optional trailing '%'
    if (*end == '%') end++;

    if (*end != '\0') return -1.0;

    // Values above 1 are treated as percentages.
    return ((value > 1.0 ? (value /= 100.0) : value), (value > 0.0 && value <= 1.0 ? value : -1.0));
}

/* read_file_bytes:
 *     - path: The path to the file to read.
 *     - out_size: Output parameter set to the number of bytes read.
 *
 * Reads an entire file into a malloc'd buffer. Returns NULL on failure. */
static unsigned char* read_file_bytes(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return NULL;
    }

    unsigned char* buf = malloc(file_size);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, file_size, f);
    fclose(f);

    *out_size = nread;
    return buf;
}

/* file_similarity:
 *     - path1: Path to the first file.
 *     - path2: Path to the second file.
 *
 * Compares two files byte-by-byte and returns a similarity ratio in [0.0, 1.0].
 * Files of different sizes are compared up to the shorter length; extra bytes
 * in the longer file count as mismatches. Returns -1.0 on read error. */
static double file_similarity(const char* path1, const char* path2) {
    size_t size1, size2;
    unsigned char* buf1 = read_file_bytes(path1, &size1);
    unsigned char* buf2 = read_file_bytes(path2, &size2);

    if (!buf1 || !buf2) {
        free(buf1);
        free(buf2);
        return -1.0;
    }

    size_t min_size = size1 < size2 ? size1 : size2;
    size_t max_size = size1 > size2 ? size1 : size2;

    if (max_size == 0) {
        free(buf1);
        free(buf2);
        return 1.0;
    }

    size_t matching = 0;
    for (size_t i = 0; i < min_size; i++) {
        if (buf1[i] == buf2[i]) matching++;
    }
    free(buf1);
    free(buf2);
    return (double)matching / (double)max_size;
}

/* copy_file_data:
 *     - src: The source file to copy.
 *     - dest: The destination to copy to.
 *     - st: stat of src (from lstat, used for mode + timestamps).
 *
 * Copies regular file contents from src to dest, preserving mode and
 * atime/mtime. Returns true on success. */
static bool copy_file_data(const char* src, const char* dest, const struct stat* st) {
    // Open src in read mode
    FILE* in = fopen(src, "rb");
    if (!in) return false;

    // Open dest in write mode. Creates the file if it does not exist.
    FILE* out = fopen(dest, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    char buf[8192];
    size_t n;
    bool ok = true;
    // Copy the contents of src to dest.
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }

    if (ferror(in)) ok = false;
    if (ok && fchmod(fileno(out), st->st_mode & 07777) != 0) ok = false;
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    // Never leave a truncated partial behind masquerading as a good copy.
    if (!ok) {
        unlink(dest);
        return false;
    }

    struct timespec times[] = {
        st->st_atim,
        st->st_mtim
    };
    return utimensat(AT_FDCWD, dest, times, 0) == 0;
}


// returns true if src and dest are the same file.
static bool is_same_file(const char* src, const char* dest) {
    struct stat src_st, dest_st;
    if (stat(src, &src_st) != 0 || stat(dest, &dest_st) != 0) return false;
    return src_st.st_dev == dest_st.st_dev && src_st.st_ino == dest_st.st_ino;
}

// returns true if dest is inside src.
static bool dest_inside_src(const char* src, const char* dest) {
    struct stat src_st;
    if (lstat(src, &src_st) != 0) return false;
    if (!S_ISDIR(src_st.st_mode) || S_ISLNK(src_st.st_mode)) return false;

    // Resolve src so prefix checks are canonical (no trailing slash, no symlinks).
    char rsrc[PATH_MAX];
    const char* s = realpath(src, rsrc) ? (const char*)rsrc : src;

    char rdst[PATH_MAX];
    char rpar[PATH_MAX];
    char dlex[PATH_MAX];
    const char* d = dest;
    struct stat dst_st;
    if (lstat(dest, &dst_st) == 0) {
        if (S_ISLNK(dst_st.st_mode)) return false;
        const char* r = realpath(dest, rdst);
        if (r) d = r;
    } else {
        const char* slash = strrchr(dest, '/');
        if (slash && slash != dest) {
            const size_t plen = (size_t)(slash - dest);
            if (plen >= sizeof(rpar)) return true;
            memcpy(rpar, dest, plen);
            rpar[plen] = '\0';
            const char* rp = realpath(rpar, rdst) ? (const char*)rdst : NULL;
            if (rp) {
                const size_t rlen = strlen(rp);
                const size_t rest = strlen(slash);
                if (rlen + rest >= sizeof(dlex)) return true;
                memcpy(dlex, rp, rlen);
                memcpy(dlex + rlen, slash, rest + 1);
                d = dlex;
            }
        }
    }

    // Strip trailing slashes for comparison (keep root "/" intact).
    size_t slen = strlen(s);
    while (slen > 1 && s[slen - 1] == '/') slen--;
    size_t dlen = strlen(d);
    while (dlen > 1 && d[dlen - 1] == '/') dlen--;

    if (dlen < slen) return false;
    if (strncmp(s, d, slen) != 0) return false;
    return dlen == slen || d[slen] == '/';
}

/* ensure_dest_dir:
 *     - dest: The destination directory path.
 *     - mode: The mkdir mode (typically src mode & 0777).
 *
 * Creates dest, tolerating EEXIST if dest is already a directory.
 * Returns true on success. */
static bool ensure_dest_dir(const char* dest, mode_t mode) {
    if (mkdir(dest, mode) != 0) {
        // Ignore error if dest folder exists
        if (errno != EEXIST) return false;
        struct stat dest_st;
        if (lstat(dest, &dest_st) != 0 || !S_ISDIR(dest_st.st_mode)) return false;
    }
    return true;
}

/* preserve_metadata:
 *     - dest: The destination file/directory.
 *     - st: stat of src (from lstat, used for mode + timestamps).
 *
 * Applies src mode and atime/mtime to dest. Returns true on success. */
static bool preserve_metadata(const char* dest, const struct stat* st) {
    if (chmod(dest, st->st_mode & 07777) != 0) return false;

    struct timespec times[] = { st->st_atim, st->st_mtim };
    return utimensat(AT_FDCWD, dest, times, 0) == 0;
}

/* copy_dir_children:
 *     - src: The source directory.
 *     - dest: The destination directory (already created).
 *     - fn: Per-child function, e.g. copy() or move().
 *
 * Iterates src entries (skipping "." and "..") and applies fn() to each
 * src/dest child pair. Owns opendir/closedir. Returns true if all
 * children succeeded. */
static bool copy_dir_children(const char* src, const char* dest, bool (*fn)(const char*, const char*)) {
    DIR* dir = opendir(src);
    if (!dir) return false;

    struct dirent* entry;
    const size_t src_len = strlen(src);
    const size_t dest_len = strlen(dest);
    while ((entry = readdir(dir)) != NULL) {
        if (folder_pointer(entry->d_name)) continue;

        const size_t name_len = strlen(entry->d_name);

        char* child_src = malloc(src_len + 1 + name_len + 1);
        char* child_dest = malloc(dest_len + 1 + name_len + 1);

        if (!child_src || !child_dest) {
            free(child_src);
            free(child_dest);
            closedir(dir);
            return false;
        }

        snprintf(
            child_src,
            src_len + 1 + name_len + 1,
            "%s/%s",
            src,
            entry->d_name
        );

        snprintf(
            child_dest,
            dest_len + 1 + name_len + 1,
            "%s/%s",
            dest,
            entry->d_name
        );

        bool ok = fn(child_src, child_dest);
        free(child_src);
        free(child_dest);

        if (!ok) {
            closedir(dir);
            return false;
        }
    }
    return closedir(dir) == 0;
}

/* copy_symlink:
 *     - src: The location of the original symlink.
 *     - dest: The destination to copy the symlink to.
 *
 * Creates a new symlink in dest, that points to the 
 * same location as src.*/
static bool copy_symlink(const char* src, const char* dest) {
    const size_t bufsize = 8192;
    char* symlink_buf = malloc(bufsize);
    if (symlink_buf == NULL) return false;

    ssize_t target_size = readlink(src, symlink_buf, bufsize - 1);
    if (target_size < 0) {
        free(symlink_buf);
        return false;
    }

    symlink_buf[target_size] = '\0';
    bool ok = symlink(symlink_buf, dest) == 0;
    free(symlink_buf);
    return ok;
}

/* copy:
 *     - src: The source file to copy.
 *     - dest: The destination to copy to.
 *
 * Copies a file from src to dest. Assumes that src and dest are absolute paths. */
static bool copy(const char* src, const char* dest) {
    struct stat st;
    return lstat(src, &st) == 0 && !dest_inside_src(src, dest) && !is_same_file(src, dest) && (
        (S_ISREG(st.st_mode) && copy_file_data(src, dest, &st)) ||
        (S_ISDIR(st.st_mode) && 
            ensure_dest_dir(dest, st.st_mode & 0777) && 
            copy_dir_children(src, dest, copy) &&
            preserve_metadata(dest, &st)
        ) ||
        (S_ISLNK(st.st_mode) && copy_symlink(src, dest))
    );
}

/* move:
 *     - src: The location to move files from.
 *     - dest: The location to move files to.
 *
 *  Cuts and pastes files from src to dest recursively. */
static bool move(const char* src, const char* dest) {
    struct stat st;
    return lstat(src, &st) == 0 && !dest_inside_src(src, dest) && !is_same_file(src, dest) && (
        // rename is faster, but only works on same filesystem
        rename(src, dest) == 0 || (
            // Errors other than EXDEV fail the function
            errno != EXDEV && (
                (S_ISREG(st.st_mode) && copy_file_data(src, dest, &st) && unlink(src) == 0) ||
                (S_ISDIR(st.st_mode) &&
                    ensure_dest_dir(dest, st.st_mode & 0777) &&
                    copy_dir_children(src, dest, move) &&
                    // Best-effort: a timestamp/perm hiccup must not orphan an
                    // emptied source dir next to a complete dest tree.
                    (preserve_metadata(dest, &st) || true) &&
                    rmdir(src) == 0
                ) ||
                (S_ISLNK(st.st_mode) && copy_symlink(src, dest) && (unlink(src) == 0))
            )
        )
    );
}

static bool delete(const char* src) {
    struct stat st;
    if (lstat(src, &st) != 0) return false;

    if (!S_ISDIR(st.st_mode))
        return unlink(src) == 0;

    DIR* dir = opendir(src);
    if (dir == NULL) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (folder_pointer(entry->d_name)) continue;

        const size_t src_len = strlen(src);
        const size_t name_len = strlen(entry->d_name);

        char* path = malloc(src_len + 1 + name_len + 1);
        if (path == NULL) {
            closedir(dir);
            return false;
        }

        memcpy(path, src, src_len);
        path[src_len] = '/';
        memcpy(path + src_len + 1, entry->d_name, name_len + 1);

        if (!delete(path)) {
            free(path);
            closedir(dir);
            return false;
        }
        free(path);
    }
    if (closedir(dir) != 0) return false;
    return rmdir(src) == 0;
}

// ================================================================================================
// Time Functions
// ================================================================================================

/* parse_date:
 *     - str: The date to parse.
 *
 * Converts a date string to a time_t reference point.
 * Supports relative durations like "40s", "6m", "8h", "1D", "4W", "3M", "2Y"
 * (measured backward from now), or an absolute date like "MM.DD.YY".
 * The returned time represents the cutoff: files modified after it are newer,
 * files modified before it are older. */
static time_t parse_date(const char* str) {
    // Absolute date "MM.DD.YY"
    if (strchr(str, '.') != NULL) {
        int mm = 0, dd = 0, yy = 0;

        if (
            // MM.DD.YY could not be parsed properly
            sscanf(str, "%d.%d.%d", &mm, &dd, &yy) != 3 ||
            // Times are not within valid ranges
            (mm < 1 || mm > 12 || dd < 1 || dd > 31 || yy < 0 || yy > 99)
        ) return (time_t)-1;

        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = (yy >= 70 ? yy : 100 + yy); // 70-99 -> 1970-1999, 00-69 -> 2000-2069
        tm.tm_mon = mm - 1;
        tm.tm_mday = dd;
        tm.tm_isdst = -1;
        return mktime(&tm);
    }

    // Relative duration like "40s", "6m", "8h", "1D", "4W", "3M", "2Y"
    char* end;
    double value = strtod(str, &end);
    if (end == str) return (time_t)-1;

    while (isspace((unsigned char)*end))
        end++;

    char unit = *end;
    if (unit == '\0') return (time_t)-1;

    // Accept both the documented cases and their lowercase/uppercase variants
    // (e.g. "S", "H", "d", "y"), but flag exact-case mismatches so the caller
    // can alert the user. The canonical forms are: s m h D W M Y.
    char canonical;
    switch (unit) {
        case 's': case 'S': canonical = 's'; break;
        case 'm': canonical = 'm'; break;
        case 'h': case 'H': canonical = 'h'; break;
        case 'd': case 'D': canonical = 'D'; break;
        case 'w': case 'W': canonical = 'W'; break;
        case 'M': canonical = 'M'; break;
        case 'y': case 'Y': canonical = 'Y'; break;
        default: return (time_t)-1;
    }

    // wrong-case unit
    if (unit != canonical) return (time_t)-2;

    time_t now = time(NULL);
    time_t multiplier;
    switch (unit) {
        case 's': multiplier = 1;                  break; // seconds
        case 'm': multiplier = 60;                 break; // minutes
        case 'h': multiplier = 60 * 60;            break; // hours
        case 'D': multiplier = 24 * 60 * 60;       break; // days
        case 'W': multiplier = 7 * 24 * 60 * 60;   break; // weeks
        case 'M': multiplier = 30 * 24 * 60 * 60;  break; // months (approx 30 days)
        case 'Y': multiplier = 365 * 24 * 60 * 60; break; // years (approx 365 days)
        default: return (time_t)-1;
    }

    do end++; 
    while (isspace((unsigned char)*end));

    if (*end != '\0') return (time_t)-1;
    return now - (time_t)(value * multiplier);
}

/* expand_file_markers:
 *     - templ: A command template token, possibly containing "{file}".
 *     - filepath: The matched path substituted for every marker.
 *
 * Expands ALL "{file}" occurrences (not just the first) in a single pass.
 * Returns a malloc'd string the caller must free, or NULL on failure. */
static char* expand_file_markers(const char* templ, const char* filepath) {
    static const char marker[] = "{file}";
    static const size_t mlen = sizeof(marker) - 1;

    size_t count = 0;
    for (const char* p = templ; (p = strstr(p, marker)) != NULL; p += mlen)
        count++;

    if (count == 0) return strdup(templ);

    size_t tlen = strlen(templ);
    size_t flen = strlen(filepath);
    // Expanded size: tlen - count*mlen + count*flen + 1. Guard overflow.
    size_t total;
    if (flen >= mlen) {
        size_t extra = flen - mlen;
        if (count > 0 && extra > (SIZE_MAX - tlen - 1) / count) return NULL;
        total = tlen + count * extra + 1;
    } else {
        total = tlen - count * (mlen - flen) + 1;
    }

    char* out = malloc(total);
    if (!out) return NULL;

    const char* src = templ;
    char* dst = out;
    const char* hit;
    while ((hit = strstr(src, marker)) != NULL) {
        size_t chunk = (size_t)(hit - src);
        memcpy(dst, src, chunk);
        dst += chunk;
        memcpy(dst, filepath, flen);
        dst += flen;
        src = hit + mlen;
    }
    strcpy(dst, src);
    return out;
}

// ================================================================================================
// Search Logic
// ================================================================================================

/* dedupe_results:
 *     - results: The list of results to filter.
 *
 * Removes duplicate result entries by canonical identity (realpath when the
 * path still resolves, raw string otherwise), preserving first-seen order.
 * This catches overlaps path_covered cannot skip: a parent searched after its
 * child, or the same tree spelled differently ("./src", trailing slashes). */
static void dedupe_results(StringArray* results) {
    if (results->count < 2) return;

    char** canon = malloc(results->count * sizeof(char*));
    if (!canon) return; // OOM: keep duplicates rather than fail the search
    for (size_t i = 0; i < results->count; i++) {
        char tmp[PATH_MAX];
        const char* src = realpath(results->items[i], tmp) ? tmp : results->items[i];
        canon[i] = strdup(src);
        if (!canon[i]) {
            for (size_t m = 0; m < i; m++) free(canon[m]);
            free(canon);
            return;
        }
    }

    size_t kept = 0;
    for (size_t i = 0; i < results->count; i++) {
        bool dup = false;
        for (size_t k = 0; k < kept; k++) {
            if (str_equals(canon[k], canon[i])) {
                dup = true;
                break; 
            }
        }
        if (dup) {
            free(results->items[i]);
            free(canon[i]);
        } else {
            results->items[kept] = results->items[i];
            canon[kept] = canon[i];
            kept++;
        }
    }
    for (size_t k = 0; k < kept; k++) free(canon[k]);
    free(canon);
    results->count = kept;
}

/* path_covered:
 *     - paths: The total list of searched paths.
 *     - path: The path to check.
 *
 * Returns true if the given path has already been reached in the given StringArray. */
static bool path_covered(StringArray* paths, const char* path) {

    // Check if the input path exists
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return false;

    for (size_t i = 0; i < paths->count; i++) {
        // Skip unreachable paths in the array.
        char existing[PATH_MAX];
        if (!realpath(paths->items[i], existing)) continue;

        // Is the current path == our input path?
        if (str_equals(existing, resolved)) return true;

        // If not, strip any trailing "/" and strip the absolute name. Are the paths the same then?
        size_t elen = strlen(existing);
        if (elen > 0 && existing[elen - 1] == '/')
            elen--;

        if (strncmp(resolved, existing, elen) == 0 && resolved[elen] == '/') return true;
    }
    return false;
}

/* cmp_path_len_lex:
 *     - a: Pointer to the first path entry (as const char* const* for qsort).
 *     - b: Pointer to the second path entry (as const char* const* for qsort).
 *
 * Orders result paths parents-first (shortest first, lexicographic tiebreak)
 * so exec_action handles a directory before its descendants. Deterministic. */
static int cmp_path_len_lex(const void* a, const void* b) {
    const char* const* pa = (const char* const*)a;
    const char* const* pb = (const char* const*)b;
    const size_t la = strlen(*pa);
    const size_t lb = strlen(*pb);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(*pa, *pb);
}

/* path_within_lex:
 *     - parent: The candidate ancestor path (trailing slashes ignored).
 *     - child: The path to test (trailing slashes ignored).
 *
 * Lexical version of the path_covered prefix check for paths that may no
 * longer exist (already moved/deleted), where realpath() would fail.
 * Returns true if child equals parent or lives under parent + "/". */
static bool path_within_lex(const char* parent, const char* child) {
    size_t plen = strlen(parent);
    while (plen > 1 && parent[plen - 1] == '/') plen--;
    size_t clen = strlen(child);
    while (clen > 1 && child[clen - 1] == '/') clen--;
    if (clen < plen) return false;
    if (strncmp(parent, child, plen) != 0) return false;
    return clen == plen || child[plen] == '/';
}

/* count_directory:
 *     - path: The directory to check for files.
 *     - what: Whether to check files, folders, or both.
 *     - shallow: If true, do not use recursion.
 *
 * Counts the number of items in the given directory with the given parameters. */
static long count_directory(const char* path, CountWhat what, bool shallow, bool include_hidden, bool include_visible) {
    // Open folder and read contents
    DIR* dir = opendir(path);
    if (!dir) return 0;

    long count = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".."
        if (folder_pointer(entry->d_name)) continue;

        // Filter by visibility.
        bool is_hidden = entry->d_name[0] == '.';
        if ((include_hidden && !is_hidden) || (include_visible && is_hidden)) continue;

        char* fullpath = join_path(path, entry->d_name);
        if (!fullpath) continue;

        struct stat st;
        if (lstat(fullpath, &st) != 0) { free(fullpath); continue; }

        // Evaluate the match based on the CountWhat value.
        bool match = false;
        switch (what) {
            case COUNT_ITEMS:   match = true;                break;
            case COUNT_FILES:   match = S_ISREG(st.st_mode); break;
            case COUNT_FOLDERS: match = S_ISDIR(st.st_mode); break;
        }

        // If our match is successful, increment our count.
        if (match) count++;

        // If shallow search is disabled and the item is a directory, recursive search.
        if (!shallow && S_ISDIR(st.st_mode))
            count += count_directory(fullpath, what, shallow, include_hidden, include_visible);
        free(fullpath);
    }
    closedir(dir);
    return count;
}

// Returns true if actual {op} target.
static bool match_count(long actual, CompareOp op, long target) {
    switch (op) {
        case CMP_EQ:  return actual == target;
        case CMP_GT:  return actual > target;
        case CMP_GTE: return actual >= target;
        case CMP_LT:  return actual < target;
        case CMP_LTE: return actual <= target;
        default:      return false;
    }
}

/* eval_condition:
 *     - fullpath: The full path of the item being evaluated.
 *     - name: The entry name of the item being evaluated.
 *     - cond: The condition to evaluate.
 *
 * Evaluates a single condition against an item, applying its negation if set. */
static bool eval_condition(const char* fullpath, const char* name, const Condition* cond, bool include_hidden, bool include_visible) {
    bool met = false;

    switch (cond->type) {
        case CONDITION_NONE:          met = true; break;
        case CONDITION_NAME_CONTAINS: met = strstr(name, cond->value) != NULL; break;
        case CONDITION_STARTSWITH:    met = startswith(name, cond->value);     break;
        case CONDITION_ENDSWITH:      met = endswith(name, cond->value);       break;
        case CONDITION_OLDERTHAN: case CONDITION_NEWERTHAN: {
            struct stat st;
            time_t cutoff = parse_date(cond->value);
            if (cutoff == (time_t)-1 || stat(fullpath, &st) != 0) {
                met = false;
                break;
            }
            // olderthan: mtime before the cutoff; newerthan: mtime after the cutoff
            met = (cond->type == CONDITION_OLDERTHAN) ? st.st_mtime < cutoff : st.st_mtime > cutoff; break;
        }
        case CONDITION_GREATERTHAN: case CONDITION_LESSTHAN: {
            long long nbytes = byte_count(fullpath);
            long long parsed = parse_bytes(cond->value);
            met = nbytes >= 0 && parsed >= 0 &&
                (cond->type == CONDITION_GREATERTHAN ? nbytes > parsed : nbytes < parsed); break;
        }
        case CONDITION_EXECUTABLE:    met = is_executable(fullpath);               break;
        case CONDITION_PERM_BITS:     met = has_permission(fullpath, cond->value); break;
        case CONDITION_FILE_CONTAINS: met = file_contains(fullpath, cond->value);  break;
        case CONDITION_FOLDER_CONTAINS: {
            met = match_count(
                count_directory(fullpath, cond->count_what, cond->count_shallow, include_hidden, include_visible),
                cond->compare_op,
                cond->count_target
            );
            break;
        }
        case CONDITION_CONTENTS_SIMILAR: {
            const double similarity = file_similarity(fullpath, cond->reference_file);
            met = similarity >= 0.0 && similarity >= (cond->similarity_threshold);
            break;
        }
    }

    // Return the opposite if cond->negated is true
    return met ^ cond->negated;
}

/* search_recursive:
 *     - path: The starting path to search.
 *     - command: The parameters/conditions of the search.
 *     - results: The array in which to store the results.
 *
 * The recursive search algorithm, called by the search() function.
 * Starts from the given path, takes parameters from command, 
 * and stores the results in the given StringArray. */
static void search_recursive(const char* path, FindCommand* command, StringArray* results) {
    DIR* dir = opendir(path);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and  ".."
        if (folder_pointer(entry->d_name)) continue;

        char* fullpath = join_path(path, entry->d_name);
        if (!fullpath) continue;

        struct stat st;
        if (lstat(fullpath, &st) != 0) { free(fullpath); continue; }

        // Always recurse into subdirectories, regardless of the visibility filter,
        // so hidden files nested inside visible directories are still found.
        if (S_ISDIR(st.st_mode) && command->recursive)
            search_recursive(fullpath, command, results);

        // Filter by visibility: skip hidden entries if only visible requested, and vice versa.
        bool is_hidden = entry->d_name[0] == '.';
        if ((command->include_hidden && !is_hidden) || (command->include_visible && is_hidden)) { free(fullpath); continue; }

        // If the command has no conditions, it has automatically been met.
        bool condition_met = true;

        // Combine conditions left to right, with no operator precedence.
        for (size_t c = 0; c < command->conditions.count; c++) {
            bool met = eval_condition(fullpath, entry->d_name, &command->conditions.items[c], command->include_hidden, command->include_visible);
            if (c == 0)
                condition_met = met;
            else if (command->conditions.items[c].logic_op == LOGIC_AND)
                condition_met = condition_met && met;
            else
                condition_met = condition_met || met;
        }

        // If our condition is met, check the file type. If the file type matches, add to results
        if (condition_met) {
            if ((command->type == TYPE_FILES && S_ISREG(st.st_mode)) ||
                (command->type == TYPE_FOLDERS && S_ISDIR(st.st_mode)) ||
                (command->type == TYPE_ITEMS && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))))
                AddString(results, fullpath);
        }
        free(fullpath);
    }
    closedir(dir);
}

// Root search command. Creates the StringArray object and calls the recursive searching.
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

    for (size_t i = 0; i < command->paths.count; i++) {
        if (!path_covered(&searched, command->paths.items[i])) {
            AddString(&searched, command->paths.items[i]);
            search_recursive(command->paths.items[i], command, &results);
        }
    }
    FreeList(&searched);
    dedupe_results(&results);
    return results;
}

// ================================================================================================
// Action Execution
// ================================================================================================

/* exec_action:
 *     - command: The command containing details and parameters for the action to execute.
 *     - results: The final StringArray containing the items.
 *
 * Using the parameters from the FindCommand, execute the given command on the result StringArray. */
static int exec_action(FindCommand* command, StringArray* results) {
    // If the action is to just print:
    if (command->action == ACTION_PRINT || results->count == 0) {
        for (size_t j = 0; j < results->count; j++)
            printf("%s\n", results->items[j]);
        return EXIT_SUCCESS;
    }

    // If the action is to delete files, give a confirmation before deleting files.
    if (command->action == ACTION_DELETE) {
        printf("Delete %zu %s? [y/N] ", results->count,
            command->type == TYPE_FILES ? "files" : command->type == TYPE_FOLDERS ? "folders" : "items");
        fflush(stdout);

        // Only proceed if input is "y" or "Y"
        char line[16];
        if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y')) {
            puts("Aborted.");
            return EXIT_SUCCESS;
        }

        // Parents first: a directory is handled before its descendants, and
        // descendants of an already-handled directory are skipped (their
        // contents moved/deleted with the parent; retrying would just noise).
        qsort(results->items, results->count, sizeof(char*), cmp_path_len_lex);
        StringArray done = {0};

        // Procedurally delete all files in list.
        for (size_t j = 0; j < results->count; j++) {
            bool covered = false;
            for (size_t k = 0; k < done.count; k++) {
                if (path_within_lex(done.items[k], results->items[j])) { covered = true; break; }
            }
            if (covered) continue;
            if (!delete(results->items[j])) {
                if (errno == ENOENT) continue; // already gone with a parent: not an error
                fprintf(stderr, "Failed to delete: %s\n", results->items[j]);
            } 
            else AddString(&done, results->items[j]);
        }
        FreeList(&done);
        return EXIT_SUCCESS;
    }

    if (command->action == ACTION_MOVE || command->action == ACTION_COPY) {

        // Make the target directory if it does not exist (tolerate EEXIST).
        if (mkdir(command->action_value, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Could not make target directory for %s\n",
                command->action == ACTION_MOVE ? "move" : "copy"
            );
            return EXIT_FAILURE;
        }

        // Parents-first sort
        qsort(results->items, results->count, sizeof(char*), cmp_path_len_lex);
        StringArray done = {0};

        for (size_t j = 0; j < results->count; j++) {
            bool covered = false;
            for (size_t k = 0; k < done.count; k++) {
                if (path_within_lex(done.items[k], results->items[j])) { covered = true; break; }
            }
            if (covered) continue;

            // Create destination file name (exactly sized, no truncation).
            char* dest = join_path(command->action_value, basename(results->items[j]));
            if (!dest) {
                fprintf(stderr, "Failed to %s: %s\n",
                    command->action == ACTION_MOVE ? "move" : "copy",
                    results->items[j]
                );
                continue;
            }

            // Copy or move to destinatio depending on action.
            bool ok = command->action == ACTION_MOVE
                ? move(results->items[j], dest)
                : copy(results->items[j], dest);

            if (!ok)
                fprintf(stderr, "Failed to %s: %s\n",
                    command->action == ACTION_MOVE ? "move" : "copy",
                    results->items[j]
                );
            else AddString(&done, results->items[j]);
            free(dest);
        }
        FreeList(&done);
        return EXIT_SUCCESS;
    }

    // If the action is to run a custom command, execute the command template on each item,
    // substituting any {file} placeholders with the item's path.
    if (command->action == ACTION_COMMAND) {
        int rc = EXIT_SUCCESS;
        for (size_t j = 0; j < results->count; j++) {
            // Get the filepath as a var to reduce complexity
            const char* filepath = results->items[j];

            // Build an argv array with one slot per template token plus a NULL terminator.
            char** args = malloc((command->command_template.count + 1) * sizeof(char*));
            if (!args) {
                perror("malloc");
                rc = EXIT_FAILURE;
                continue;
            }

            // Expand every {file} marker in each token.
            bool exp_ok = true;
            for (size_t k = 0; k < command->command_template.count; k++) {
                args[k] = expand_file_markers(command->command_template.items[k], filepath);
                if (!args[k]) {
                    perror("malloc");
                    for (size_t m = 0; m < k; m++)
                        free(args[m]);
                    free(args);
                    exp_ok = false;
                    break;
                }
            }
            if (!exp_ok) {
                rc = EXIT_FAILURE;
                continue;
            }
            args[command->command_template.count] = NULL;

            // Fork a child process and execute the command, waiting for it to finish.
            pid_t pid = fork();
            if (pid == 0) {
                execvp(args[0], args);
                fprintf(stderr, "Failed to exec: %s\n", args[0]);
                _exit(127);
            }
            else if (pid > 0) {
                int status;
                if (waitpid(pid, &status, 0) < 0) {
                    perror("waitpid");
                    rc = EXIT_FAILURE;
                } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    if (WIFEXITED(status))
                        fprintf(stderr, "Command exited with status %d: %s\n",
                            WEXITSTATUS(status), args[0]);
                    else
                        fprintf(stderr, "Command terminated abnormally: %s\n", args[0]);
                    rc = EXIT_FAILURE;
                }
            }
            else {
                perror("fork");
                rc = EXIT_FAILURE;
            }

            // Done! Free the template.
            for (size_t k = 0; k < command->command_template.count; k++)
                free(args[k]);

            free(args);
        }
        return rc;
    }
    return EXIT_SUCCESS;
}

// ================================================================================================
// Main Code
// ================================================================================================

int err(const char* msg) {
    fputs(msg, stderr);
    return EXIT_FAILURE;
}

int help(int code) {
    switch (code) {
        case 0:
            puts(
                "find.c - Find and operate on files with readable syntax.\n\n"

                "SYNOPSIS\n"
                "    find <type> [shallow] [visible|hidden] [in <paths>] [where <conditions>] [then <action>]\n\n"

                "TYPES\n"
                "    files       Search for files\n"
                "    folders     Search for directories\n"
                "    items       Search for both files and directories\n\n"

                "FLAGS\n"
                "    The program by default includes both hidden and visible files.\n"
                "    You cannot include both the hidden and visible flags\n\n"

                "    shallow     Only search the specified directory (no recursion)\n"
                "    visible     Only search visible files (Exclude hidden files)\n"
                "    hidden      Only search hidden files (Exclude visible files)\n"

                "PATHS\n"
                "    in <dir> [and <dir>...]\n"
                "        Directories to search. Defaults to \".\" if omitted.\n\n"

                "CONDITIONS\n"
                "    All conditions can be prefixed with \"not\" to negate them.\n"
                "    Conditions are combined left to right with \"and\" or \"or\".\n"
                "    There is no operator precedence; the expression is evaluated in order.\n\n"

                "    name contains <str>       Filename contains <str>\n"
                "    name startswith <str>     Filename starts with <str>\n"
                "    name endswith <str>       Filename ends with <str>\n"
                "    contents contains <str>   File contents contain <str>\n"
                "    contents similar <pct> to <file>\n"
                "                              File contents are at least <pct>% similar to <file>\n"
                "    contents <pct> similar to <file>\n"
                "                              File contents are at least <pct>% similar to <file>\n"
                "    size greaterthan <size>   File size greater than <size>\n"
                "    size lessthan <size>      File size less than <size>\n"
                "    <size> can be: 100, 10kb, 5mb, 2gb, 1tb\n\n"

                "    perms is <octal>          Permission bits match <octal> (e.g. 644)\n"
                "    perms exec                File is executable\n\n"
                "    date olderthan <date>     File was last modified before <date>\n"
                "    date newerthan <date>     File was last modified after <date>\n"
                "    <date> can be 40s 6m 8h 1D 4W 3M 2Y or MM.DD.YY\n\n"

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
                "    find folders hidden in ./cache where perms is 775 then delete"
                "    find files in src where contents contains TODO then command grep -n {file}\n"
                "    find files where contents similar 98 to main.c\n"
                "    find files where contents 0.98 similar to main.c\n"
                "    find files visible where name endswith .c or name endswith .h\n"
                "    find files where not name contains .o and name contains main\n"
                "    find items in .. where date olderthan 08.27.26 or perms exec then delete"
            );
            return EXIT_SUCCESS;
        case 1:  return err("find: <type> must be \"files\", \"folders\", or \"items\".\n");
        case 2:  return err("find: expected a path after \"in\".\n");
        case 3:  return err("find: incomplete or missing condition after \"where\".\n");
        case 4:  return err("find: unknown condition. Expected: name, size, date, perms, or contents.\n");
        case 5:  return err("find: expected \"then\", \"and\", or \"or\" after condition.\n");
        case 6:  return err("find: expected an action: moveto, copyto, delete, or command.\n");
        case 7:  return err("find: incomplete action. moveto/copyto require a destination, command requires arguments.\n");
        case 8:  return err("find: Unexpected argument after action.\n");
        case 9:  return err("find: \"contents contains\" is not valid with type \"items\".\n");
        case 10: return err("find: \"contents similar\" is only valid with type \"files\".\n");
        case 11: return err("find: \"hidden\" and \"visible\" cannot be used together (find searches both by default). \n");
        case 12: return err("find: Invalid time unit case. Use lowercase for seconds/minutes/hours (s, m, h) and uppercase for days/weeks/months/years (D, W, M, Y), e.g. 40s, 6m, 8h, 1D, 4W, 3M, 2Y.\n");
        case 13: return err("find: Valid modifiers are \"hidden\", \"visible\", and \"shallow\".\n");
        case 14: return err("find: Invalid characters after \"delete\".\n");
        default: return EXIT_FAILURE;
    }
}

/* main:
 *     - argc: The number of command line arguments.
 *     - argv: The array of command line argument strings.
 *
 * Parses the command line arguments using a state machine that transitions through
 * the following states: STATE_TYPE -> STATE_PATHS -> STATE_CONDITION -> STATE_ACTION -> STATE_DONE.
 * Each state consumes arguments and populates the FindCommand struct accordingly.
 * Once parsing is complete, the search is executed and the action is performed on the results. */
int main(int argc, char* argv[]) {

    // If only binary name passed, display help.
    if (argc == 1) return help(0);

    int i = 1;
    FindCommand command = {0};
    ParserState state = STATE_TYPE;

    // State machine loop: continues until a final state (DONE or ERROR) is reached.
    while (state != STATE_DONE && state != STATE_ERROR) {
        switch (state) {
            /* STATE_TYPE:
             * Parses the first argument to determine what to search for (files, folders, or items).
             * Sets default flags and transitions to STATE_MODIFIER to handle optional keyword modifiers. */
            case STATE_TYPE: {
                if (str_equals(argv[i], "files"))
                    command.type = TYPE_FILES;
                else if (str_equals(argv[i], "folders"))
                    command.type = TYPE_FOLDERS;
                else if (str_equals(argv[i], "items"))
                    command.type = TYPE_ITEMS;
                else return help(1);

                command.recursive = true;
                i++;
                state = STATE_MODIFIER;
                break;
            }
            /* STATE_MODIFIER:
             * Handles optional keyword modifiers: "shallow", "visible", "hidden".
             * Loops back to itself for each modifier found, then transitions to
             * STATE_PATHS, STATE_CONDITION, STATE_ACTION, or STATE_DONE depending
             * on the next argument, or finishes if no more arguments remain. */
            case STATE_MODIFIER: {
                if (i >= argc) {
                    AddString(&command.paths, ".");
                    command.action = ACTION_PRINT;
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "shallow")) {
                    command.recursive = false;
                    i++;
                }
                else if (str_equals(argv[i], "visible")) {
                    if (command.include_hidden) return help(11);
                    command.include_visible = true;
                    i++;
                }
                else if (str_equals(argv[i], "hidden")) {
                    if (command.include_visible) return help(11);
                    command.include_hidden = true;
                    i++;
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
                else return help(13);
                break;
            }
            /* STATE_PATHS:
             * Collects one or more directory paths following the "in" keyword.
             * Paths are separated by "and" keywords, which are skipped during parsing.
             * Parsing stops when "where" or "then" is encountered, or when arguments are exhausted.
             * Defaults to the current directory (".") if no paths were explicitly given. */
            case STATE_PATHS: {
                i++;
                // Collect paths until we hit a state keyword or run out of arguments.
                while (i < argc && !str_equals(argv[i], "where") && !str_equals(argv[i], "then")) {
                    // Skip "and" separators between paths (e.g., "in src and lib").
                    if (str_equals(argv[i], "and")) {
                        i++;
                        continue;
                    }
                    AddString(&command.paths, argv[i++]);
                }

                if (command.paths.count == 0) return help(2);
                else if (i >= argc) {
                    // No conditions or action specified; default to printing results.
                    command.action = ACTION_PRINT;
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "where"))
                    state = STATE_CONDITION;
                else if (str_equals(argv[i], "then"))
                    state = STATE_ACTION;

                break;
            }
            /* STATE_CONDITION:
             * Parses one or more conditions after the "where" keyword.
             * Each condition consists of a type (name, size, perms, contents), optional negation ("not"),
             * and a value/parameter. Conditions are linked by "and"/"or" operators with no precedence.
             * The loop continues until "then" is encountered (transitioning to STATE_ACTION)
             * or arguments are exhausted. Each parsed condition is added to the command's ConditionArray. */
            case STATE_CONDITION: {
                // The operator linking the next condition to the previous one.
                LogicOp next_op = LOGIC_AND;

                // Infinite loop that breaks when "then" is found or arguments are exhausted.
                for (;;) {
                    bool contains_keyword = true;
                    bool negated = false;
                    ConditionType cond_type = CONDITION_NONE;
                    const char* cond_value = NULL;
                    double similarity_threshold = 0.0;
                    const char* reference_file = NULL;

                    if (++i >= argc) return help(3);

                    // Check for optional "not" negation before the condition.
                    if (str_equals(argv[i], "not")) {
                        negated = true;
                        if (++i >= argc) return help(3);
                    }

                    // Parse "name" conditions: contains, startswith, endswith.
                    if (str_equals(argv[i], "name")) {
                        if (++i >= argc) return help(3);
                        else if (str_equals(argv[i], "contains"))
                            cond_type = CONDITION_NAME_CONTAINS;
                        else if (str_equals(argv[i], "endswith"))
                            cond_type = CONDITION_ENDSWITH;
                        else if (str_equals(argv[i], "ends")) {
                            // Separated words: "ends with <str>"
                            if (++i >= argc || !str_equals(argv[i], "with"))
                                return help(3);
                            cond_type = CONDITION_ENDSWITH;
                        }
                        else if (str_equals(argv[i], "startswith"))
                            cond_type = CONDITION_STARTSWITH;
                        else if (str_equals(argv[i], "starts")) {
                            // Separated words: "starts with <str>"
                            if (++i >= argc || !str_equals(argv[i], "with"))
                                return help(3);
                            cond_type = CONDITION_STARTSWITH;
                        }
                        else return help(4);
                    }
                    else if (str_equals(argv[i], "date")) {
                        if (++i >= argc) return help(3);
                        else if (str_equals(argv[i], "olderthan"))
                            cond_type = CONDITION_OLDERTHAN;
                        else if (str_equals(argv[i], "newerthan"))
                            cond_type = CONDITION_NEWERTHAN;
                        // Separated words: "older than <date>", "newer than <date>"
                        else if (str_equals(argv[i], "older")) {
                            if (++i >= argc || !str_equals(argv[i], "than")) return help(3);
                            cond_type = CONDITION_OLDERTHAN;
                        }
                        else if (str_equals(argv[i], "newer")) {
                            if (++i >= argc || !str_equals(argv[i], "than")) return help(3);
                            cond_type = CONDITION_NEWERTHAN;
                        }
                        else return help(4);
                    }
                    // Parse "size" conditions: lessthan, greaterthan (uses parse_bytes for units like "5mb").
                    else if (str_equals(argv[i], "size")) {
                        if (++i >= argc) return help(3);
                        else if (str_equals(argv[i], "lessthan"))
                            cond_type = CONDITION_LESSTHAN;
                        else if (str_equals(argv[i], "greaterthan"))
                            cond_type = CONDITION_GREATERTHAN;
                        // Separated words: "less than <size>", "greater than <size>"
                        else if (str_equals(argv[i], "less")) {
                            if (++i >= argc || !str_equals(argv[i], "than")) return help(3);
                            cond_type = CONDITION_LESSTHAN;
                        }
                        else if (str_equals(argv[i], "greater")) {
                            if (++i >= argc || !str_equals(argv[i], "than")) return help(3);
                            cond_type = CONDITION_GREATERTHAN;
                        }
                        else return help(4);
                    }
                    // Parse "perms" conditions: exec (executable bit) or is (exact permission bits).
                    else if (str_equals(argv[i], "perms")) {
                        if (++i >= argc) return help(3);
                        else if (str_equals(argv[i], "exec")) {
                            // "perms exec" has no value keyword; it just tests the executable bit.
                            cond_type = CONDITION_EXECUTABLE;
                            contains_keyword = false;
                        }
                        else if (str_equals(argv[i], "is"))
                            cond_type = CONDITION_PERM_BITS;
                        else return help(4);
                    }
                    // Parse "contents" conditions: contains (file content search), similar (file similarity), or folder containment count.
                    else if (str_equals(argv[i], "contents")) {
                        if (++i >= argc) return help(3);
                        // "contents similar <pct> to <file>" syntax
                        else if (str_equals(argv[i], "similar")) {
                            // Syntax: contents similar <pct> to <file>
                            // Similarity comparison only valid for file searches.
                            if (command.type != TYPE_FILES) return help(10);
                            if (++i >= argc) return help(3);

                            double pct = parse_similarity(argv[i]);
                            if (pct < 0.0) return help(3);

                            if (++i >= argc || !str_equals(argv[i], "to")) return help(3);
                            if (++i >= argc) return help(3);

                            cond_type = CONDITION_CONTENTS_SIMILAR;
                            contains_keyword = false;
                            similarity_threshold = pct;
                            reference_file = argv[i];
                        }
                        else if (!str_equals(argv[i], "contains")) {
                            // Syntax: contents <pct> similar to <file>
                            double pct = parse_similarity(argv[i]);
                            if (pct < 0.0) return help(4);

                            if (command.type != TYPE_FILES) return help(10);
                            if (++i >= argc || !str_equals(argv[i], "similar")) return help(3);
                            if (++i >= argc || !str_equals(argv[i], "to")) return help(3);
                            if (++i >= argc) return help(3);

                            cond_type = CONDITION_CONTENTS_SIMILAR;
                            contains_keyword = false;
                            similarity_threshold = pct;
                            reference_file = argv[i];
                        }
                        else if (command.type == TYPE_ITEMS) return help(9);

                        if (cond_type != CONDITION_CONTENTS_SIMILAR) {
                            // For file searches, use substring matching on file contents.
                            if (command.type == TYPE_FILES)
                                cond_type = CONDITION_FILE_CONTAINS;
                            else {
                                // For folder searches, parse the comparison operator, target count, and count type.
                                cond_type = CONDITION_FOLDER_CONTAINS;
                                if (++i >= argc) return help(3);

                                CompareOp op = CMP_EQ;
                                const char* tok = argv[i];

                                // Parse comparison operator: can be a separate token (">", ">=") or prefix (">5").
                                if (tok[0] == '>' || tok[0] == '<') {
                                    char op_char = tok[0];

                                    if (tok[1] == '=') {
                                        op = op_char == '>' ? CMP_GTE : CMP_LTE;
                                        tok += 2;
                                    } else {
                                        op = op_char == '>' ? CMP_GT : CMP_LT;
                                        tok += 1;
                                    }

                                    if (*tok == '\0') {
                                        if (++i >= argc) return help(3);
                                        tok = argv[i];
                                    }
                                }
                                // Parse the numeric target for the comparison.
                                char* end;
                                long target = strtol(tok, &end, 10);
                                if (end == tok || target < 0 || *end != '\0') return help(3);
                                if (++i >= argc) return help(3);

                                // Parse what to count: items, files, or folders.
                                CountWhat what;
                                if (str_equals(argv[i], "items"))
                                    what = COUNT_ITEMS;
                                else if (str_equals(argv[i], "files"))
                                    what = COUNT_FILES;
                                else if (str_equals(argv[i], "folders"))
                                    what = COUNT_FOLDERS;
                                else return help(4);

                                // Optional "shallow" flag to disable recursion when counting.
                                bool shallow = false;
                                if (i + 1 < argc && str_equals(argv[i + 1], "shallow")) {
                                    shallow = true;
                                    i++;
                                }

                                cond_value = NULL;
                                contains_keyword = false;

                                // Add the folder contains condition directly since it has a complex structure.
                                AddCondition(&command.conditions, (Condition){
                                    .type = CONDITION_FOLDER_CONTAINS,
                                    .value = NULL,
                                    .negated = negated,
                                    .logic_op = next_op,
                                    .compare_op = op,
                                    .count_target = target,
                                    .count_what = what,
                                    .count_shallow = shallow,
                                });

                                if (++i >= argc) {
                                    state = STATE_DONE;
                                    break;
                                }
                                else if (str_equals(argv[i], "and")) {
                                    next_op = LOGIC_AND;
                                    continue;
                                }
                                else if (str_equals(argv[i], "or")) {
                                    next_op = LOGIC_OR;
                                    continue;
                                }
                                else if (str_equals(argv[i], "then")) {
                                    state = STATE_ACTION;
                                    break;
                                }
                                else return help(5);
                            }
                        }
                    }
                    else return help(4);

                    // For conditions that require a value keyword (e.g., "name contains <str>").
                    if (contains_keyword) {
                        if (++i >= argc) return help(3);
                        else cond_value = argv[i];

                        // Validate date values up front so the user gets one clear
                        // help message instead of a silent no-match (or per-file spam).
                        if (cond_type == CONDITION_OLDERTHAN || cond_type == CONDITION_NEWERTHAN) {
                            time_t cutoff = parse_date(cond_value);
                            if (cutoff == (time_t)-2) return help(12);
                            if (cutoff == (time_t)-1) return help(3);
                        }
                        if (cond_type == CONDITION_GREATERTHAN || cond_type == CONDITION_LESSTHAN) {
                            if (parse_bytes(cond_value) < 0) return help(3);
                        }
                    }

                    AddCondition(&command.conditions, (Condition){
                        .type = cond_type,
                        .value = (char*)cond_value,
                        .negated = negated,
                        .logic_op = next_op,
                        .similarity_threshold = similarity_threshold,
                        .reference_file = (char*)reference_file,
                    });

                    // Determine the next state based on the following argument.
                    if (++i >= argc) {
                        // No more arguments; finish parsing.
                        state = STATE_DONE;
                        break;
                    }
                    else if (str_equals(argv[i], "and")) {
                        // Link next condition with AND.
                        next_op = LOGIC_AND;
                        continue;
                    }
                    else if (str_equals(argv[i], "or")) {
                        // Link next condition with OR.
                        next_op = LOGIC_OR;
                        continue;
                    }
                    else if (str_equals(argv[i], "then")) {
                        // Transition to the action state.
                        state = STATE_ACTION;
                        break;
                    }
                    else return help(5);
                }
                break;
            }

            /* STATE_ACTION:
             * Parses the action to perform on the search results after the "then" keyword.
             * Supported actions: moveto (move files), copyto (copy files), delete (remove files),
             * or command (run a custom command with {file} placeholder expansion).
             * moveto/copyto require a destination directory argument.
             * command collects all remaining arguments as the command template.
             * delete takes no additional arguments. */
            case STATE_ACTION: {
                if (++i >= argc) return help(6);
                else if (str_equals(argv[i], "moveto"))
                    command.action = ACTION_MOVE;
                else if (str_equals(argv[i], "copyto"))
                    command.action = ACTION_COPY;
                else if (str_equals(argv[i], "delete")) {
                    command.action = ACTION_DELETE;
                    if (++i != argc) return help(14);
                    state = STATE_DONE;
                    break;
                }
                else if (str_equals(argv[i], "command")) {
                    command.action = ACTION_COMMAND;
                    i++;
                    while (i < argc)
                        AddString(&command.command_template, argv[i++]);
                    if (command.command_template.count == 0) return help(7);
                    state = STATE_DONE;
                    break;
                }
                else return help(6);

                if (++i >= argc) return help(7);
                else command.action_value = argv[i];

                if (++i != argc) return help(8);
                else state = STATE_DONE;

                break;
            }
            case STATE_DONE:
            case STATE_ERROR:
                break;
        }
    }

    // Execute the search using the populated FindCommand struct.
    StringArray results = search(&command);

    // Perform the specified action on the search results.
    int exit_code = exec_action(&command, &results);

    // Clean up all dynamically allocated memory.
    FreeList(&results);
    FreeConditions(&command.conditions);
    FreeList(&command.command_template);
    FreeList(&command.paths);
    return exit_code;
}
