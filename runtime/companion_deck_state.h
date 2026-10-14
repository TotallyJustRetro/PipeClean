#pragma once

#include "games.h"

/* Game-deck eligibility is intentionally separate from frame availability.
 * The deck must switch as soon as gameplay starts, including startup or any
 * tick in which the emulator has not supplied a new render frame yet. */
static inline int companion_deck_supports_game_view(int game)
{
    return game == GAME_DRMARIO || game == GAME_SML ||
           game == GAME_SML2 || game == GAME_WARIO_SML3;
}

static inline int companion_deck_should_show_game_view(int game, int playing,
                                                        int editor_preview)
{
    /* Layout preview is an explicit request to show the game's companion.
       Otherwise switch only for an active game session, so the dashboard
       returns automatically when gameplay closes. */
    if (editor_preview) return 1;
    return playing && companion_deck_supports_game_view(game);
}
