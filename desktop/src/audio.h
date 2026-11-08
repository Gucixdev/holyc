/* audio.h - the volume backend and the `volume` command (see audio.c). */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdio.h>
extern int volume_osd_ms, volume_step, volume_max;   /* settings: OSD timeout (ms, 0 = no OSD), step (percent), ceiling (percent) */
/* run one `volume ...` command line (the part after "volume"); writes the reply to o; returns 1 when the volume or mute changed */
int audio_command(char *args, FILE *o);
/* the state line `volume get` prints: "spk 55 0 mic 30 0 seq 12 osd 1500 step 5 max 100" (0 = not muted) */
void audio_state(char *buf, size_t n);
/* start the small volume panel (`desktop settings volume --osd`) unless one is already up; no-op when volume_osd_ms is 0 */
void audio_osd(void);
#endif
