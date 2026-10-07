// Host-test stub for Android's <cutils/properties.h>.
// property "a.b.c" is read from the environment variable PROP_a_b_c.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROPERTY_VALUE_MAX 92

static inline int property_get(const char* key, char* value, const char* def) {
    char name[256];
    snprintf(name, sizeof(name), "PROP_%s", key);
    for (char* p = name; *p; p++) {
        if (*p == '.') *p = '_';
    }
    const char* v = getenv(name);
    if (v == NULL) v = def;
    if (v == NULL) {
        value[0] = '\0';
        return 0;
    }
    strncpy(value, v, PROPERTY_VALUE_MAX - 1);
    value[PROPERTY_VALUE_MAX - 1] = '\0';
    return int(strlen(value));
}
