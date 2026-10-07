// Host-side test for device/amazon/biscuit/hwcomposer.
// Loads the module like hw_get_module() does and drives it the way
// SurfaceFlinger 7.1 (HWComposer_hwc1.cpp) does.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static const hw_module_t* g_module;

static hwc_composer_device_1_t* openDevice() {
    hw_device_t* dev = NULL;
    int err = g_module->methods->open(g_module, HWC_HARDWARE_COMPOSER, &dev);
    CHECK(err == 0);
    return reinterpret_cast<hwc_composer_device_1_t*>(dev);
}

static void closeDevice(hwc_composer_device_1_t* dev) {
    CHECK(dev->common.close(&dev->common) == 0);
}

static void sleepMs(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// ---- vsync recorder -------------------------------------------------------
static std::mutex g_vsyncLock;
static std::vector<int64_t> g_vsyncTs;
static std::atomic<int> g_vsyncDisp(-1);
static void onVsync(const hwc_procs_t*, int disp, int64_t ts) {
    std::lock_guard<std::mutex> l(g_vsyncLock);
    g_vsyncTs.push_back(ts);
    g_vsyncDisp = disp;
}
static void onInvalidate(const hwc_procs_t*) {}
static hwc_procs_t g_procs = {onInvalidate, onVsync, NULL};
static size_t vsyncCount() {
    std::lock_guard<std::mutex> l(g_vsyncLock);
    return g_vsyncTs.size();
}

// ---- tests ----------------------------------------------------------------
static void testModuleAndVersion() {
    fprintf(stderr, "[test] module + version\n");
    CHECK(g_module->id != NULL && strcmp(g_module->id, HWC_HARDWARE_MODULE_ID) == 0);
    CHECK(g_module->module_api_version == HWC_MODULE_API_VERSION_0_1);
    hw_device_t* dev = NULL;
    CHECK(g_module->methods->open(g_module, "nonsense", &dev) == -EINVAL);

    hwc_composer_device_1_t* d = openDevice();
    CHECK(d->common.tag == HARDWARE_DEVICE_TAG);
    CHECK(d->common.version == HWC_DEVICE_API_VERSION_1_1);
    CHECK(d->common.module == g_module);
    closeDevice(d);
}

static void testQuery() {
    fprintf(stderr, "[test] query\n");
    hwc_composer_device_1_t* d = openDevice();
    int v = -1;
    CHECK(d->query(d, HWC_BACKGROUND_LAYER_SUPPORTED, &v) == 0 && v == 0);
    CHECK(d->query(d, HWC_VSYNC_PERIOD, &v) == 0 && v == 16666666);
    CHECK(d->query(d, HWC_DISPLAY_TYPES_SUPPORTED, &v) == 0 && v == HWC_DISPLAY_PRIMARY_BIT);
    CHECK(d->query(d, 12345, &v) == -EINVAL);
    CHECK(d->query(d, HWC_VSYNC_PERIOD, NULL) == -EINVAL);
    closeDevice(d);
}

// Mirrors HWComposer::queryDisplayProperties() in SurfaceFlinger 7.1.
static void testDisplayProperties(int expectW, int expectH) {
    fprintf(stderr, "[test] display properties (expect %dx%d)\n", expectW, expectH);
    hwc_composer_device_1_t* d = openDevice();

    uint32_t configs[128] = {0};
    size_t num = 128;
    CHECK(d->getDisplayConfigs(d, HWC_DISPLAY_PRIMARY, configs, &num) == 0);
    CHECK(num == 1 && configs[0] == 0);

    // external display must look "not connected"
    num = 128;
    CHECK(d->getDisplayConfigs(d, HWC_DISPLAY_EXTERNAL, configs, &num) != 0);

    const uint32_t withColor[] = {HWC_DISPLAY_VSYNC_PERIOD, HWC_DISPLAY_WIDTH,
            HWC_DISPLAY_HEIGHT, HWC_DISPLAY_DPI_X, HWC_DISPLAY_DPI_Y,
            HWC_DISPLAY_COLOR_TRANSFORM, HWC_DISPLAY_NO_ATTRIBUTE};
    const uint32_t preHwc15[] = {HWC_DISPLAY_VSYNC_PERIOD, HWC_DISPLAY_WIDTH,
            HWC_DISPLAY_HEIGHT, HWC_DISPLAY_DPI_X, HWC_DISPLAY_DPI_Y,
            HWC_DISPLAY_NO_ATTRIBUTE};
    int32_t values[8];
    memset(values, 0, sizeof(values));
    CHECK(d->getDisplayAttributes(d, HWC_DISPLAY_PRIMARY, 0, withColor, values) != 0);
    CHECK(d->getDisplayAttributes(d, HWC_DISPLAY_PRIMARY, 0, preHwc15, values) == 0);
    CHECK(values[0] == 16666666);
    CHECK(values[1] == expectW);
    CHECK(values[2] == expectH);
    CHECK(values[3] == 213000 && values[4] == 213000);
    CHECK(d->getDisplayAttributes(d, HWC_DISPLAY_PRIMARY, 1, preHwc15, values) != 0);
    CHECK(d->getDisplayAttributes(d, HWC_DISPLAY_EXTERNAL, 0, preHwc15, values) != 0);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    d->dump(d, buf, sizeof(buf));
    CHECK(strstr(buf, "headless HWC 1.1") != NULL);
    char tiny[8];
    d->dump(d, tiny, sizeof(tiny));  // must truncate, not overflow
    CHECK(tiny[sizeof(tiny) - 1] == '\0');
    closeDevice(d);
}

static void testVsync() {
    fprintf(stderr, "[test] vsync\n");
    hwc_composer_device_1_t* d = openDevice();
    {
        std::lock_guard<std::mutex> l(g_vsyncLock);
        g_vsyncTs.clear();
    }
    d->registerProcs(d, &g_procs);

    CHECK(d->eventControl(d, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, 0) == 0);  // SF does this first
    sleepMs(100);
    CHECK(vsyncCount() == 0);

    CHECK(d->eventControl(d, HWC_DISPLAY_EXTERNAL, HWC_EVENT_VSYNC, 1) == -EINVAL);
    CHECK(d->eventControl(d, HWC_DISPLAY_PRIMARY, 77, 1) == -EINVAL);

    CHECK(d->eventControl(d, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, 1) == 0);
    sleepMs(1000);
    CHECK(d->eventControl(d, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, 0) == 0);
    sleepMs(50);
    const size_t n = vsyncCount();
    fprintf(stderr, "  %zu vsync callbacks in ~1s\n", n);
    CHECK(n >= 50 && n <= 66);
    CHECK(g_vsyncDisp == HWC_DISPLAY_PRIMARY);

    {
        std::lock_guard<std::mutex> l(g_vsyncLock);
        int64_t prev = g_vsyncTs[0];
        bool monotonic = true, nominal = true;
        for (size_t i = 1; i < g_vsyncTs.size(); i++) {
            int64_t delta = g_vsyncTs[i] - prev;
            if (delta <= 0) monotonic = false;
            if (delta % 16666666 != 0) nominal = false;  // nominal ticks only
            prev = g_vsyncTs[i];
        }
        CHECK(monotonic);
        CHECK(nominal);
    }

    sleepMs(200);
    CHECK(vsyncCount() == n);  // nothing after disable
    closeDevice(d);
}

static hwc_display_contents_1_t* newList(size_t layers) {
    size_t size = sizeof(hwc_display_contents_1_t) + layers * sizeof(hwc_layer_1_t);
    hwc_display_contents_1_t* l = static_cast<hwc_display_contents_1_t*>(calloc(1, size));
    l->numHwLayers = layers;
    l->retireFenceFd = -1;
    l->flags = HWC_GEOMETRY_CHANGED;
    for (size_t i = 0; i < layers; i++) {
        l->hwLayers[i].acquireFenceFd = -1;
        l->hwLayers[i].releaseFenceFd = -1;
    }
    return l;
}

static bool fdClosed(int fd) { return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

static void testPrepareSet() {
    fprintf(stderr, "[test] prepare/set\n");
    hwc_composer_device_1_t* d = openDevice();

    // 1.1: two display slots, the external one is NULL (not connected)
    hwc_display_contents_1_t* primary = newList(4);
    primary->hwLayers[0].compositionType = HWC_FRAMEBUFFER;
    primary->hwLayers[1].compositionType = HWC_OVERLAY;
    primary->hwLayers[1].flags = HWC_SKIP_LAYER;
    primary->hwLayers[2].compositionType = HWC_OVERLAY;
    primary->hwLayers[3].compositionType = HWC_FRAMEBUFFER_TARGET;
    hwc_display_contents_1_t* displays[2] = {primary, NULL};

    CHECK(d->prepare(d, 2, displays) == 0);
    CHECK(primary->hwLayers[0].compositionType == HWC_FRAMEBUFFER);
    CHECK(primary->hwLayers[1].compositionType == HWC_FRAMEBUFFER);
    CHECK(primary->hwLayers[2].compositionType == HWC_FRAMEBUFFER);
    CHECK((primary->hwLayers[1].flags & HWC_SKIP_LAYER) == 0);
    CHECK(primary->hwLayers[3].compositionType == HWC_FRAMEBUFFER_TARGET);

    int fbtPipe[2], layerPipe[2];
    CHECK(pipe(fbtPipe) == 0 && pipe(layerPipe) == 0);
    primary->hwLayers[3].acquireFenceFd = fbtPipe[0];
    primary->hwLayers[1].acquireFenceFd = layerPipe[0];
    primary->hwLayers[0].releaseFenceFd = 99;  // garbage must be reset
    CHECK(d->set(d, 2, displays) == 0);
    CHECK(fdClosed(fbtPipe[0]));
    CHECK(fdClosed(layerPipe[0]));
    CHECK(primary->hwLayers[3].acquireFenceFd == -1);
    CHECK(primary->hwLayers[0].releaseFenceFd == -1);
    CHECK(primary->retireFenceFd == -1);
    close(fbtPipe[1]);
    close(layerPipe[1]);

    CHECK(d->prepare(d, 0, NULL) == 0);
    CHECK(d->set(d, 0, NULL) == 0);
    CHECK(d->blank(d, HWC_DISPLAY_PRIMARY, 1) == 0);
    CHECK(d->blank(d, HWC_DISPLAY_PRIMARY, 0) == 0);

    // layer-less list (SF with nothing to draw)
    hwc_display_contents_1_t* empty = newList(0);
    hwc_display_contents_1_t* only[1] = {empty};
    CHECK(d->prepare(d, 1, only) == 0 && d->set(d, 1, only) == 0);

    free(primary);
    free(empty);
    closeDevice(d);
}

struct StressArgs {
    hwc_composer_device_1_t* dev;
    std::atomic<bool> stop;
};
static void* stressSideThread(void* p) {
    StressArgs* a = static_cast<StressArgs*>(p);
    char buf[128];
    while (!a->stop) {
        a->dev->registerProcs(a->dev, &g_procs);
        a->dev->dump(a->dev, buf, sizeof(buf));
    }
    return NULL;
}

static void testStressAndLifecycle() {
    fprintf(stderr, "[test] stress + open/close cycles\n");
    hwc_composer_device_1_t* d = openDevice();
    d->registerProcs(d, &g_procs);
    StressArgs args;
    args.dev = d;
    args.stop = false;
    pthread_t side;
    CHECK(pthread_create(&side, NULL, stressSideThread, &args) == 0);
    for (int i = 0; i < 3000; i++) {
        CHECK(d->eventControl(d, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, i & 1) == 0);
        if ((i % 100) == 0) sleepMs(2);
    }
    args.stop = true;
    pthread_join(side, NULL);
    closeDevice(d);  // closes while vsync may still be enabled

    for (int i = 0; i < 100; i++) {
        hwc_composer_device_1_t* x = openDevice();
        x->registerProcs(x, &g_procs);
        x->eventControl(x, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, 1);
        closeDevice(x);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <hwcomposer.so> [expected WxH]\n", argv[0]);
        return 2;
    }
    int w = 320, h = 320;
    if (argc >= 3 && sscanf(argv[2], "%dx%d", &w, &h) != 2) return 2;

    void* so = dlopen(argv[1], RTLD_NOW);
    if (so == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }
    g_module = static_cast<const hw_module_t*>(dlsym(so, HAL_MODULE_INFO_SYM_AS_STR));
    if (g_module == NULL) {
        fprintf(stderr, "no %s symbol\n", HAL_MODULE_INFO_SYM_AS_STR);
        return 2;
    }

    testModuleAndVersion();
    testQuery();
    testDisplayProperties(w, h);
    testVsync();
    testPrepareSet();
    testStressAndLifecycle();

    if (g_failures) {
        fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
