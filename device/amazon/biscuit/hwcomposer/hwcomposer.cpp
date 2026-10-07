/*
 * Headless hardware composer (HWC 1.1) for the Amazon Biscuit (Echo Dot 2nd gen).
 *
 * Biscuit has no panel and its kernel has no framebuffer driver (CONFIG_MTK_FB
 * is off, the MediaTek display sources are not part of the Fire OS 6.5.7.1
 * drop), so there is nothing to scan out to. SurfaceFlinger still needs a
 * primary display object, a vsync source and somewhere to post composed frames.
 *
 * The stock blob (hwcomposer.mt8163.so) is a sample HWC that declares API 1.0.
 * SurfaceFlinger then demands a framebuffer device and aborts without one.
 * This module declares API 1.1 instead, so SurfaceFlinger takes the display
 * description from the HWC (getDisplayConfigs/getDisplayAttributes) and needs
 * no fbdev.
 *
 * Behaviour:
 *   - one always-connected primary display, size from debug.biscuit.hwc.size
 *     ("WxH", default 320x320), 60 Hz, 213 dpi (ACONFIGURATION_DENSITY_TV)
 *   - no external display (disp 1 reports an error, i.e. "not connected")
 *   - prepare() marks every layer HWC_FRAMEBUFFER, so SurfaceFlinger composes
 *     everything with GLES (software GL is fine) into the framebuffer target
 *   - set() consumes the frame: it closes the fences the HWC owns and posts
 *     nothing
 *   - vsync is generated in software (timerfd, 60 Hz) while enabled
 */
#define LOG_TAG "hwcomposer.biscuit"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

namespace {

const int kDefaultWidth = 320;
const int kDefaultHeight = 320;
const int kMaxDimension = 4096;
const int64_t kVsyncPeriodNs = 16666666;  // 60 Hz
const int32_t kDpiMilli = 213000;         // ACONFIGURATION_DENSITY_TV, in 1/1000 dpi
const char kSizeProperty[] = "debug.biscuit.hwc.size";

struct HwcContext {
    hwc_composer_device_1_t device;  // must stay the first member
    int width;
    int height;

    pthread_mutex_t lock;            // protects the fields below
    const hwc_procs_t* procs;
    bool vsyncEnabled;
    int64_t vsyncBaseNs;

    int timerFd;                     // periodic vsync timer
    int exitFd;                      // written to stop the vsync thread
    pthread_t thread;
};

HwcContext* contextOf(hwc_composer_device_1_t* dev) {
    return reinterpret_cast<HwcContext*>(dev);
}

int64_t nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct timespec toTimespec(int64_t ns) {
    struct timespec ts;
    ts.tv_sec = ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    return ts;
}

void loadSize(int* width, int* height) {
    *width = kDefaultWidth;
    *height = kDefaultHeight;

    char value[PROPERTY_VALUE_MAX];
    if (property_get(kSizeProperty, value, "") <= 0) {
        return;
    }
    int w = 0;
    int h = 0;
    char trailing;
    if (sscanf(value, "%dx%d%c", &w, &h, &trailing) == 2 &&
            w >= 1 && h >= 1 && w <= kMaxDimension && h <= kMaxDimension) {
        *width = w;
        *height = h;
    } else {
        ALOGW("ignoring invalid %s='%s', using %dx%d", kSizeProperty, value,
                kDefaultWidth, kDefaultHeight);
    }
}

// Arms or disarms the vsync timer. Timestamps handed to SurfaceFlinger are
// nominal ticks (base + n * period), like SurfaceFlinger's own fake vsync.
int setVsync(HwcContext* ctx, bool enable) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));

    pthread_mutex_lock(&ctx->lock);
    int flags = 0;
    if (enable) {
        ctx->vsyncBaseNs = nowNs();
        its.it_value = toTimespec(ctx->vsyncBaseNs + kVsyncPeriodNs);
        its.it_interval = toTimespec(kVsyncPeriodNs);
        flags = TFD_TIMER_ABSTIME;
    }
    ctx->vsyncEnabled = enable;
    int err = timerfd_settime(ctx->timerFd, flags, &its, NULL);
    int savedErrno = errno;
    pthread_mutex_unlock(&ctx->lock);

    if (err != 0) {
        ALOGE("timerfd_settime failed: %s", strerror(savedErrno));
        return -savedErrno;
    }
    return 0;
}

