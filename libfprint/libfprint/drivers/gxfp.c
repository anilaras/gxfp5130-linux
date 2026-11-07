#define FP_COMPONENT "gxfp"

#include "drivers_api.h"

#include <errno.h>
#include <gio/gio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gxfp/algo/common.h"
#include "gxfp/flow/session.h"

#include "goodix-chicago-calibration.h"
#include "goodix-chicago-enrollment.h"
#include "goodix-chicago-preprocess.h"
#include "goodix-chicago-runtime.h"
#include "goodix-chicago-template.h"

/*
 * GXFP5130 driver using the native Goodix Chicago matcher.
 *
 * The sensor is a ChicagoHS part: its Windows production calibration
 * (goodix_calib.dat) has exactly the layout the Chicago backend consumes, and
 * the raw 80x64 frames from the eSPI transport line up pixel for pixel with
 * the calibration's offset plane. Matching therefore runs on raw frames with
 * the Windows gain/offset calibration instead of SIGFM on dark-subtracted
 * images, which proved unreliable: the sensor keeps a latent print for a while
 * after each touch, so per-session dark frames often carry the last finger.
 *
 * Every action runs in its own thread with blocking session I/O, mirroring the
 * libfprint-goodix-spi GDIX51C0 driver this backend comes from; results are
 * handed back to the action's main context.
 */

#define GXFP_DEFAULT_PSK_PATH   "/var/lib/fprintd/gxfp/psk_raw32.bin"
#define GXFP_DEFAULT_CALIB_PATH "/var/lib/fprintd/gxfp/goodix_calib.dat"
#define GXFP_T0_CACHE_PATH      "/var/lib/fprint/gxfp/t0.bin"
#define GXFP_T0_CACHE_MAGIC     "GXT0"

#define GXFP_NPIX GOODIX_CHICAGO_PIXELS

/* Activation may wait for a finger to lift before taking its no-finger frame. */
#define GXFP_ACTIVATE_TIMEOUT_MS   60000
#define GXFP_CAPTURE_TIMEOUT_MS    5000
#define GXFP_LIFT_TIMEOUT_MS       30000
#define GXFP_DEACTIVATE_TIMEOUT_MS 3000

/*
 * A no-finger frame is a usable T0 only if the sensor has settled. Right after
 * a touch the sensor carries a latent print that fades within seconds: two
 * frames 0.3 s apart then differ by std 15-46 with a drifting mean, while a
 * rested sensor repeats itself to std ~6. Dirt on the sensor is stable, so it
 * passes and gets cancelled by T0 like it does on Windows. A frame as dark as
 * a resting finger (~1700 below the factory plane) is never taken.
 */
#define GXFP_T0_MAX_PAIR_MEAN   25.0
#define GXFP_T0_MAX_PAIR_STD    10.0
#define GXFP_T0_MIN_FACTORY_DIFF (-1400.0)

/*
 * Windows' OnRetryCaptureIMG re-captures a press that did not match while the
 * finger stays down. On this sensor that never turned a miss into a match in
 * testing: on a moist or oily finger the ridge signal only fades further as
 * the press goes on (std 38 -> 13). Each extra frame is also another chance
 * for an impostor, so only one frame per press is used. A frame still counts
 * as "finger down" while it is at least this much darker than T0.
 */
#define GXFP_CAPTURE_ATTEMPTS      1
#define GXFP_FINGER_MIN_MEAN_DROP  800.0

/*
 * Ridge-band strength (std of a sigma 1 - sigma 4 difference of Gaussians on
 * T0 - raw). Clean presses measure ~100-220; a smudged sensor or wet finger
 * fills the valleys and drops it to ~13-45, at the noise floor. Below this
 * the frame carries no usable print: ask for a new press instead of spending
 * a match attempt on it.
 */
#define GXFP_MIN_RIDGE_STRENGTH    25.0

typedef enum {
  GXFP_ACTION_ENROLL,
  GXFP_ACTION_VERIFY,
  GXFP_ACTION_IDENTIFY,
} GxfpActionKind;

struct _FpiDeviceGxfp
{
  FpDevice              parent;

  struct gxfp_session   sess;
  gboolean              session_open;

  GBytes               *calibration;
  guint8                sensor_id[GOODIX_CHICAGO_SENSOR_ID_LEN];
  guint16               offset_plane[GXFP_NPIX];
  guint16               t0[GXFP_NPIX];
  gboolean              t0_valid;

  GMainContext         *action_context;
  GCancellable         *action_cancellable;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGxfp, fpi_device_gxfp, FPI, DEVICE_GXFP, FpDevice)
G_DEFINE_TYPE (FpiDeviceGxfp, fpi_device_gxfp, FP_TYPE_DEVICE)

