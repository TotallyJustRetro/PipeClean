#include "yougame_web.h"
#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>

#include "games.h"
#include "launcher.h"
#include "rom.h"

#define PIPECLEAN_WEB_ROM_PATH "/pipeclean-sml2.gb"

EMSCRIPTEN_KEEPALIVE
int pc_yg_import_base_rom(const unsigned char *bytes, unsigned int size)
{
    FILE *file;
    int identified;
    if (!bytes || size < 0x150 || size > (8u << 20)) return 0;
    identified = rom_identify(bytes, size);
    if (identified != GAME_SML2) {
        launcher_toast("That file is not a supported Super Mario Land 2 ROM.");
        return 0;
    }
    file = fopen(PIPECLEAN_WEB_ROM_PATH, "wb");
    if (!file) {
        launcher_toast("Couldn't write the browser ROM copy.");
        return 0;
    }
    if (fwrite(bytes, 1, size, file) != size) {
        fclose(file);
        launcher_toast("Couldn't save the browser ROM copy.");
        return 0;
    }
    if (fclose(file) != 0) {
        launcher_toast("Couldn't close the browser ROM copy.");
        return 0;
    }
    launcher_drop(PIPECLEAN_WEB_ROM_PATH);
    return 1;
}

EM_ASYNC_JS(int, pc_yg_load_base_game_js, (), {
    try {
        if (typeof YouGame === "undefined" || !YouGame.baseGame) return 0;
        await YouGame.ready();
        const base = await YouGame.baseGame();
        if (!base || !base.bytes || !base.sha1) return 0;
        const bytes = new Uint8Array(base.bytes);
        const ptr = _malloc(bytes.byteLength);
        if (!ptr) return 0;
        HEAPU8.set(bytes, ptr);
        const ok = _pc_yg_import_base_rom(ptr, bytes.byteLength);
        _free(ptr);
        if (!ok) return 0;
        const state = window.__pcYouGame || (window.__pcYouGame = {});
        state.baseSha1 = String(base.sha1).toLowerCase();
        state.baseName = String(base.name || "Super Mario Land 2");
        if (typeof YouGame.setGameState === "function") YouGame.setGameState("menu");
        return 1;
    } catch (error) {
        console.error("[PipeClean/YouGame] Base-game import failed:", error);
        return 0;
    }
});

int yougame_web_prepare_base_game(void)
{
    return pc_yg_load_base_game_js();
}
