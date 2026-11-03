/*
 * The launcher's USB functions: usb_functions.h.
 *
 * The SDK takes at most two functions (USB_FUNCTIONS_MAX, usb.h), and
 * the launcher has three. The camera needs all the packet memory MIDI
 * leaves (usb_video.h), so it never shares the configuration with the
 * drive or the audio input anyway. So the SDK gets two:
 *
 *   - the drive, which leaves the descriptors while the camera is on;
 *   - "audio or camera": one slot whose descriptor and class driver are
 *     the camera's while it's on and the audio input's otherwise.
 *
 * usb_msc.c, usb_audio.c and usb_video.c are compiled with
 * usb_add_function renamed to launcher_usb_add_function (CMakeLists.txt),
 * so their usb_*_start() hand their usb_function_t to this file instead
 * of the SDK.
 *
 * Whether the camera is on comes from the camera's own descriptor():
 * usb_video.c makes the pending usb_video_select() choice current inside
 * it (it runs while the E16 is detached, see usb.c) and returns 0 without
 * a source, so asking it for its length (NULL buffer) is both up to date
 * and harmless, whichever slot the SDK asks first.
 */
#include "usb_functions.h"

#include <string.h>

#include "usb.h"
#include "usb_audio.h"
#include "usb_msc.h"
#include "usb_video.h"

#include "disk_mode.h"
#include "log.h"
#include "port_e16_usbaudio.h"

bool launcher_usb_add_function(const usb_function_t *f);

static const usb_function_t *g_msc, *g_audio, *g_video;

static bool camera_on(void)
{
    return g_video && g_video->descriptor(NULL, 0) != 0;
}

/* ---- The drive ---- */

static usb_function_t g_msc_slot;

static uint16_t msc_descriptor(uint8_t *desc, uint8_t first_itf)
{
    return camera_on() ? 0 : g_msc->descriptor(desc, first_itf);
}

/* ---- Audio or camera ---- */

/* The driver of the interfaces TinyUSB opened in this configuration, or
 * NULL. Set and cleared in the USB interrupts only. */
static const usbd_class_driver_t *g_owner;

static const usb_function_t *av_current(void)
{
    return camera_on() ? g_video : g_audio;
}

static uint16_t av_descriptor(uint8_t *desc, uint8_t first_itf)
{
    const usb_function_t *f = av_current();
    return f ? f->descriptor(desc, first_itf) : 0;
}

static void av_init(void)
{
    if (g_audio) {
        g_audio->driver->init();
    }
    if (g_video) {
        g_video->driver->init();
    }
}

static bool av_deinit(void)
{
    bool ok = true;
    if (g_audio && g_audio->driver->deinit) {
        ok &= g_audio->driver->deinit();
    }
    if (g_video && g_video->driver->deinit) {
        ok &= g_video->driver->deinit();
    }
    return ok;
}

static void av_reset(uint8_t rhport)
{
    g_owner = NULL;
    if (g_audio) {
        g_audio->driver->reset(rhport);
    }
    if (g_video) {
        g_video->driver->reset(rhport);
    }
}

static uint16_t av_open(uint8_t rhport, const tusb_desc_interface_t *itf, uint16_t max_len)
{
    const usb_function_t *f = itf->bInterfaceClass == TUSB_CLASS_VIDEO ? g_video
                            : itf->bInterfaceClass == TUSB_CLASS_AUDIO ? g_audio
                            : NULL;
    if (!f) {
        return 0;
    }
    uint16_t n = f->driver->open(rhport, itf, max_len);
    if (n) {
        g_owner = f->driver;
    }
    return n;
}

static bool av_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    return g_owner && g_owner->control_xfer_cb(rhport, stage, request);
}

static bool av_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    return g_owner && g_owner->xfer_cb(rhport, ep_addr, result, xferred_bytes);
}

static bool av_xfer_isr(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    return g_owner && g_owner->xfer_isr && g_owner->xfer_isr(rhport, ep_addr, result, xferred_bytes);
}

static void av_sof(uint8_t rhport, uint32_t frame_count)
{
    if (g_owner && g_owner->sof) {
        g_owner->sof(rhport, frame_count);
    }
}

static const usbd_class_driver_t g_av_driver = {
    .name = "AUDIO/VIDEO",
    .init = av_init,
    .deinit = av_deinit,
    .reset = av_reset,
    .open = av_open,
    .control_xfer_cb = av_control_xfer_cb,
    .xfer_cb = av_xfer_cb,
    .xfer_isr = av_xfer_isr,
    .sof = av_sof,
};

static void av_service(void)
{
    if (g_audio && g_audio->service) {
        g_audio->service();
    }
    if (g_video && g_video->service) {
        g_video->service();
    }
}

static void av_mounted(bool mounted)
{
    if (g_audio && g_audio->mounted) {
        g_audio->mounted(mounted);
    }
    if (g_video && g_video->mounted) {
        g_video->mounted(mounted);
    }
}

static usb_function_t g_av_slot = {
    .descriptor = av_descriptor,
    .driver = &g_av_driver,
    .service = av_service,
    .mounted = av_mounted,
};

/* ---- Collecting the functions ---- */

/* usb_msc_start(), usb_audio_start() and usb_video_start() land here. */
bool launcher_usb_add_function(const usb_function_t *f)
{
    const char *name = f->driver->name;
    if (strcmp(name, "MSC") == 0 && !g_msc) {
        g_msc = f;
        g_msc_slot = *f;
        g_msc_slot.descriptor = msc_descriptor;
        return usb_add_function(&g_msc_slot);
    }
    const usb_function_t **slot = strcmp(name, "AUDIO") == 0 ? &g_audio
                                : strcmp(name, "VIDEO") == 0 ? &g_video
                                : NULL;
    if (!slot || *slot) {
        return false;
    }
    /* Both take the slot's interface numbers, so they must agree on how many. */
    const usb_function_t *other = g_audio ? g_audio : g_video;
    if (other && (other->interfaces != f->interfaces || other->iad != f->iad)) {
        return false;
    }
    *slot = f;
    if (other) {
        return true;
    }
    g_av_slot.interfaces = f->interfaces;
    g_av_slot.iad = f->iad;
    if (!usb_add_function(&g_av_slot)) {
        *slot = NULL;
        return false;
    }
    return true;
}

void usb_functions_start(void)
{
    bool disk = disk_mode_usb_start();
    bool audio = port_e16_usbaudio_start();
    bool video = usb_video_start(NULL); /* no camera until webcam mode selects one */
    if (!disk || !audio || !video) {
        log_printf("usb: missing a function:%s%s%s", disk ? "" : " disk", audio ? "" : " audio",
                   video ? "" : " camera");
    }
}
