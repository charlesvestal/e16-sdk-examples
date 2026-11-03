#ifndef TUSB_CONFIG_APP_H
#define TUSB_CONFIG_APP_H

/*
 * TinyUSB's classes for the launcher's USB functions (usb_functions.h),
 * included by the SDK's tusb_config.h. The settings are the ones in the
 * SDK examples' own tusb_config_app.h files (usb_disk, usb_tone, usb_cam).
 */

/* The drive: usb_msc.c. */
#define CFG_TUD_MSC                             1
#define CFG_TUD_MSC_EP_BUFSIZE                  512

/* The audio input: usb_audio.c. */
#define CFG_TUD_AUDIO                           1
#define CFG_TUD_AUDIO_ENABLE_EP_IN              1
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL        0
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       196
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    (2 * 196)

/* The camera: usb_video.c. */
#define CFG_TUD_VIDEO                           1
#define CFG_TUD_VIDEO_STREAMING                 1
#define CFG_TUD_VIDEO_STREAMING_BULK            0
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE      928

#endif
