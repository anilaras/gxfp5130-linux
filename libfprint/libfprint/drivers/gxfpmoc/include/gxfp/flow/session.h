#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gxfp/algo/image/decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

enum gxfp_session_state {
    GXFP_SESSION_STATE_INACTIVE = 0,
    GXFP_SESSION_STATE_IDLE,
    GXFP_SESSION_STATE_AWAIT_FINGER_ON,
    GXFP_SESSION_STATE_CAPTURE,
    GXFP_SESSION_STATE_AWAIT_FINGER_OFF,
};

enum gxfp_session_error_target {
    GXFP_SESSION_ERR_TARGET_GENERIC = 0,
    GXFP_SESSION_ERR_TARGET_ACTIVATE,
    GXFP_SESSION_ERR_TARGET_CAPTURE,
};

struct gxfp_session_events {
    int activate_complete;
    int deactivate_complete;
    int close_complete;
    int session_error;
    int finger_status_changed;
    int finger_present;
    int image_ready;

    int request_tick;
    int tick_delay_ms;
    int cancel_tick;

    int error_code;
    enum gxfp_session_error_target error_target;
    char error_msg[192];
};

struct gxfp_session {
    void *impl;
};

enum gxfp_session_io_condition {
    GXFP_SESSION_IO_IN = 1u << 0,
    GXFP_SESSION_IO_ERR = 1u << 3,
    GXFP_SESSION_IO_HUP = 1u << 4,
    GXFP_SESSION_IO_NVAL = 1u << 5,
};

void gxfp_session_events_clear(struct gxfp_session_events *ev);

void gxfp_session_init(struct gxfp_session *s);
void gxfp_session_dispose(struct gxfp_session *s);

int gxfp_session_open(struct gxfp_session *s,
                      const char *dev_path,
                      const uint8_t *psk,
                      size_t psk_len,
                      int enable_log,
                      char *errbuf,
                      size_t errbuf_len);

void gxfp_session_request_close(struct gxfp_session *s,
                                int reset_mcu,
                                struct gxfp_session_events *ev);

void gxfp_session_request_deactivate(struct gxfp_session *s,
                                     struct gxfp_session_events *ev);

int gxfp_session_activate(struct gxfp_session *s,
                          int enable_log,
                          struct gxfp_session_events *ev);

void gxfp_session_change_state(struct gxfp_session *s,
                               enum gxfp_session_state state,
                               struct gxfp_session_events *ev);

void gxfp_session_on_fd(struct gxfp_session *s,
                        uint32_t condition,
                        int action_cancelled,
                        struct gxfp_session_events *ev);

void gxfp_session_pump(struct gxfp_session *s,
                       int action_cancelled,
                       struct gxfp_session_events *ev);

int gxfp_session_take_image(struct gxfp_session *s, struct gxfp_decoded_image *out);
/* Deliver captures without dark-frame (FPN) correction, for matchers that do
 * their own calibration. */
void gxfp_session_set_raw_output(struct gxfp_session *s, int enable);
/* Copy the no-finger frame captured during the last activation. Returns the
 * pixel count, -ENOENT when none is loaded, -EMSGSIZE when out is too small. */
int gxfp_session_get_dark(const struct gxfp_session *s, uint16_t *out,
                          size_t out_count, int *rows, int *cols);
/* In raw-output mode activation takes two no-finger frames ~0.3 s apart.
 * Reports how much the second differs from the first (mean and standard
 * deviation of the pixel difference); -ENOENT when no pair was taken. */
int gxfp_session_get_dark_stability(const struct gxfp_session *s,
                                    double *mean_diff, double *std_diff);
/* Raw-output mode: a known-rested frame to compare the first no-finger frame
 * against. When they agree within the limits the second frame is skipped and
 * the stability figures describe the difference to this reference. NULL clears. */
void gxfp_session_set_dark_reference(struct gxfp_session *s, const uint16_t *ref,
                                     size_t count, double max_mean, double max_std);
int gxfp_session_poll_readable(const struct gxfp_session *s, int timeout_ms);

#ifdef __cplusplus
}
#endif
