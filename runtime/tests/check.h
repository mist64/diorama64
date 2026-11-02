#pragma once
#include <stdio.h>
#include <stdlib.h>
#define CHECK(cond)                                         \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            fflush(stdout);                                 \
            exit(1);                                        \
        }                                                   \
    } while (0)
#define SKIP(msg)                      \
    do {                               \
        printf("SKIP %s\n", (msg));    \
        fflush(stdout);                \
        exit(77);                      \
    } while (0)