/* ------------------------------------------------------------------ */
/* Main-context reporting                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
  FpDevice           *dev;
  GxfpActionKind      kind;
  FpFingerStatusFlags finger_status;
  gint                stage;
  FpPrint            *print;
  GVariant           *data;
  FpiMatchResult      verify_result;
  FpPrint            *identify_match;
  gboolean            complete;
  GError             *error;
} GxfpEvent;

static void
gxfp_event_free (GxfpEvent *event)
{
  g_object_unref (event->dev);
  g_clear_object (&event->print);
  g_clear_object (&event->identify_match);
  g_clear_pointer (&event->data, g_variant_unref);
  g_clear_error (&event->error);
  g_free (event);
}

static void
gxfp_invoke (FpiDeviceGxfp *self, GSourceFunc func, GxfpEvent *event)
{
  g_main_context_invoke_full (self->action_context, G_PRIORITY_DEFAULT, func, event,
                              (GDestroyNotify) gxfp_event_free);
}

static GxfpEvent *
gxfp_event_new (FpiDeviceGxfp *self)
{
  GxfpEvent *event = g_new0 (GxfpEvent, 1);

  event->dev = g_object_ref (FP_DEVICE (self));
  return event;
}

static gboolean
gxfp_finger_status_main (gpointer user_data)
{
  GxfpEvent *event = user_data;

  fpi_device_report_finger_status (event->dev, event->finger_status);
  return G_SOURCE_REMOVE;
}

static void
gxfp_report_finger_status (FpiDeviceGxfp *self, FpFingerStatusFlags status)
{
  GxfpEvent *event = gxfp_event_new (self);

  event->finger_status = status;
  gxfp_invoke (self, gxfp_finger_status_main, event);
}

static gboolean
gxfp_enroll_progress_main (gpointer user_data)
{
  GxfpEvent *event = user_data;

  fpi_device_enroll_progress (event->dev, event->stage, NULL, g_steal_pointer (&event->error));
  return G_SOURCE_REMOVE;
}

static void
gxfp_enroll_progress (FpiDeviceGxfp *self, gint stage, GError *retry)
{
  GxfpEvent *event = gxfp_event_new (self);

  event->stage = stage;
  event->error = retry;
  gxfp_invoke (self, gxfp_enroll_progress_main, event);
}

static gboolean
gxfp_enroll_complete_main (gpointer user_data)
{
  GxfpEvent *event = user_data;

  if (event->error)
    {
      fpi_device_enroll_complete (event->dev, NULL, g_steal_pointer (&event->error));
      return G_SOURCE_REMOVE;
    }

  fpi_print_set_type (event->print, FPI_PRINT_RAW);
  g_object_set (G_OBJECT (event->print), "fpi-data", event->data, NULL);
  fpi_device_enroll_complete (event->dev, g_object_ref (event->print), NULL);
  return G_SOURCE_REMOVE;
}

static void
gxfp_enroll_complete (FpiDeviceGxfp *self, FpPrint *print, GVariant *data, GError *error)
{
  GxfpEvent *event = gxfp_event_new (self);

  event->print = print ? g_object_ref (print) : NULL;
  event->data = data;
  event->error = error;
  gxfp_invoke (self, gxfp_enroll_complete_main, event);
}

/* Reports a result (unless error is a hard failure) and completes the action. */
static gboolean
gxfp_match_complete_main (gpointer user_data)
{
  GxfpEvent *event = user_data;
  GError *error = g_steal_pointer (&event->error);
  gboolean retry = error && error->domain == FP_DEVICE_RETRY;

  /* Before the report: fprintd persists the matched print from its match
   * callback when the serialized data changed. */
  if (event->print && event->data)
    g_object_set (G_OBJECT (event->print), "fpi-data", event->data, NULL);

  if (event->kind == GXFP_ACTION_VERIFY)
    {
      if (!error || retry)
        fpi_device_verify_report (event->dev, retry ? FPI_MATCH_ERROR : event->verify_result,
                                  NULL, retry ? error : NULL);
      fpi_device_verify_complete (event->dev, retry ? NULL : error);
    }
  else
    {
      if (!error || retry)
        fpi_device_identify_report (event->dev, event->identify_match, NULL, retry ? error : NULL);
      fpi_device_identify_complete (event->dev, retry ? NULL : error);
    }
  return G_SOURCE_REMOVE;
}

static void
gxfp_match_complete (FpiDeviceGxfp *self, GxfpActionKind kind, FpiMatchResult verify_result,
                     FpPrint *identify_match, FpPrint *updated_print, GVariant *updated_data,
                     GError *error)
{
  GxfpEvent *event = gxfp_event_new (self);

  event->print = updated_print && updated_data ? g_object_ref (updated_print) : NULL;
  event->data = event->print ? g_variant_ref (updated_data) : NULL;
  event->kind = kind;
  event->verify_result = verify_result;
  event->identify_match = identify_match ? g_object_ref (identify_match) : NULL;
  event->error = error;
  gxfp_invoke (self, gxfp_match_complete_main, event);
}

/* ------------------------------------------------------------------ */
/* Blocking session I/O (action thread only)                           */
/* ------------------------------------------------------------------ */

typedef gboolean (*GxfpDoneFunc) (const struct gxfp_session_events *ev);

static gboolean
done_activate (const struct gxfp_session_events *ev)
{
  return ev->activate_complete;
}

static gboolean
done_deactivate (const struct gxfp_session_events *ev)
{
  return ev->deactivate_complete;
}

static gboolean
done_finger_on (const struct gxfp_session_events *ev)
{
  return ev->finger_status_changed && ev->finger_present;
}

static gboolean
done_finger_off (const struct gxfp_session_events *ev)
{
  return ev->finger_status_changed && !ev->finger_present;
}

static gboolean
done_image (const struct gxfp_session_events *ev)
{
  return ev->image_ready;
}

static gboolean
gxfp_check_events (const struct gxfp_session_events *ev, GxfpDoneFunc done,
                   gboolean *finished, GError **error)
{
  if (ev->session_error)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "gxfp: %s",
                   ev->error_msg[0] ? ev->error_msg : "session error");
      return FALSE;
    }
  *finished = done (ev);
  return TRUE;
}

