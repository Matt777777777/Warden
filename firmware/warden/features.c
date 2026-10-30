/* WARDEN - motion features (see features.h). Plain C, no Arduino dependency. */
#include <math.h>
#include "features.h"

#define FREEFALL_G 0.35f       /* |a| below this: the crate is falling */
#define HARD_HIT_G 1.5f        /* |a| above this: a hit, a drop impact, a violent shake */
#define SHARP_G    0.15f       /* |a| changing by more than this in 10 ms: a knock */

const char *const WF_NAMES[WF_COUNT] = {
  "acc_mean", "acc_std", "acc_min", "acc_max",
  "ax_mean", "ay_mean", "az_mean",
  "ax_std", "ay_std", "az_std",
  "gyro_mean", "gyro_max",
  "jerk", "freefall_frac", "hit_frac", "jerk_max", "sharp_frac"
};

void wf_reset(WardenFeatures *w) {
  w->n = 0;
  w->m_sum = w->m_sq = 0.0f;
  w->m_min = 1000.0f;
  w->m_max = 0.0f;
  w->ax_sum = w->ay_sum = w->az_sum = 0.0f;
  w->ax_sq = w->ay_sq = w->az_sq = 0.0f;
  w->g_sum = w->g_max = 0.0f;
  w->jerk_sum = w->jerk_max = 0.0f;
  w->m_prev = 0.0f;
  w->ff_count = w->hi_count = w->sharp_count = 0;
}

void wf_add(WardenFeatures *w, float ax, float ay, float az, float gx, float gy, float gz) {
  float m = sqrtf(ax * ax + ay * ay + az * az);
  float g = sqrtf(gx * gx + gy * gy + gz * gz);
  if (w->n >= WF_WINDOW) return;            /* window full: caller must compute and reset */

  w->m_sum += m;
  w->m_sq += m * m;
  if (m < w->m_min) w->m_min = m;
  if (m > w->m_max) w->m_max = m;

  w->ax_sum += ax;  w->ay_sum += ay;  w->az_sum += az;
  w->ax_sq += ax * ax;  w->ay_sq += ay * ay;  w->az_sq += az * az;

  w->g_sum += g;
  if (g > w->g_max) w->g_max = g;

  if (w->n > 0) {
    float d = fabsf(m - w->m_prev);
    w->jerk_sum += d;
    if (d > w->jerk_max) w->jerk_max = d;
    if (d > SHARP_G) w->sharp_count++;
  }
  w->m_prev = m;

  if (m < FREEFALL_G) w->ff_count++;
  if (m > HARD_HIT_G) w->hi_count++;
  w->n++;
}

int wf_ready(const WardenFeatures *w) { return w->n >= WF_WINDOW; }

static float spread(float sum, float sq, float n) {      /* standard deviation from sums */
  float mean = sum / n;
  float var = sq / n - mean * mean;
  return var > 0.0f ? sqrtf(var) : 0.0f;
}

void wf_compute(const WardenFeatures *w, float *out) {
  float n = (float)(w->n ? w->n : 1);
  out[0] = w->m_sum / n;
  out[1] = spread(w->m_sum, w->m_sq, n);
  out[2] = w->m_min;
  out[3] = w->m_max;
  out[4] = w->ax_sum / n;
  out[5] = w->ay_sum / n;
  out[6] = w->az_sum / n;
  out[7] = spread(w->ax_sum, w->ax_sq, n);
  out[8] = spread(w->ay_sum, w->ay_sq, n);
  out[9] = spread(w->az_sum, w->az_sq, n);
  out[10] = w->g_sum / n;
  out[11] = w->g_max;
  out[12] = w->n > 1 ? w->jerk_sum / (n - 1.0f) : 0.0f;
  out[13] = (float)w->ff_count / n;
  out[14] = (float)w->hi_count / n;
  out[15] = w->jerk_max;
  out[16] = (float)w->sharp_count / n;
}