void* vsyncThread(void* arg) {
    HwcContext* ctx = static_cast<HwcContext*>(arg);
    struct pollfd fds[2];
    fds[0].fd = ctx->timerFd;
    fds[0].events = POLLIN;
    fds[1].fd = ctx->exitFd;
    fds[1].events = POLLIN;

    for (;;) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            ALOGE("vsync poll failed: %s", strerror(errno));
            break;
        }
        if (fds[1].revents != 0) {
            break;
        }
        if ((fds[0].revents & POLLIN) == 0) {
            continue;
        }

        uint64_t expirations = 0;
        if (read(ctx->timerFd, &expirations, sizeof(expirations)) !=
                ssize_t(sizeof(expirations))) {
            continue;  // timer was disarmed in the meantime
        }

        pthread_mutex_lock(&ctx->lock);
        const bool enabled = ctx->vsyncEnabled;
        const hwc_procs_t* procs = ctx->procs;
        const int64_t base = ctx->vsyncBaseNs;
        pthread_mutex_unlock(&ctx->lock);

        if (!enabled || procs == NULL || procs->vsync == NULL) {
            continue;
        }
        // Never call back with the lock held: SurfaceFlinger may call
        // eventControl() from inside the callback chain.
        const int64_t now = nowNs();
        const int64_t timestamp = now - ((now - base) % kVsyncPeriodNs);
        procs->vsync(procs, HWC_DISPLAY_PRIMARY, timestamp);
    }
    return NULL;
}

int hwc_prepare(hwc_composer_device_1_t* /*dev*/, size_t numDisplays,
        hwc_display_contents_1_t** displays) {
    if (displays == NULL) {
        return 0;
    }
    for (size_t i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        if (list == NULL) {
            continue;
        }
        for (size_t j = 0; j < list->numHwLayers; j++) {
            hwc_layer_1_t* layer = &list->hwLayers[j];
            if (layer->compositionType == HWC_FRAMEBUFFER_TARGET ||
                    layer->compositionType == HWC_SIDEBAND) {
                continue;
            }
            layer->compositionType = HWC_FRAMEBUFFER;
            layer->flags &= ~uint32_t(HWC_SKIP_LAYER);
        }
    }
    return 0;
}

int hwc_set(hwc_composer_device_1_t* /*dev*/, size_t numDisplays,
        hwc_display_contents_1_t** displays) {
    if (displays == NULL) {
        return 0;
    }
    for (size_t i = 0; i < numDisplays; i++) {
        hwc_display_contents_1_t* list = displays[i];
        if (list == NULL) {
            continue;
        }
        // The HWC owns the acquire fences it is handed and must close them;
        // nothing is displayed, so there are no release/retire fences.
        for (size_t j = 0; j < list->numHwLayers; j++) {
            hwc_layer_1_t* layer = &list->hwLayers[j];
            if (layer->acquireFenceFd >= 0) {
                close(layer->acquireFenceFd);
                layer->acquireFenceFd = -1;
            }
            layer->releaseFenceFd = -1;
        }
        list->retireFenceFd = -1;
    }
    return 0;
}

int hwc_event_control(hwc_composer_device_1_t* dev, int disp, int event, int enabled) {
    if (disp != HWC_DISPLAY_PRIMARY || event != HWC_EVENT_VSYNC) {
        return -EINVAL;
    }
    return setVsync(contextOf(dev), enabled != 0);
}

int hwc_blank(hwc_composer_device_1_t* /*dev*/, int disp, int /*blank*/) {
    return disp == HWC_DISPLAY_PRIMARY ? 0 : -EINVAL;
}

int hwc_query(hwc_composer_device_1_t* /*dev*/, int what, int* value) {
    if (value == NULL) {
        return -EINVAL;
    }
    switch (what) {
        case HWC_BACKGROUND_LAYER_SUPPORTED:
            *value = 0;
            return 0;
        case HWC_VSYNC_PERIOD:
            *value = int(kVsyncPeriodNs);
            return 0;
        case HWC_DISPLAY_TYPES_SUPPORTED:
            *value = HWC_DISPLAY_PRIMARY_BIT;
            return 0;
        default:
            return -EINVAL;
    }
}

void hwc_register_procs(hwc_composer_device_1_t* dev, const hwc_procs_t* procs) {
    HwcContext* ctx = contextOf(dev);
    pthread_mutex_lock(&ctx->lock);
    ctx->procs = procs;
    pthread_mutex_unlock(&ctx->lock);
}

void hwc_dump(hwc_composer_device_1_t* dev, char* buff, int buffLen) {
    if (buff == NULL || buffLen <= 0) {
        return;
    }
    HwcContext* ctx = contextOf(dev);
    pthread_mutex_lock(&ctx->lock);
    const bool vsync = ctx->vsyncEnabled;
    pthread_mutex_unlock(&ctx->lock);
    snprintf(buff, size_t(buffLen),
            "Biscuit headless HWC 1.1: primary %dx%d @ 60 Hz, no scanout, vsync %s\n",
            ctx->width, ctx->height, vsync ? "on" : "off");
}

