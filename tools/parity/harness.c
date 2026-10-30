/* WARDEN parity harness: replays session CSVs through the firmware's own C code
   (features.c + model.h) on the laptop and prints the votes of every window.
   Built and run by tools/check_parity.py. */
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/warden/features.h"
#include "../../firmware/warden/model.h"

int main(int argc, char **argv) {
  for (int a = 1; a < argc; a++) {
    FILE *fp = fopen(argv[a], "r");
    char line[256];
    if (!fp || !fgets(line, sizeof line, fp)) { fprintf(stderr, "cannot read %s\n", argv[a]); return 1; }
    WardenFeatures acc[2];
    wf_reset(&acc[0]); wf_reset(&acc[1]);
    long i = 0, k = 0;
    while (fgets(line, sizeof line, fp)) {
      long t; float v[6]; int light;
      if (sscanf(line, "%ld,%f,%f,%f,%f,%f,%f,%d", &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &light) != 8) continue;
      wf_add(&acc[0], v[0], v[1], v[2], v[3], v[4], v[5]);
      if (i >= 50) wf_add(&acc[1], v[0], v[1], v[2], v[3], v[4], v[5]);
      for (int w = 0; w < 2; w++) {
        if (wf_ready(&acc[w])) {
          float x[WF_COUNT]; uint8_t votes[WM_CLASSES];
          wf_compute(&acc[w], x);
          wm_predict(x, votes);
          printf("%s %ld %d %d %d %d\n", argv[a], k++, votes[0], votes[1], votes[2], votes[3]);
          wf_reset(&acc[w]);
        }
      }
      i++;
    }
    fclose(fp);
  }
  return 0;
}
