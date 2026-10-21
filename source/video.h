#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MP4 (H.264 + AAC) player: New 3DS hardware video decoder (MVD) and the
 * Helix AAC decoder through NDSP. One video at a time. Frames come out as
 * RGB565 at a size fitting max_w x max_h. */

typedef enum { VP_IDLE, VP_STARTING, VP_PLAYING, VP_ERROR } VpState;

bool    vp_start(const char *path, int max_w, int max_h);
void    vp_stop(void);
bool    vp_running(void);
VpState vp_state(char *msg, size_t n);       /* msg: error text, or a note (e.g. "no sound: ...") */
bool    vp_dims(int *w, int *h);             /* display frame size, once known */

/* If a frame is due, calls upload(frame, ud) and returns true. Late frames are
 * skipped. Call once per screen frame from the main thread. Also loops the
 * video at the end. */
bool    vp_show_due(void (*upload)(const uint16_t *px, void *ud), void *ud);

double  vp_time(void);
double  vp_duration(void);
bool    vp_paused(void);
void    vp_set_paused(bool p);
void    vp_seek(double t);
bool    vp_has_audio(void);
