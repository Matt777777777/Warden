/*
  WARDEN - motion features, computed on the fly.

  The SAME file is compiled into the Arduino firmware and into the laptop
  training pipeline (tools/train.py loads it through ctypes), so the model is
  trained on exactly the numbers the crate will compute.

  A window is 1 second (100 samples at 100 Hz). Nothing is buffered: each
  sample only updates running sums, minima and maxima (a cashier's running
  total instead of a pile of receipts), so one window costs ~60 bytes of RAM.
*/
#ifndef WARDEN_FEATURES_H
#define WARDEN_FEATURES_H

#ifdef __cplusplus
extern "C" {
#endif

#define WF_WINDOW 100          /* samples per window (1 s at 100 Hz) */
#define WF_COUNT  17           /* number of features */

typedef struct {
  unsigned char n;             /* samples added so far */
  float m_sum, m_sq, m_min, m_max;          /* |a| statistics */
  float ax_sum, ay_sum, az_sum;             /* orientation (means) */
  float ax_sq, ay_sq, az_sq;                /* per-axis spread */
  float g_sum, g_max;                       /* rotation speed */
  float jerk_sum, jerk_max, m_prev;         /* how abruptly |a| changes */
  unsigned char ff_count, hi_count, sharp_count; /* free fall, hard hits, abrupt changes */
} WardenFeatures;

/* Names, in output order (for reports and documentation). */
extern const char *const WF_NAMES[WF_COUNT];

void wf_reset(WardenFeatures *w);
/* Add one sample: acceleration in g, rotation in deg/s. */
void wf_add(WardenFeatures *w, float ax, float ay, float az, float gx, float gy, float gz);
/* 1 when the window is complete. */
int wf_ready(const WardenFeatures *w);
/* Fill out[WF_COUNT] from the current window. */
void wf_compute(const WardenFeatures *w, float *out);

#ifdef __cplusplus
}
#endif
#endif
