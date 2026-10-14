#include "companion_deck_state.h"
#include <stdio.h>

static int check(const char *name, int game, int playing, int preview, int expected)
{
    int actual = companion_deck_should_show_game_view(game, playing, preview);
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s (expected %d, got %d)\n", name, expected, actual);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += check("idle Wario selection keeps Recently Played dashboard",
                      GAME_WARIO_SML3, 0, 0, 0);
    failures += check("active Wario uses Treasure Expedition deck",
                      GAME_WARIO_SML3, 1, 0, 1);
    failures += check("active Wario deck stays selected without a fresh frame",
                      GAME_WARIO_SML3, 1, 0, 1);
    failures += check("Wario layout preview works while idle",
                      GAME_WARIO_SML3, 0, 1, 1);
    failures += check("Super Mario Land deck remains supported",
                      GAME_SML, 1, 0, 1);
    failures += check("Super Mario Land 2 deck remains supported",
                      GAME_SML2, 1, 0, 1);
    failures += check("Dr. Mario deck remains supported",
                      GAME_DRMARIO, 1, 0, 1);
    failures += check("idle Wario Land II GB selection keeps Recently Played dashboard",
                      GAME_WARIO_LAND2_GB, 0, 0, 0);
    failures += check("active Wario Land II GB uses its Companion Deck",
                      GAME_WARIO_LAND2_GB, 1, 0, 1);
    failures += check("active Wario Land II deck stays selected without a fresh frame",
                      GAME_WARIO_LAND2_GB, 1, 0, 1);
    failures += check("Wario Land II GB layout preview works while idle",
                      GAME_WARIO_LAND2_GB, 0, 1, 1);
    failures += check("idle Wario Land II GBC selection keeps Recently Played dashboard",
                      GAME_WARIO_LAND2_GBC, 0, 0, 0);
    failures += check("active Wario Land II GBC uses the same Companion Deck",
                      GAME_WARIO_LAND2_GBC, 1, 0, 1);
    failures += check("closing Wario Land II returns to dashboard",
                      GAME_WARIO_LAND2_GBC, 0, 0, 0);
    failures += check("Wario Land 3 is still separate from the Wario Land II deck",
                      GAME_WARIO_LAND3_GBC, 1, 0, 0);
    if (failures) {
        fprintf(stderr, "%d companion deck state check(s) failed\n", failures);
        return 1;
    }
    puts("Companion Deck state checks passed.");
    return 0;
}
