/* On-chip check: replays the first 800 samples of a recorded session through features.c + model.h
   on an ATmega328P simulated by simavr, prints the votes and cycle counts. See tools/parity/README.md. */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <stdio.h>
#include "features.h"
#include "model.h"
#include "session.h"

static int uart_putchar(char c, FILE *s) { (void)s; while (!(UCSR0A & (1 << UDRE0))); UDR0 = c; return 0; }
static FILE out = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);
static volatile uint16_t ovf;
ISR(TIMER1_OVF_vect) { ovf++; }
static uint32_t cycles(void) {
  uint16_t t, o;
  cli(); t = TCNT1; o = ovf; if ((TIFR1 & (1 << TOV1)) && t < 0x8000) o++; sei();
  return ((uint32_t)o << 16) | t;
}

int main(void) {
  UBRR0 = 8; UCSR0B = (1 << TXEN0); stdout = &out;
  TCCR1B = 1; TIMSK1 = 1 << TOIE1; sei();
  WardenFeatures acc[2];
  wf_reset(&acc[0]); wf_reset(&acc[1]);
  uint32_t add_max = 0, inf_max = 0, inf_sum = 0; uint16_t k = 0;
  for (uint16_t i = 0; i < SESSION_N; i++) {
    float ax = (int16_t)pgm_read_word(&ACC[i][0]) / 1000.0f;
    float ay = (int16_t)pgm_read_word(&ACC[i][1]) / 1000.0f;
    float az = (int16_t)pgm_read_word(&ACC[i][2]) / 1000.0f;
    float gx = (int32_t)pgm_read_dword(&GYR[i][0]) / 100.0f;
    float gy = (int32_t)pgm_read_dword(&GYR[i][1]) / 100.0f;
    float gz = (int32_t)pgm_read_dword(&GYR[i][2]) / 100.0f;
    uint32_t t0 = cycles();
    wf_add(&acc[0], ax, ay, az, gx, gy, gz);
    if (i >= 50) wf_add(&acc[1], ax, ay, az, gx, gy, gz);
    uint32_t dt = cycles() - t0; if (dt > add_max) add_max = dt;
    for (uint8_t w = 0; w < 2; w++) if (wf_ready(&acc[w])) {
      float x[WF_COUNT]; uint8_t v[WM_CLASSES];
      t0 = cycles();
      wf_compute(&acc[w], x);
      wm_predict(x, v);
      dt = cycles() - t0; if (dt > inf_max) inf_max = dt; inf_sum += dt;
      wf_reset(&acc[w]);
      printf("W %u %u %u %u %u\n", k++, v[0], v[1], v[2], v[3]);
    }
  }
  printf("T %lu %lu %lu\n", add_max, inf_max, k ? inf_sum / k : 0);
  while (!(UCSR0A & (1 << TXC0)));
  cli(); sleep_cpu();
}
