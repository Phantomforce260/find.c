#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "find.h"

int help(int code, const char* args) {
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
            printf("find expects [type] of \"files\" or \"folders\". Received \"%s\" ", args);
            return EXIT_FAILURE;
        case 2:
            puts("find expects a path after \"in\".");
            return EXIT_FAILURE;
        case 3:
            puts("Incomplete Condition.");
            return EXIT_FAILURE;
        default:
            return EXIT_FAILURE;
    }
}

bool str_equals(const char* str1, const char* str2) {
    return strcmp(str1, str2) == 0;
}

int main(int argc, char* argv[]) {

    /* If only binary name passed, display help. */
    if (argc == 1)
        return help(0, NULL);

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
                    return help(1, argv[i]);

                i++;

                if (i >= argc) {
                    AddString(&command.paths, ".");
                    command.action = ACTION_PRINT;
                    state = STATE_DONE;
                }
                else {
                    i++;
                    state = STATE_PATHS;
                }

                break;
            case STATE_PATHS:
                while (i < argc && !str_equals(argv[i], "where") && !str_equals(argv[i], "then")) {

                    if (str_equals(argv[i], "and")) {
                        i++;
                        continue;
                    }

                    AddString(&command.paths, argv[i]);
                    i++;
                }

                if (command.paths.count == 0)
                    return help(2, NULL);
                else if (i >= argc) {
                    command.action = ACTION_PRINT;
                    state = STATE_DONE;
                }
                else if (str_equals(argv[i], "where")) {
                    i++;
                    state = STATE_CONDITION;
                }
                else if (str_equals(argv[i], "then")) {
                    i++;
                    state = STATE_ACTION;
                }

                break;
            case STATE_CONDITION:

                if (i >= argc || str_equals(argv[i], "name"))
                    return help(3, NULL);

                i++;

                if (i >= argc) {
                    fprintf(stderr, "Expected condition operator.\n");
                    state = STATE_ERROR;
                    break;
                }

                if (strcmp(argv[i], "contains") == 0)
                    command.condition = CONDITION_CONTAINS;
                else if (strcmp(argv[i], "endswith") == 0)
                    command.condition = CONDITION_ENDSWITH;
                else {
                    fprintf(
                        stderr,
                        "Unknown condition: %s\n",
                        argv[i]
                    );

                    state = STATE_ERROR;
                    break;
                }

                i++;

                if (i >= argc) {
                    fprintf(stderr, "Expected condition value.\n");
                    state = STATE_ERROR;
                    break;
                }

                command.value = argv[i];
                i++;

                if (i >= argc)
                    state = STATE_DONE;
                else if (strcmp(argv[i], "then") == 0) {
                    i++;
                    state = STATE_ACTION;
                }
                else {
                    fprintf(
                        stderr,
                        "Expected \"then\" or end of command.\n"
                    );

                    state = STATE_ERROR;
                }

                break;
            case STATE_ACTION:
                if (i >= argc || strcmp(argv[i], "move") != 0) {
                    fprintf(stderr, "Expected action.\n");
                    state = STATE_ERROR;
                    break;
                }

                i++;

                if (i >= argc || strcmp(argv[i], "to") != 0) {
                    fprintf(stderr, "Expected \"to\" after \"move\".\n");
                    state = STATE_ERROR;
                    break;
                }

                i++;

                if (i >= argc) {
                    fprintf(stderr, "Expected destination path.\n");
                    state = STATE_ERROR;
                    break;
                }

                //command.has_action = true;
                //command.action = argv[i];

                i++;

                if (i != argc) {
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
