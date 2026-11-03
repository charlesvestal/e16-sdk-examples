#ifndef RT_API_CTL_H
#define RT_API_CTL_H
#include "lua.h"
#include "rt_scene.h"
#include "rt_port.h"

/* Registers the controller global (getPage/set/setByIndex/setControls/get/
 * setHoldTime) into L and resets all per-script state for the new scene:
 * current page = 1, hold disarmed, every encoder's press state cleared.
 * Also loads the scene's wiring (pages/pad_pages/settings_page/assigned),
 * same as rt_api_ctl_set_wiring. */
void rt_api_ctl_open(lua_State *L, const rt_scene_t *scene);

/* Called from rt_unload(): the script's lua_State is gone, so resets the
 * same per-script state rt_api_ctl_open() resets (current page, hold
 * arming, press state), leaving the wiring (scene state, not script
 * state) alone. */
void rt_api_ctl_close(void);

/* Updates the wiring (pages/pad_pages/settings_page/assigned) without
 * touching the current page, hold arming or press state. Lets a host
 * harness change the scene under an already-loaded script (see
 * test/host/rt_module.c). */
void rt_api_ctl_set_wiring(const rt_scene_t *scene);

/* The ids encoder `enc` (1-16) turns and pushes on `page` under the
 * current wiring (the last rt_api_ctl_open()/rt_api_ctl_set_wiring()):
 * pad pages turn enc and push 16+enc, the settings page turns 32+enc and
 * pushes 48+enc; both 0 if the page isn't wired. Whether a script
 * declared the id (scene->assigned) is a separate check. The screen uses
 * this for its default labels. */
void rt_ctl_ids(int page, int enc, int *turn_id, int *push_id);

/* Checks whether any currently-down press is due for its hold callback,
 * and fires it at most once per press, against controller.setHoldTime's
 * *current* (live) value -- setHoldTime(0) disarms a press already held
 * down, matching the mock. Within one call, though, that value is read
 * once and used for every press checked in the call, so an
 * onEncoderHold's own setHoldTime() can't retroactively change the
 * threshold a sibling press in the same pass is evaluated against (see
 * the comment above rt_api_ctl_tick() in rt_api_ctl.c). Called from
 * rt_tick(). A no-op while no script is loaded. */
void rt_api_ctl_tick(rt_time_t now);

/* Time at which a hold could next fire (RT_TIME_NEVER if none is armed/pending),
 * so a host harness's advance() can step finely enough to land on it. */
rt_time_t rt_api_ctl_next_due(void);

/* controller.setHoldTime's current value (0 = disarmed), for a host
 * harness (the mock's M.hold). */
float rt_api_ctl_hold_ms(void);

#endif