static gboolean
gxfp_wait (FpiDeviceGxfp *self, gint timeout_ms, gboolean cancellable,
           GxfpDoneFunc done, GError **error)
{
  gint64 deadline = timeout_ms > 0 ? g_get_monotonic_time () + (gint64) timeout_ms * 1000 : 0;

  for (;;)
    {
      struct gxfp_session_events ev;
      gboolean finished = FALSE;
      int pr;

      if (cancellable && self->action_cancellable &&
          g_cancellable_set_error_if_cancelled (self->action_cancellable, error))
        return FALSE;

      gxfp_session_events_clear (&ev);
      gxfp_session_pump (&self->sess, 0, &ev);
      if (!gxfp_check_events (&ev, done, &finished, error))
        return FALSE;
      if (finished)
        return TRUE;

      pr = gxfp_session_poll_readable (&self->sess, 50);
      if (pr == 0)
        {
          gxfp_session_events_clear (&ev);
          gxfp_session_on_fd (&self->sess, GXFP_SESSION_IO_IN, 0, &ev);
          if (!gxfp_check_events (&ev, done, &finished, error))
            return FALSE;
          if (finished)
            return TRUE;
        }
      else if (pr != -EAGAIN)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-pr),
                       "gxfp: poll failed: %s", g_strerror (-pr));
          return FALSE;
        }

      if (deadline && g_get_monotonic_time () > deadline)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                               "gxfp: sensor did not respond in time");
          return FALSE;
        }
    }
}

/* Applies an immediate state change and checks its synchronous errors. */
static gboolean
gxfp_change_state (FpiDeviceGxfp *self, enum gxfp_session_state state, GError **error)
{
  struct gxfp_session_events ev;
  gboolean unused;

  gxfp_session_events_clear (&ev);
  gxfp_session_change_state (&self->sess, state, &ev);
  return gxfp_check_events (&ev, done_finger_on, &unused, error);
}

static double
gxfp_t0_diff_stats (FpiDeviceGxfp *self, const guint16 *frame, double *std)
{
  double sum = 0, sum2 = 0;

  for (guint i = 0; i < GXFP_NPIX; i++)
    {
      double d = (double) frame[i] - self->offset_plane[i];

      sum += d;
      sum2 += d * d;
    }
  sum /= GXFP_NPIX;
  *std = sqrt (MAX (sum2 / GXFP_NPIX - sum * sum, 0));
  return sum;
}

static void
gxfp_t0_cache_save (FpiDeviceGxfp *self)
{
  g_autoptr(GByteArray) buf = g_byte_array_new ();
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dir = g_path_get_dirname (GXFP_T0_CACHE_PATH);

  g_byte_array_append (buf, (const guint8 *) GXFP_T0_CACHE_MAGIC, 4);
  for (guint i = 0; i < GXFP_NPIX; i++)
    {
      guint8 le[2] = { self->t0[i] & 0xff, self->t0[i] >> 8 };
      g_byte_array_append (buf, le, 2);
    }
  if (g_mkdir_with_parents (dir, 0700) < 0 ||
      !g_file_set_contents (GXFP_T0_CACHE_PATH, (const gchar *) buf->data, buf->len, &error))
    fp_dbg ("gxfp: could not cache T0: %s", error ? error->message : g_strerror (errno));
}

static void
gxfp_t0_cache_load (FpiDeviceGxfp *self)
{
  g_autofree gchar *contents = NULL;
  gsize len = 0;

  if (!g_file_get_contents (GXFP_T0_CACHE_PATH, &contents, &len, NULL) ||
      len != 4 + GXFP_NPIX * 2 || memcmp (contents, GXFP_T0_CACHE_MAGIC, 4) != 0)
    return;
  for (guint i = 0; i < GXFP_NPIX; i++)
    self->t0[i] = (guint8) contents[4 + 2 * i] | ((guint8) contents[5 + 2 * i] << 8);
  self->t0_valid = TRUE;
  fp_dbg ("gxfp: loaded cached T0");
}

/*
 * Take the activation's no-finger frame as T0 when the sensor has settled,
 * otherwise keep the last good one (cached across restarts), and fall back to
 * the factory offset plane.
 */
static void
gxfp_update_t0 (FpiDeviceGxfp *self)
{
  guint16 dark[GXFP_NPIX];
  int rows = 0, cols = 0;
  double fmean, fstd = 0, pmean = 0, pstd = 0;

  if (gxfp_session_get_dark (&self->sess, dark, GXFP_NPIX, &rows, &cols) != GXFP_NPIX ||
      rows != GOODIX_CHICAGO_IMAGE_HEIGHT || cols != GOODIX_CHICAGO_IMAGE_WIDTH)
    {
      fp_dbg ("gxfp: activation produced no usable no-finger frame");
    }
  else
    {
      gboolean paired = gxfp_session_get_dark_stability (&self->sess, &pmean, &pstd) == 0;

      fmean = gxfp_t0_diff_stats (self, dark, &fstd);
      if (paired && fabs (pmean) <= GXFP_T0_MAX_PAIR_MEAN && pstd <= GXFP_T0_MAX_PAIR_STD &&
          fmean >= GXFP_T0_MIN_FACTORY_DIFF)
        {
          memcpy (self->t0, dark, sizeof (self->t0));
          self->t0_valid = TRUE;
          gxfp_t0_cache_save (self);
          fp_dbg ("gxfp: fresh T0 accepted (pair mean %.0f std %.1f, factory mean %.0f std %.1f)",
                  pmean, pstd, fmean, fstd);
          return;
        }
      fp_dbg ("gxfp: fresh T0 rejected (pair %s mean %.0f std %.1f, factory mean %.0f std %.1f), "
              "keeping %s", paired ? "" : "missing", pmean, pstd, fmean, fstd,
              self->t0_valid ? "previous T0" : "factory offset plane");
    }

  if (!self->t0_valid)
    {
      memcpy (self->t0, self->offset_plane, sizeof (self->t0));
      self->t0_valid = TRUE;
    }
}

