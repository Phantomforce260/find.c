#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TYPE_FILES,
    TYPE_FOLDERS,
} EntryType;

typedef enum {
    CONDITION_NONE,
    CONDITION_NAME_CONTAINS,
    CONDITION_FILE_CONTAINS,
    CONDITION_STARTSWITH,
    CONDITION_ENDSWITH,
    CONDITION_LESSTHAN,
    CONDITION_GREATERTHAN,
    CONDITION_EXECUTABLE,
    CONDITION_PERM_BITS
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
    ConditionType type;
    char* value;
    bool negated;
} Condition;

typedef struct {
    Condition* items;
    size_t count;
    size_t capacity;
} ConditionArray;

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

void AddCondition(ConditionArray* array, ConditionType type, const char* value, bool negated) {
    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity == 0
            ? 16
            : array->capacity * 2;

        Condition* new_items = realloc(
            array->items,
            new_capacity * sizeof(Condition)
        );

        if (!new_items) {
            puts("Error: Could not expand ConditionArray!");
            exit(EXIT_FAILURE);
        }

        array->items = new_items;
        array->capacity = new_capacity;
    }

    array->items[array->count].type = type;
    array->items[array->count].value = value ? strdup(value) : NULL;
    array->items[array->count].negated = negated;
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
