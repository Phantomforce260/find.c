#include <stddef.h>
#include <stdbool.h>

/*
    command     := type "in" paths [where] [then]

    type        := "files" | "folders"

    paths       := path ("and" path)*

    where       := "where" field operator value

    field       := "name"

    operator    := "contains"
                 | "endswith"
                 | "startswith"

    then        := "then" action

    action      := "move" "to" path
*/

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
    CONDITION_CONTAINS,
    CONDITION_STARTSWITH,
    CONDITION_ENDSWITH
} ConditionType;

typedef enum {
    ACTION_PRINT,
    ACTION_MOVE,
    ACTION_DELETE,
    ACTION_COPY,
    ACTION_COMMAND
} ActionType;

// A custom data structure that makes an ArrayList of strings with properties
// to manage it.
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

// types.c

void AddString(StringArray* array, const char* string);

void FreeList(StringArray* array);

StringArray search(FindCommand* command);

void search_recursive(const char* path, int want_files, StringArray* results);