static gboolean
gxfp_activate (FpiDeviceGxfp *self, GError **error)
{
  struct gxfp_session_events ev;
  gboolean unused;

  /* A first frame matching the known-rested T0 needs no second frame. */
  if (self->t0_valid)
    gxfp_session_set_dark_reference (&self->sess, self->t0, GXFP_NPIX,
                                     GXFP_T0_MAX_PAIR_MEAN, GXFP_T0_MAX_PAIR_STD);

  gxfp_session_events_clear (&ev);
  if (gxfp_session_activate (&self->sess, g_getenv ("FP_GXFP_LOG") ? 1 : 0, &ev) < 0 ||
      !gxfp_check_events (&ev, done_activate, &unused, error))
    {
      if (error && !*error)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "gxfp: activation failed");
      return FALSE;
    }
  if (!ev.activate_complete &&
      !gxfp_wait (self, GXFP_ACTIVATE_TIMEOUT_MS, TRUE, done_activate, error))
    return FALSE;

  gxfp_update_t0 (self);
  return TRUE;
}

static void
gxfp_deactivate (FpiDeviceGxfp *self)
{
  struct gxfp_session_events ev;
  g_autoptr(GError) error = NULL;

  gxfp_session_events_clear (&ev);
  gxfp_session_request_deactivate (&self->sess, &ev);
  if (!ev.deactivate_complete && !ev.session_error &&
      !gxfp_wait (self, GXFP_DEACTIVATE_TIMEOUT_MS, FALSE, done_deactivate, &error))
    fp_dbg ("gxfp: deactivate: %s", error->message);
}

/* Captures a raw 80x64 frame right away, without waiting for finger-down. */
static guint16 *
gxfp_capture_raw_now (FpiDeviceGxfp *self, GError **error)
{
  struct gxfp_decoded_image img = { 0 };
  g_autofree guint16 *raw = NULL;
  int r;

  if (!gxfp_change_state (self, GXFP_SESSION_STATE_CAPTURE, error) ||
      !gxfp_wait (self, GXFP_CAPTURE_TIMEOUT_MS, FALSE, done_image, error))
    return NULL;

  r = gxfp_session_take_image (&self->sess, &img);
  if (r < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-r),
                   "gxfp: capture image unavailable: %s", g_strerror (-r));
      return NULL;
    }
  if (img.rows != GOODIX_CHICAGO_IMAGE_HEIGHT || img.cols != GOODIX_CHICAGO_IMAGE_WIDTH)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gxfp: unexpected frame size %dx%d", img.cols, img.rows);
      gxfp_decoded_image_free (&img);
      return NULL;
    }
  raw = g_new (guint16, GXFP_NPIX);
  memcpy (raw, img.pixels, GXFP_NPIX * sizeof (guint16));
  gxfp_decoded_image_free (&img);
  return g_steal_pointer (&raw);
}

/* Waits for a finger and returns its raw 80x64 frame. */
static guint16 *
gxfp_capture_raw (FpiDeviceGxfp *self, GError **error)
{
  if (!gxfp_change_state (self, GXFP_SESSION_STATE_AWAIT_FINGER_ON, error))
    return NULL;
  gxfp_report_finger_status (self, FP_FINGER_STATUS_NEEDED);
  if (!gxfp_wait (self, 0, TRUE, done_finger_on, error))
    return NULL;
  gxfp_report_finger_status (self, FP_FINGER_STATUS_NEEDED | FP_FINGER_STATUS_PRESENT);
  return gxfp_capture_raw_now (self, error);
}

static void
gxfp_gaussian_blur (const float *in, float *out, int w, int h, double sigma)
{
  int radius = (int) ceil (sigma * 3);
  g_autofree float *kernel = g_new (float, 2 * radius + 1);
  g_autofree float *tmp = g_new (float, w * h);
  float sum = 0;

  for (int k = -radius; k <= radius; k++)
    sum += kernel[k + radius] = expf (-(float) (k * k) / (float) (2 * sigma * sigma));
  for (int k = 0; k <= 2 * radius; k++)
    kernel[k] /= sum;

  /* Separable, borders clamped. */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        float acc = 0;
        for (int k = -radius; k <= radius; k++)
          acc += kernel[k + radius] * in[y * w + CLAMP (x + k, 0, w - 1)];
        tmp[y * w + x] = acc;
      }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        float acc = 0;
        for (int k = -radius; k <= radius; k++)
          acc += kernel[k + radius] * tmp[CLAMP (y + k, 0, h - 1) * w + x];
        out[y * w + x] = acc;
      }
}

static double
gxfp_ridge_strength (FpiDeviceGxfp *self, const guint16 *raw)
{
  const int x0 = 4, y0 = 4;
  const int w = GOODIX_CHICAGO_IMAGE_WIDTH - 2 * x0, h = GOODIX_CHICAGO_IMAGE_HEIGHT - 2 * y0;
  g_autofree float *d = g_new (float, w * h);
  g_autofree float *fine = g_new (float, w * h);
  g_autofree float *coarse = g_new (float, w * h);
  double sum = 0, sum2 = 0;

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        int i = (y + y0) * GOODIX_CHICAGO_IMAGE_WIDTH + x + x0;
        d[y * w + x] = (float) self->t0[i] - raw[i];
      }
  gxfp_gaussian_blur (d, fine, w, h, 1.0);
  gxfp_gaussian_blur (d, coarse, w, h, 4.0);
  for (int i = 0; i < w * h; i++)
    {
      double v = fine[i] - coarse[i];
      sum += v;
      sum2 += v * v;
    }
  sum /= w * h;
  return sqrt (MAX (sum2 / (w * h) - sum * sum, 0));
}

