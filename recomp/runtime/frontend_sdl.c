/* Fenster-Frontend mit SDL3.
 *
 * Tastenbelegung (vorläufig, später frei einstellbar):
 *   Pfeiltasten  Steuerkreuz        X  A (Mario)        C  B (Luigi)
 *   S            X                  D  Y
 *   Enter        Start              Rücktaste / rechte Umschalttaste  Select
 *   Q            L                  W  R
 *   F11          Vollbild an/aus    Esc  beenden
 * Das Bild wird ohne Verzerrung skaliert (Balken an den Seiten bei Bedarf).
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include "frontend.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint64_t next_frame_ns;
static uint64_t fps_t0, fps_frames;

#define FRAME_NS 16715000ull   /* DS: 59,826 Bilder/s */

int frontend_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 0; }
    win = SDL_CreateWindow("Mario & Luigi: Bowser's Inside Story - PC (prototype)", 512, 768, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 0; }
    SDL_SetRenderVSync(ren, 0);
    SDL_SetRenderLogicalPresentation(ren, 256, 384, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 384);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return 0; }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);   /* scharfe Pixel */
    next_frame_ns = SDL_GetTicksNS();
    fps_t0 = next_frame_ns;
    return 1;
}

void frontend_present(const uint32_t *top, const uint32_t *bottom) {
    void *pixels;
    int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
        for (int y = 0; y < 384; y++) {
            const uint32_t *src = y < 192 ? top + y * 256 : bottom + (y - 192) * 256;
            SDL_memcpy((uint8_t *)pixels + y * pitch, src, 256 * 4);
        }
        SDL_UnlockTexture(tex);
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);

    /* Bildrate im Fenstertitel */
    fps_frames++;
    uint64_t now = SDL_GetTicksNS();
    if (now - fps_t0 >= 1000000000ull) {
        char title[160];
        SDL_snprintf(title, sizeof title, "Mario & Luigi: Bowser's Inside Story - PC (prototype) - %.1f fps",
                     (double)fps_frames * 1e9 / (double)(now - fps_t0));
        SDL_SetWindowTitle(win, title);
        fps_t0 = now;
        fps_frames = 0;
    }
}

int frontend_poll(uint16_t *keys, uint16_t *xy) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return 0;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) return 0;
            if (e.key.scancode == SDL_SCANCODE_F11)
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
        }
    }
    const bool *k = SDL_GetKeyboardState(NULL);
    uint16_t pressed = 0;
    if (k[SDL_SCANCODE_X]) pressed |= 1u << 0;                 /* A */
    if (k[SDL_SCANCODE_C]) pressed |= 1u << 1;                 /* B */
    if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) pressed |= 1u << 2;   /* Select */
    if (k[SDL_SCANCODE_RETURN]) pressed |= 1u << 3;            /* Start */
    if (k[SDL_SCANCODE_RIGHT]) pressed |= 1u << 4;
    if (k[SDL_SCANCODE_LEFT]) pressed |= 1u << 5;
    if (k[SDL_SCANCODE_UP]) pressed |= 1u << 6;
    if (k[SDL_SCANCODE_DOWN]) pressed |= 1u << 7;
    if (k[SDL_SCANCODE_W]) pressed |= 1u << 8;                 /* R */
    if (k[SDL_SCANCODE_Q]) pressed |= 1u << 9;                 /* L */
    *keys = (uint16_t)(0x03ff & ~pressed);
    uint16_t x_y = 0x0c00;
    if (k[SDL_SCANCODE_S]) x_y &= ~0x0400;                     /* X */
    if (k[SDL_SCANCODE_D]) x_y &= ~0x0800;                     /* Y */
    *xy = x_y;
    return 1;
}

void frontend_wait_close(const char *reason) {
    char title[256];
    SDL_snprintf(title, sizeof title, "Stopped: %s  (report in the console or log, close the window to exit)", reason);
    SDL_SetWindowTitle(win, title);
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) break;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) break;
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
}

void frontend_pace(void) {
    next_frame_ns += FRAME_NS;
    uint64_t now = SDL_GetTicksNS();
    if (next_frame_ns > now) SDL_DelayNS(next_frame_ns - now);
    else if (now - next_frame_ns > 100000000ull) next_frame_ns = now;   /* zu langsam: nicht aufholen */
}
