#ifndef PIPECLEAN_MUSEUM3D_H
#define PIPECLEAN_MUSEUM3D_H

#include <SDL.h>

/* Draw the Museum's actual perspective-rendered 3D environment behind the
 * existing interactive SDL UI. Coordinates are launcher logical pixels. */
void museum3d_draw_background(SDL_Renderer *renderer,
                              float x, float y, float width, float height,
                              float elapsed_seconds,
                              float museum_x, float museum_y, float museum_scroll,
                              int visible_entries, int draw_cubby_grid);

#endif