/* Chicago probe for a raw frame, rejecting frames too faint to hold a print. */
static GoodixChicagoRuntimeProbe *
gxfp_prepare_probe (FpiDeviceGxfp *self, GoodixChicagoPreprocessor *pp, const guint16 *raw,
                    GoodixChicagoRuntimeReject *reject, GError **error)
{
  double ridge = gxfp_ridge_strength (self, raw);

  if (ridge < GXFP_MIN_RIDGE_STRENGTH)
    {
      fp_dbg ("gxfp: frame too faint (ridge strength %.1f)", ridge);
      *reject = GOODIX_CHICAGO_RUNTIME_REJECT_POOR_CAPTURE;
      return NULL;
    }
  fp_dbg ("gxfp: ridge strength %.1f", ridge);
  return goodix_chicago_runtime_prepare_probe (pp, raw, reject, error);
}

static gboolean
gxfp_frame_has_finger (FpiDeviceGxfp *self, const guint16 *raw)
{
  double drop = 0;

  for (guint i = 0; i < GXFP_NPIX; i++)
    drop += (double) self->t0[i] - raw[i];
  return drop / GXFP_NPIX >= GXFP_FINGER_MIN_MEAN_DROP;
}

static gboolean
gxfp_wait_lift (FpiDeviceGxfp *self, GError **error)
{
  if (!gxfp_change_state (self, GXFP_SESSION_STATE_AWAIT_FINGER_OFF, error) ||
      !gxfp_wait (self, GXFP_LIFT_TIMEOUT_MS, TRUE, done_finger_off, error))
    return FALSE;
  gxfp_report_finger_status (self, FP_FINGER_STATUS_NONE);
  return TRUE;
}

static GoodixChicagoRuntimeProbe *
gxfp_capture_probe (FpiDeviceGxfp *self, GoodixChicagoPreprocessor *pp,
                    GoodixChicagoRuntimeReject *reject, GError **error)
{
  g_autofree guint16 *raw = gxfp_capture_raw (self, error);

  if (!raw)
    return NULL;
  return gxfp_prepare_probe (self, pp, raw, reject, error);
}

/* Re-captures the current press. Returns NULL with *lifted set (and no error)
 * once the finger has left the sensor. */
static GoodixChicagoRuntimeProbe *
gxfp_recapture_probe (FpiDeviceGxfp *self, GoodixChicagoPreprocessor *pp,
                      GoodixChicagoRuntimeReject *reject, gboolean *lifted, GError **error)
{
  g_autofree guint16 *raw = gxfp_capture_raw_now (self, error);

  *lifted = FALSE;
  if (!raw)
    return NULL;
  if (!gxfp_frame_has_finger (self, raw))
    {
      *lifted = TRUE;
      return NULL;
    }
  return gxfp_prepare_probe (self, pp, raw, reject, error);
}

static GError *
gxfp_reject_retry (GoodixChicagoRuntimeReject reject)
{
  if (reject == GOODIX_CHICAGO_RUNTIME_REJECT_NO_FEATURES)
    return fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER,
                                     "No fingerprint features found; cover the sensor fully.");
  return fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                   "Fingerprint image is too faint or smudged; "
                                   "wipe the sensor and your finger, then try again.");
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void
gxfp_enroll_sync (FpiDeviceGxfp *self, FpPrint *print)
{
  g_autoptr(GoodixChicagoEnrollment) enrollment = goodix_chicago_enrollment_new ();
  g_autoptr(GoodixChicagoRuntimeProbe) deferred = NULL;
  GoodixChicagoPreprocessor *pp = NULL;
  GoodixChicagoEngineEnrollmentPolicy policy;
  GVariant *data = NULL;
  GError *error = NULL;

  if (!gxfp_activate (self, &error))
    goto out;
  pp = goodix_chicago_preprocessor_new (self->calibration, self->t0, &error);
  if (!pp)
    goto out;

  goodix_chicago_engine_enrollment_policy_init (&policy);
  while (!goodix_chicago_engine_enrollment_policy_complete (&policy))
    {
      g_autoptr(GoodixChicagoRuntimeProbe) probe = NULL;
      const GoodixChicagoSubtemplateView *view;
      GoodixChicagoRuntimeReject reject = GOODIX_CHICAGO_RUNTIME_REJECT_NONE;
      GoodixChicagoEnrollmentResult result;
      guint position_reject = 0;
      gboolean ok, accepted;

      probe = gxfp_capture_probe (self, pp, &reject, &error);
      if (!probe)
        {
          if (error)
            goto out;
          gxfp_enroll_progress (self, policy.used, gxfp_reject_retry (reject));
          if (!gxfp_wait_lift (self, &error))
            goto out;
          continue;
        }

      view = goodix_chicago_runtime_probe_get_view (probe);
      if (goodix_chicago_enrollment_get_count (enrollment) == 0)
        ok = goodix_chicago_enrollment_insert_first (enrollment, view->records, view->record_count,
                                                     view->active_count, view->quality, view->coverage,
                                                     view->metric_data, &result, &error);
      else
        ok = goodix_chicago_enrollment_insert_next (enrollment, view->records, view->record_count,
                                                    view->active_count, view->quality, view->coverage,
                                                    view->metric_data, &result, &error);
      if (!ok)
        goto out;

      accepted = goodix_chicago_engine_enrollment_policy_accept (&policy, result.position_x,
                                                                  result.position_y, &position_reject);

      /* The engine may park a sample and re-insert it later; see the GDIX51C0 driver. */
      if (policy.defer_current_sample)
        {
          if (deferred || !goodix_chicago_enrollment_drop_last (enrollment, &error))
            {
              if (!error)
                g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "gxfp: duplicate deferred enrollment sample");
              goto out;
            }
          deferred = g_steal_pointer (&probe);
        }
      if (policy.restore_deferred_sample)
        {
          const GoodixChicagoSubtemplateView *dv;
          GoodixChicagoEnrollmentResult dres;

          if (!deferred)
            {
              g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "gxfp: missing deferred enrollment sample");
              goto out;
            }
          dv = goodix_chicago_runtime_probe_get_view (deferred);
          if (!goodix_chicago_enrollment_insert_next (enrollment, dv->records, dv->record_count,
                                                      dv->active_count, dv->quality, dv->coverage,
                                                      dv->metric_data, &dres, &error))
            goto out;
          g_clear_pointer (&deferred, goodix_chicago_runtime_probe_free);
        }

      if (!accepted)
        {
          const gchar *msg = position_reject == 1 ? "Finger is too high; move it lower." :
                             position_reject == 2 ? "Finger is too low; move it higher." :
                             position_reject == 3 ? "Finger is too far left; move it right." :
                             "Finger is too far right; move it left.";

          gxfp_enroll_progress (self, policy.used,
                                fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER, "%s", msg));
        }
      else
        {
          gxfp_enroll_progress (self, policy.used, NULL);
        }

      fp_dbg ("gxfp: enroll sample %u/%u position %u,%u reject %u",
              policy.used, GOODIX_CHICAGO_ENGINE_REQUIRED_SAMPLES,
              result.position_x, result.position_y, position_reject);

      if (!gxfp_wait_lift (self, &error))
        goto out;
    }

  {
    g_autoptr(GBytes) packed = goodix_chicago_enrollment_pack (enrollment, &error);

    if (!packed)
      goto out;
    data = g_variant_ref_sink (goodix_chicago_print_data_build (self->sensor_id, self->calibration,
                                                                packed));
  }
  fp_dbg ("gxfp: enrollment complete with %u subtemplates",
          goodix_chicago_enrollment_get_count (enrollment));

