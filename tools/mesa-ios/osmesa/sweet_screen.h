#ifndef SWEET_SCREEN_H
#define SWEET_SCREEN_H
struct pipe_screen;
/* panfrost's screen on the SweetMali service, or NULL (then the caller renders in software). */
struct pipe_screen *sweet_panfrost_screen(void);
#endif
