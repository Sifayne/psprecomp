#ifndef PSPRECOMP_HOST_SAVE_DIALOG_H
#define PSPRECOMP_HOST_SAVE_DIALOG_H
#include <SDL2/SDL.h>
#include <stdint.h>
enum { SAVE_DIALOG_W=960, SAVE_DIALOG_H=544 };
/* SDL-thread calls. The UI never reads or writes guest memory. */
int save_dialog_init(void);
/* Why init failed (font, SDL_ttf, canvas), or NULL. Static string. */
const char *save_dialog_error(void);
void save_dialog_shutdown(void);
void save_dialog_update(SDL_GameController *pad);
int save_dialog_active(void);
int save_dialog_input_neutral(SDL_GameController *pad);
int save_dialog_event(const SDL_Event *event, SDL_JoystickID controller);
void save_dialog_draw_software(SDL_Renderer *renderer);
/* GL-owner call: copies a completed RGBA overlay. No SDL calls. Returns active. */
int save_dialog_copy_pixels(uint32_t *out, uint64_t *revision);
/* Same completed pixels for visual test artifacts. SDL thread only. */
int save_dialog_capture(const char *path);
#endif
