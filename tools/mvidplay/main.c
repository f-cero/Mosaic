/* SPDX-License-Identifier: Apache-2.0
 * mvidplay — the desktop/Windows MVID player. Builds against SDL2 only.
 *   keys: space pause, left/right step, home restart, esc quit
 */
#include "mvid.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mvidplay clip.mvid\n"); return 1; }
    size_t sz = 0;
    uint8_t *blob = mvid_read_file(argv[1], &sz);
    if (!blob) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

    static uint8_t sheet[MVID_MAX_TILES * 64];
    static uint16_t map[128 * 128];
    mvid_t v;
    int rc = mvid_open(&v, blob, sz, sheet, map);
    if (rc != MVID_E_OK) { fprintf(stderr, "%s: %s\n", argv[1], mvid_err_str(rc)); free(blob); return 3; }

    const int cw = v.info.width, ch = v.info.height;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); free(blob); return 4;
    }
    SDL_Window *win = SDL_CreateWindow("mvidplay", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, cw * 2, ch * 2, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(ren, cw, ch);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STREAMING, cw, ch);
    uint16_t *fb = (uint16_t *)malloc((size_t)cw * ch * 2u);
    if (!win || !ren || !tex || !fb) { fprintf(stderr, "SDL init failed\n"); return 5; }

    int running = 1, paused = 0;
    const Uint32 frame_ms = (Uint32)(1000 / (v.info.fps ? v.info.fps : 12));
    Uint32 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_SPACE: paused = !paused; break;
                case SDLK_HOME: mvid_seek(&v, 0); last = SDL_GetTicks(); break;
                case SDLK_RIGHT:
                    if (v.cur + 1 < v.info.frames) mvid_seek(&v, (uint16_t)(v.cur + 1));
                    last = SDL_GetTicks(); break;
                case SDLK_LEFT:
                    if (v.cur > 0) mvid_seek(&v, (uint16_t)(v.cur - 1));
                    last = SDL_GetTicks(); break;
                default: break;
                }
            }
        }
        const Uint32 now = SDL_GetTicks();
        if (!paused && now - last >= frame_ms) {
            last = now;
            if (!mvid_next(&v)) {
                if (v.info.flags & MVID_F_LOOP) mvid_seek(&v, 0);
                else { paused = 1; }
            }
        }
        mvid_draw(&v, fb, (unsigned)cw, (unsigned)ch);
        SDL_UpdateTexture(tex, NULL, fb, cw * 2);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(4);
    }
    free(fb); free(blob);
    SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