int hwc_get_display_configs(hwc_composer_device_1_t* /*dev*/, int disp,
        uint32_t* configs, size_t* numConfigs) {
    if (disp != HWC_DISPLAY_PRIMARY) {
        return -EINVAL;  // external display: not connected
    }
    if (configs == NULL || numConfigs == NULL) {
        return -EINVAL;
    }
    if (*numConfigs >= 1) {
        configs[0] = 0;
    }
    *numConfigs = 1;
    return 0;
}

int hwc_get_display_attributes(hwc_composer_device_1_t* dev, int disp, uint32_t config,
        const uint32_t* attributes, int32_t* values) {
    if (disp != HWC_DISPLAY_PRIMARY || config != 0) {
        return -EINVAL;
    }
    if (attributes == NULL || values == NULL) {
        return -EINVAL;
    }
    HwcContext* ctx = contextOf(dev);
    for (size_t i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        switch (attributes[i]) {
            case HWC_DISPLAY_VSYNC_PERIOD:
                values[i] = int32_t(kVsyncPeriodNs);
                break;
            case HWC_DISPLAY_WIDTH:
                values[i] = ctx->width;
                break;
            case HWC_DISPLAY_HEIGHT:
                values[i] = ctx->height;
                break;
            case HWC_DISPLAY_DPI_X:
            case HWC_DISPLAY_DPI_Y:
                values[i] = kDpiMilli;
                break;
            default:
                // Like a pre-1.5 HWC: SurfaceFlinger retries without
                // HWC_DISPLAY_COLOR_TRANSFORM.
                ALOGV("unsupported display attribute %u", attributes[i]);
                return -EINVAL;
        }
    }
    return 0;
}

int hwc_device_close(hw_device_t* device) {
    HwcContext* ctx = reinterpret_cast<HwcContext*>(device);
    if (ctx == NULL) {
        return 0;
    }
    const uint64_t one = 1;
    if (write(ctx->exitFd, &one, sizeof(one)) != ssize_t(sizeof(one))) {
        ALOGE("failed to signal vsync thread: %s", strerror(errno));
    }
    pthread_join(ctx->thread, NULL);
    close(ctx->timerFd);
    close(ctx->exitFd);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
    return 0;
}

int hwc_device_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (name == NULL || strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
        return -EINVAL;
    }

    HwcContext* ctx = static_cast<HwcContext*>(calloc(1, sizeof(HwcContext)));
    if (ctx == NULL) {
        return -ENOMEM;
    }
    loadSize(&ctx->width, &ctx->height);
    pthread_mutex_init(&ctx->lock, NULL);

    ctx->timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    ctx->exitFd = eventfd(0, EFD_CLOEXEC);
    if (ctx->timerFd < 0 || ctx->exitFd < 0) {
        int err = -errno;
        ALOGE("failed to create vsync fds: %s", strerror(errno));
        if (ctx->timerFd >= 0) close(ctx->timerFd);
        if (ctx->exitFd >= 0) close(ctx->exitFd);
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return err;
    }
    if (pthread_create(&ctx->thread, NULL, vsyncThread, ctx) != 0) {
        ALOGE("failed to start vsync thread");
        close(ctx->timerFd);
        close(ctx->exitFd);
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return -ENOMEM;
    }

    ctx->device.common.tag = HARDWARE_DEVICE_TAG;
    ctx->device.common.version = HWC_DEVICE_API_VERSION_1_1;
    ctx->device.common.module = const_cast<hw_module_t*>(module);
    ctx->device.common.close = hwc_device_close;
    ctx->device.prepare = hwc_prepare;
    ctx->device.set = hwc_set;
    ctx->device.eventControl = hwc_event_control;
    ctx->device.blank = hwc_blank;
    ctx->device.query = hwc_query;
    ctx->device.registerProcs = hwc_register_procs;
    ctx->device.dump = hwc_dump;
    ctx->device.getDisplayConfigs = hwc_get_display_configs;
    ctx->device.getDisplayAttributes = hwc_get_display_attributes;

    ALOGI("headless HWC 1.1, primary display %dx%d", ctx->width, ctx->height);
    *device = &ctx->device.common;
    return 0;
}

hw_module_methods_t hwc_module_methods = {
    hwc_device_open
};

}  // namespace

extern "C" {
hwc_module_t HAL_MODULE_INFO_SYM __attribute__((visibility("default"))) = {
    {
        HARDWARE_MODULE_TAG,           // tag
        HWC_MODULE_API_VERSION_0_1,    // module_api_version
        HARDWARE_HAL_API_VERSION,      // hal_api_version
        HWC_HARDWARE_MODULE_ID,        // id
        "Biscuit headless hwcomposer", // name
        "Biscuit",                     // author
        &hwc_module_methods,           // methods
        NULL,                          // dso
        {0}                            // reserved
    }
};
}
