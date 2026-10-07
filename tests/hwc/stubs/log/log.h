// Host-test stub for Android's <log/log.h>. Info logs are silent unless
// HWC_TEST_VERBOSE is set; warnings and errors always print.
#pragma once
#include <stdio.h>
#include <stdlib.h>

#define ALOGV(...) ((void)0)
#define ALOGI(fmt, ...) \
    do { if (getenv("HWC_TEST_VERBOSE")) fprintf(stderr, "  [hwc I] " fmt "\n", ##__VA_ARGS__); } while (0)
#define ALOGW(fmt, ...) fprintf(stderr, "  [hwc W] " fmt "\n", ##__VA_ARGS__)
#define ALOGE(fmt, ...) fprintf(stderr, "  [hwc E] " fmt "\n", ##__VA_ARGS__)