out:
  g_clear_pointer (&pp, goodix_chicago_preprocessor_free);
  gxfp_deactivate (self);
  gxfp_enroll_complete (self, print, data, error);
}

static gint32
gxfp_score (FpiDeviceGxfp *self, const GoodixChicagoRuntimeProbe *probe, FpPrint *print,
            GoodixChicagoMatchTemplateResult *result, GError **error)
{
  g_autoptr(GVariant) data = NULL;

  memset (result, 0, sizeof (*result));
  g_object_get (G_OBJECT (print), "fpi-data", &data, NULL);
  if (!data)
    return -1;
  if (!goodix_chicago_runtime_match_print_data (probe, data, self->sensor_id, self->calibration,
                                                result, error))
    return G_MININT32;
  return result->score;
}

/*
 * templateStudy: let Chicago fold the matched press into the print, as the
 * Windows driver does after every successful match. The engine admits only
 * strong, study-eligible matches. Returns the new print data or NULL.
 */
static GVariant *
gxfp_study (FpiDeviceGxfp *self, const GoodixChicagoRuntimeProbe *probe, FpPrint *print,
            const GoodixChicagoMatchTemplateResult *result)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GError) error = NULL;
  GVariant *updated = NULL;

  g_object_get (G_OBJECT (print), "fpi-data", &data, NULL);
  if (!data)
    return NULL;
  if (!goodix_chicago_runtime_study_print_data (probe, data, self->sensor_id, self->calibration,
                                                result, &updated, &error))
    {
      fp_warn ("gxfp: templateStudy failed: %s", error->message);
      return NULL;
    }
  fp_dbg ("gxfp: templateStudy %s", updated ? "updated the print" : "not eligible");
  return updated ? g_variant_ref_sink (updated) : NULL;
}

