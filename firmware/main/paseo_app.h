#pragma once

/* Paseo Passport application entry point.
 * Replaces the xiaozhi Application startup: BOOT -> PROVISIONING (no Paseo
 * config in NVS, or OK long-press held during the boot window) or
 * CONNECTING -> paseo_client ONLINE. Never returns. */
void paseo_app_run(void);

/* ---- board key hook ----
 * The AI Passport board's ADC ladder keys (UP/DOWN/OK) normally drive the
 * xiaozhi Application. paseo_app registers a hook before the board is
 * constructed; while a hook is set, the board forwards key events to it
 * instead. Short vs long press is decided by the iot_button framework:
 * for the ADC buttons the base convention is a 2000 ms long-press time
 * (see main/boards/common/button.cc, AdcButton). */
enum {
    PASEO_KEY_UP = 0,
    PASEO_KEY_DOWN = 1,
    PASEO_KEY_OK = 2,
};
enum {
    PASEO_KEY_CLICK = 0,     /* released before the long-press threshold */
    PASEO_KEY_LONG = 1,      /* held past the long-press threshold */
    PASEO_KEY_PRESS_DOWN = 2, /* physical press start (phase 3: hold-to-talk) */
    PASEO_KEY_PRESS_UP = 3,   /* physical press end */
};
typedef void (*paseo_key_hook_t)(int key, int kind);
void paseo_set_key_hook(paseo_key_hook_t hook);
paseo_key_hook_t paseo_get_key_hook(void);