static void
gxfp_match_sync (FpiDeviceGxfp *self, GxfpActionKind kind, FpPrint *verify_print,
                 GPtrArray *gallery)
{
  g_autoptr(GoodixChicagoRuntimeProbe) probe = NULL;
  GoodixChicagoPreprocessor *pp = NULL;
  GoodixChicagoRuntimeReject reject = GOODIX_CHICAGO_RUNTIME_REJECT_NONE;
  FpiMatchResult verify_result = FPI_MATCH_FAIL;
  FpPrint *match = NULL;
  g_autoptr(GVariant) updated_data = NULL;
  GError *error = NULL;
  gboolean captured = FALSE;
  gboolean saw_valid = FALSE;

  if (!gxfp_activate (self, &error))
    goto out;
  pp = goodix_chicago_preprocessor_new (self->calibration, self->t0, &error);
  if (!pp)
    goto out;

  for (guint attempt = 1; attempt <= GXFP_CAPTURE_ATTEMPTS; attempt++)
    {
      gboolean lifted = FALSE;

      g_clear_pointer (&probe, goodix_chicago_runtime_probe_free);
      if (attempt == 1)
        probe = gxfp_capture_probe (self, pp, &reject, &error);
      else
        probe = gxfp_recapture_probe (self, pp, &reject, &lifted, &error);
      if (error)
        goto out;
      captured = TRUE;
      if (lifted)
        {
          fp_dbg ("gxfp: finger lifted before capture %u", attempt);
          break;
        }
      if (!probe)
        {
          fp_dbg ("gxfp: capture %u rejected (reason %d)", attempt, reject);
          continue;
        }
      saw_valid = TRUE;

      if (kind == GXFP_ACTION_VERIFY)
        {
          GoodixChicagoMatchTemplateResult result;
          gint32 score = gxfp_score (self, probe, verify_print, &result, &error);

          if (error)
            goto out;
          verify_result = score > 0 ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
          fp_dbg ("gxfp: verify capture %u score %d -> %s", attempt, score,
                  verify_result == FPI_MATCH_SUCCESS ? "match" : "no match");
          if (verify_result == FPI_MATCH_SUCCESS)
            {
              updated_data = gxfp_study (self, probe, verify_print, &result);
              break;
            }
        }
      else
        {
          GoodixChicagoMatchTemplateResult best_result = { 0 };
          gint32 best = 0;

          for (guint i = 0; gallery && i < gallery->len; i++)
            {
              FpPrint *candidate = g_ptr_array_index (gallery, i);
              GoodixChicagoMatchTemplateResult result;
              gint32 score = gxfp_score (self, probe, candidate, &result, NULL);

              fp_dbg ("gxfp: identify capture %u candidate %u score %d", attempt, i, score);
              if (score > best)
                {
                  best = score;
                  best_result = result;
                  match = candidate;
                }
            }
          if (match)
            {
              updated_data = gxfp_study (self, probe, match, &best_result);
              break;
            }
        }
    }

  /* Every frame of the press was unusable: ask for another press. */
  if (!saw_valid && reject != GOODIX_CHICAGO_RUNTIME_REJECT_NONE)
    error = gxfp_reject_retry (reject);

out:
  g_clear_pointer (&pp, goodix_chicago_preprocessor_free);
  /* A match is reported right away; the next activation copes with a finger
   * that is still down. Anything else waits for the lift so that a retry
   * needs a new press instead of re-reading this one. */
  if (captured)
    {
      g_autoptr(GError) lift_error = NULL;

      if (error || (verify_result != FPI_MATCH_SUCCESS && !match))
        {
          if (!gxfp_wait_lift (self, &lift_error))
            fp_dbg ("gxfp: finger lift not seen: %s", lift_error->message);
        }
      else
        {
          gxfp_report_finger_status (self, FP_FINGER_STATUS_NONE);
        }
    }
  gxfp_deactivate (self);
  gxfp_match_complete (self, kind, verify_result, match,
                       kind == GXFP_ACTION_VERIFY ? verify_print : match, updated_data, error);
}

typedef struct
{
  FpiDeviceGxfp *self;
  GxfpActionKind kind;
  FpPrint       *print;
  GPtrArray     *gallery;
} GxfpAction;

static gpointer
gxfp_action_thread (gpointer user_data)
{
  GxfpAction *action = user_data;
  FpiDeviceGxfp *self = action->self;

  if (action->kind == GXFP_ACTION_ENROLL)
    gxfp_enroll_sync (self, action->print);
  else
    gxfp_match_sync (self, action->kind, action->print, action->gallery);

  g_clear_object (&action->print);
  g_clear_pointer (&action->gallery, g_ptr_array_unref);
  g_object_unref (self);
  g_free (action);
  return NULL;
}

static void
gxfp_run_action (FpDevice *dev, GxfpActionKind kind)
{
  FpiDeviceGxfp *self = FPI_DEVICE_GXFP (dev);
  GxfpAction *action = g_new0 (GxfpAction, 1);
  GCancellable *cancellable = fpi_device_get_cancellable (dev);
  GMainContext *context = g_main_context_get_thread_default ();

  action->self = g_object_ref (self);
  action->kind = kind;
  if (kind == GXFP_ACTION_ENROLL)
    {
      FpPrint *print = NULL;

      fpi_device_get_enroll_data (dev, &print);
      action->print = g_object_ref (print);
    }
  else if (kind == GXFP_ACTION_VERIFY)
    {
      FpPrint *print = NULL;

      fpi_device_get_verify_data (dev, &print);
      action->print = g_object_ref (print);
    }
  else
    {
      GPtrArray *gallery = NULL;

      fpi_device_get_identify_data (dev, &gallery);
      action->gallery = g_ptr_array_new_with_free_func (g_object_unref);
      for (guint i = 0; gallery && i < gallery->len; i++)
        g_ptr_array_add (action->gallery, g_object_ref (g_ptr_array_index (gallery, i)));
    }

  g_clear_pointer (&self->action_context, g_main_context_unref);
  g_clear_object (&self->action_cancellable);
  self->action_context = g_main_context_ref (context ? context : g_main_context_default ());
  self->action_cancellable = cancellable ? g_object_ref (cancellable) : NULL;

  g_thread_unref (g_thread_new ("gxfp-action", gxfp_action_thread, action));
}

static void
gxfp_enroll (FpDevice *dev)
{
  gxfp_run_action (dev, GXFP_ACTION_ENROLL);
}

static void
gxfp_verify (FpDevice *dev)
{
  gxfp_run_action (dev, GXFP_ACTION_VERIFY);
}

static void
gxfp_identify (FpDevice *dev)
{
  gxfp_run_action (dev, GXFP_ACTION_IDENTIFY);
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

static gboolean
gxfp_load_calibration (FpiDeviceGxfp *self, GError **error)
{
  const gchar *path = g_getenv ("FP_GXFP_CALIB");
  g_autofree gchar *contents = NULL;
  gsize len = 0;

  if (!path || !path[0])
    path = GXFP_DEFAULT_CALIB_PATH;
  if (!g_file_get_contents (path, &contents, &len, error))
    {
      g_prefix_error (error, "gxfp: copy goodix_calib.dat from Windows "
                      "(C:\\ProgramData\\Goodix) to %s: ", GXFP_DEFAULT_CALIB_PATH);
      return FALSE;
    }
  if (len != GOODIX_CHICAGO_CALIBRATION_FILE_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "gxfp: %s has %zu bytes, expected %u", path, len,
                   (guint) GOODIX_CHICAGO_CALIBRATION_FILE_LEN);
      return FALSE;
    }
  memcpy (self->sensor_id, contents, GOODIX_CHICAGO_SENSOR_ID_LEN);

  g_clear_pointer (&self->calibration, g_bytes_unref);
  self->calibration = goodix_chicago_calibration_load (path, self->sensor_id, error);
  if (!self->calibration)
    return FALSE;

  for (guint i = 0; i < GXFP_NPIX; i++)
    if (!goodix_chicago_calibration_get_corrections (self->calibration, i, NULL,
                                                     &self->offset_plane[i], error))
      return FALSE;
  return TRUE;
}

static void
gxfp_open (FpDevice *dev)
{
  FpiDeviceGxfp *self = FPI_DEVICE_GXFP (dev);
  g_autoptr(GError) error = NULL;
  g_autofree uint8_t *psk = NULL;
  size_t psk_len = 0;
  const gchar *chardev = fpi_device_get_udev_data (dev, FPI_DEVICE_UDEV_SUBTYPE_CHARDEV);
  const gchar *psk_path = g_getenv ("FP_GXFP_PSK");
  char errbuf[256] = { 0 };
  int r;

  if (!psk_path || !psk_path[0])
    psk_path = GXFP_DEFAULT_PSK_PATH;

  if (!chardev || !chardev[0])
    {
      fpi_device_open_complete (dev, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                                          "gxfp: no chardev from udev"));
      return;
    }
  if (!gxfp_load_calibration (self, &error))
    {
      fpi_device_open_complete (dev, g_steal_pointer (&error));
      return;
    }
  if (!self->t0_valid)
    gxfp_t0_cache_load (self);

  r = gxfp_read_file_all (psk_path, &psk, &psk_len);
  if (r < 0)
    {
      fpi_device_open_complete (dev, g_error_new (G_IO_ERROR, g_io_error_from_errno (-r),
                                                  "gxfp: read PSK '%s' failed: %s",
                                                  psk_path, g_strerror (-r)));
      return;
    }

  gxfp_session_init (&self->sess);
  r = gxfp_session_open (&self->sess, chardev, psk, psk_len, g_getenv ("FP_GXFP_LOG") ? 1 : 0,
                         errbuf, sizeof (errbuf));
  if (r < 0)
    {
      gxfp_session_dispose (&self->sess);
      fpi_device_open_complete (dev, g_error_new (G_IO_ERROR, g_io_error_from_errno (-r), "%s",
                                                  errbuf[0] ? errbuf : "gxfp_session_open failed"));
      return;
    }
  gxfp_session_set_raw_output (&self->sess, 1);
  self->session_open = TRUE;
  fpi_device_open_complete (dev, NULL);
}

static void
gxfp_close (FpDevice *dev)
{
  FpiDeviceGxfp *self = FPI_DEVICE_GXFP (dev);

  if (self->session_open)
    {
      struct gxfp_session_events ev;
      gint64 deadline = g_get_monotonic_time () + GXFP_DEACTIVATE_TIMEOUT_MS * 1000;

      gxfp_session_events_clear (&ev);
      gxfp_session_request_close (&self->sess, 0, &ev);
      while (!ev.close_complete && !ev.session_error && g_get_monotonic_time () < deadline)
        {
          gxfp_session_events_clear (&ev);
          gxfp_session_pump (&self->sess, 0, &ev);
          if (!ev.close_complete && gxfp_session_poll_readable (&self->sess, 50) == 0)
            gxfp_session_on_fd (&self->sess, GXFP_SESSION_IO_IN, 0, &ev);
        }
      gxfp_session_dispose (&self->sess);
      self->session_open = FALSE;
    }
  fpi_device_close_complete (dev, NULL);
}

/* ------------------------------------------------------------------ */
/* GObject                                                             */
/* ------------------------------------------------------------------ */

static const FpIdEntry driver_ids[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_CHARDEV, .chardev_acpi_id = "GXFP5130" },
  { .udev_types = 0 },
};

static void
fpi_device_gxfp_init (FpiDeviceGxfp *self)
{
}

static void
fpi_device_gxfp_finalize (GObject *object)
{
  FpiDeviceGxfp *self = FPI_DEVICE_GXFP (object);

  if (self->session_open)
    gxfp_session_dispose (&self->sess);
  g_clear_pointer (&self->calibration, g_bytes_unref);
  g_clear_pointer (&self->action_context, g_main_context_unref);
  g_clear_object (&self->action_cancellable);
  G_OBJECT_CLASS (fpi_device_gxfp_parent_class)->finalize (object);
}

static void
fpi_device_gxfp_class_init (FpiDeviceGxfpClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Goodix GXFP5130 eSPI Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = driver_ids;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = GOODIX_CHICAGO_ENGINE_REQUIRED_SAMPLES;
  dev_class->temp_hot_seconds = -1;
  dev_class->open = gxfp_open;
  dev_class->close = gxfp_close;
  dev_class->enroll = gxfp_enroll;
  dev_class->verify = gxfp_verify;
  dev_class->identify = gxfp_identify;

  fpi_device_class_auto_initialize_features (dev_class);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_gxfp_finalize;
}
