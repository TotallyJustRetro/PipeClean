package com.pipeclean.app;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Movie;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import java.io.FileInputStream;
import java.io.InputStream;
import java.util.Locale;

/** A playful, game-specific second-screen companion deck. All artwork is original UI art. */
final class PipeCleanCompanionPresentation extends Presentation {
    private final DeckView deck;

    PipeCleanCompanionPresentation(Context context, Display display) {
        super(context, display);
        deck = new DeckView(context);
        String savedBackground = context.getSharedPreferences(
            "PipeCleanCompanion", Context.MODE_PRIVATE).getString("background_path", "");
        deck.setBackgroundFile(savedBackground);
        setContentView(deck);
        Window window = getWindow();
        if (window != null) {
            window.setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                            WindowManager.LayoutParams.FLAG_FULLSCREEN);
            window.getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    void updateState(int game, int lives, int stage, int world, boolean paused,
                     int slot, int palette, int filter, int item) {
        deck.setState(game, lives, stage, world, paused, slot, palette, filter, item);
    }

    void setBackgroundFile(String path) { deck.setBackgroundFile(path); }

    private static final class DeckView extends View {
        private static final int SAVE = 1, LOAD = 2, PAUSE = 3, REWIND = 4;
        private static final int PALETTE = 5, FILTER = 6, SLOT_PREV = 7, SLOT_NEXT = 8;
        private static final int ITEM_USE = 9, BACKGROUND = 10, RESET_BACKGROUND = 11, CLOSE_GAME = 12;

        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int game = -1, lives, stage = -1, world = -1, slot, palette, filter, item, downAction;
        private boolean paused, itemPressed;
        private long itemPressStart, itemReleaseStart;
        private Bitmap wallpaperBitmap;
        private Movie wallpaperMovie;
        private String wallpaperPath = "";
        private static final String[] PALETTES = {
            "Classic Green", "Original DMG", "Pocket", "Black & White", "Ice Blue",
            "Cherry", "Gold", "Grape", "Mint", "Sepia", "Sunset", "Midnight",
            "Classic (color)", "Arcade (color)", "Candy (color)", "Ocean (color)"
        };
        private static final String[] FILTERS = {
            "None", "Game Boy LCD", "Pocket", "CRT TV", "Bloom", "HDR"
        };

        DeckView(Context context) {
            super(context);
            setFocusable(true);
            setClickable(true);
            p.setTypeface(Typeface.create("monospace", Typeface.BOLD));
            setContentDescription("PipeClean companion deck: item button, save states, pause, rewind, wallpaper and close game");
        }

        void setState(int game, int lives, int stage, int world, boolean paused,
                      int slot, int palette, int filter, int item) {
            this.game = game;
            this.lives = Math.max(0, Math.min(99, lives));
            this.stage = stage;
            this.world = world;
            this.paused = paused;
            this.slot = Math.max(0, Math.min(9, slot));
            this.palette = Math.max(0, Math.min(PALETTES.length - 1, palette));
            this.filter = Math.max(0, Math.min(FILTERS.length - 1, filter));
            this.item = item;
            postInvalidate();
        }

        void setBackgroundFile(String path) {
            String next = path == null ? "" : path.trim();
            if (next.equals(wallpaperPath) && (wallpaperBitmap != null || wallpaperMovie != null || next.isEmpty()))
                return;
            wallpaperPath = next;
            if (wallpaperBitmap != null) {
                wallpaperBitmap.recycle();
                wallpaperBitmap = null;
            }
            wallpaperMovie = null;
            if (!next.isEmpty()) {
                try {
                    if (next.toLowerCase(Locale.US).endsWith(".gif")) {
                        try (InputStream in = new FileInputStream(next)) {
                            wallpaperMovie = Movie.decodeStream(in);
                        }
                    } else {
                        wallpaperBitmap = decodeSampledBitmap(next);
                    }
                } catch (Exception ex) {
                    wallpaperBitmap = null;
                    wallpaperMovie = null;
                    android.util.Log.w("PipeClean", "Couldn't load companion wallpaper.", ex);
                }
                if (wallpaperBitmap == null && wallpaperMovie == null) {
                    wallpaperPath = "";
                    android.util.Log.w("PipeClean", "Unsupported or unreadable companion wallpaper.");
                }
            }
            invalidate();
        }

        private Bitmap decodeSampledBitmap(String path) {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null;
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = 1;
            while (bounds.outWidth / options.inSampleSize > 2048 ||
                   bounds.outHeight / options.inSampleSize > 2048)
                options.inSampleSize *= 2;
            return BitmapFactory.decodeFile(path, options);
        }

        private boolean sml1() { return game == 1; }
        private boolean sml2() { return game == 2; }
        private int bg() { return sml1() ? 0xFFB8D991 : sml2() ? 0xFF25152F : 0xFF172337; }
        private int panel() { return sml1() ? 0xF0F0F5D5 : sml2() ? 0xEE352342 : 0xEE24354C; }
        private int accent() { return sml1() ? 0xFF397C30 : sml2() ? 0xFFFFCA5B : 0xFF6EE5EF; }
        private int ink() { return sml1() ? 0xFF253C20 : sml2() ? 0xFFFFF0D2 : 0xFFEAF5FF; }
        private int muted() { return sml1() ? 0xFF57764D : sml2() ? 0xFFD1B7D5 : 0xFFAFC3DD; }
        private int edge() { return sml1() ? 0xFF80A951 : sml2() ? 0xFFAD7B41 : 0xFF577D9D; }
        private int gold() { return 0xFFFFD66B; }
        private int goldDark() { return 0xFFAC6B22; }

        private String title() {
            if (sml1()) return "SUPER MARIO LAND";
            if (sml2()) return "6 GOLDEN COINS";
            if (game == 0) return "DR. MARIO";
            return "PIPECLEAN // READY";
        }

        private String stageText() {
            if (stage < 0) return "—";
            if (sml1() && world > 0 && stage > 0)
                return String.format(Locale.US, "%d - %d", world, stage);
            if (sml2()) return String.format(Locale.US, "STAGE %02d", stage);
            return "—";
        }

        private String itemName() {
            if (item == 0) return "EMPTY POCKET";
            if (sml1()) {
                if (item == 1) return "MUSHROOM";
                if (item == 2) return "SUPERBALL FLOWER";
            }
            if (sml2()) {
                if (item == 1) return "MUSHROOM";
                if (item == 2) return "FIRE FLOWER";
                if (item == 3) return "CARROT";
            }
            return "POWER-UP";
        }

        private String actionHint() {
            if (item == 0) return "Find a power-up to fill your pocket";
            return sml1() ? "Tap to release into the level" : "Tap when you're ready";
        }

        private String paletteText() { return PALETTES[palette]; }
        private String filterText() { return FILTERS[filter]; }

        @Override protected void onDraw(Canvas c) {
            super.onDraw(c);
            boolean portrait = getHeight() > getWidth();
            float w = portrait ? 450f : 800f, h = portrait ? 800f : 450f;
            float scale = Math.min(getWidth() / w, getHeight() / h);
            c.save();
            c.translate((getWidth() - w * scale) * 0.5f, (getHeight() - h * scale) * 0.5f);
            c.scale(scale, scale);
            drawWallpaper(c, w, h);
            if (portrait) drawPortrait(c); else drawLandscape(c);
            drawCornerGems(c, w, h);
            c.restore();
            // The gold ring and power-up breathe; GIF wallpapers advance at about 30 fps.
            postInvalidateDelayed(33);
        }

        private void drawWallpaper(Canvas c, float w, float h) {
            // Clear before drawing so transparent PNGs reveal a clean theme-colored base.
            c.drawColor(bg());
            if (wallpaperMovie != null && wallpaperMovie.width() > 0 && wallpaperMovie.height() > 0) {
                int duration = wallpaperMovie.duration();
                if (duration <= 0) duration = 1000;
                wallpaperMovie.setTime((int)(SystemClock.uptimeMillis() % duration));
                float scale = Math.max(w / wallpaperMovie.width(), h / wallpaperMovie.height());
                float dw = wallpaperMovie.width() * scale, dh = wallpaperMovie.height() * scale;
                c.save();
                c.translate((w - dw) * 0.5f, (h - dh) * 0.5f);
                c.scale(scale, scale);
                p.setColor(0xFFFFFFFF);
                wallpaperMovie.draw(c, 0, 0, p);
                c.restore();
                c.drawColor(0x4C090B1C);
            } else if (wallpaperBitmap != null && !wallpaperBitmap.isRecycled()) {
                float scale = Math.max(w / wallpaperBitmap.getWidth(), h / wallpaperBitmap.getHeight());
                float dw = wallpaperBitmap.getWidth() * scale, dh = wallpaperBitmap.getHeight() * scale;
                c.save();
                c.translate((w - dw) * 0.5f, (h - dh) * 0.5f);
                c.scale(scale, scale);
                p.setColor(0xFFFFFFFF);
                c.drawBitmap(wallpaperBitmap, 0, 0, p);
                c.restore();
                c.drawColor(0x4C090B1C);
            } else {
                drawScenery(c, w, h);
            }
        }

        private void drawScenery(Canvas c, float w, float h) {
            c.drawColor(bg());
            if (sml1()) {
                // Original pixel hills/clouds, deliberately not game sprites.
                rect(c, 0, h-75, w, 75, 0xFF9CC977);
                c.drawOval(-40, h-150, w*0.42f, h+18, pColor(0xFF91C16D));
                c.drawOval(w*0.28f, h-120, w*0.76f, h+18, pColor(0xFF8AB967));
                c.drawOval(w*0.63f, h-163, w+30, h+18, pColor(0xFF99C875));
                pixelCloud(c, 50, 68, 6, 0xFFEAF6D4);
                pixelCloud(c, w-170, 116, 4, 0xFFEAF6D4);
                pixelCloud(c, w*0.48f, 38, 3, 0xFFDCEEBF);
                for (int i=0;i<9;i++) rect(c, 18+i*30, h-14, 4, 4, 0xFF68974B);
            } else if (sml2()) {
                rect(c, 0, h-64, w, 64, 0xFF392749);
                c.drawOval(-35, h-155, w*0.45f, h+20, pColor(0xFF30203D));
                c.drawOval(w*0.35f, h-124, w*0.88f, h+20, pColor(0xFF342142));
                c.drawCircle(w-67, 66, 26, pColor(0xFFFFD87A));
                c.drawCircle(w-55, 58, 23, pColor(0xFF37233F));
                for (int i=0;i<32;i++) {
                    float sx=(i*73+17)%w, sy=(i*47+21)%(Math.max(1,h-38));
                    rect(c,sx,sy,(i%5==0)?4:2,(i%5==0)?4:2,(i%3==0)?0xFFFFD66B:0xFFCBB5E8);
                }
                pixelCloud(c, 40, 140, 3, 0xFF5A416A);
            } else {
                for (int i=0;i<32;i++) rect(c,(i*53+9)%w,(i*31+17)%h,3,3,edge());
            }
            // Thin horizontal scan accents make the deck read like a playful handheld.
            for (int y=0; y<h; y+=6) rect(c,0,y,w,1,0x12000000);
        }

        private Paint pColor(int color) { p.setColor(color); p.setStyle(Paint.Style.FILL); return p; }

        private void drawCornerGems(Canvas c, float w, float h) {
            int col = sml2() ? 0xCCFFD66B : sml1() ? 0xCCEFF9D7 : 0xCC6EE5EF;
            rect(c, 9, 9, 12, 3, col); rect(c, 9, 9, 3, 12, col);
            rect(c, w-21, 9, 12, 3, col); rect(c, w-12, 9, 3, 12, col);
            rect(c, 9, h-12, 12, 3, col); rect(c, 9, h-21, 3, 12, col);
            rect(c, w-21, h-12, 12, 3, col); rect(c, w-12, h-21, 3, 12, col);
        }

        private void drawLandscape(Canvas c) {
            // Header: live game status plus a real wallpaper picker.
            headerPlate(c, 18, 7, 265, 49);
            text(c, "PIPE CLEAN  •  POCKET QUEST", 26, 23, 10, accent());
            text(c, title(), 25, 43, 16, ink());
            badge(c, 294, 13, 104, 42, "LIVES", String.format(Locale.US, "× %02d", lives));
            badge(c, 407, 13, 135, 42, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE", stageText());
            if (!wallpaperPath.isEmpty()) smallButton(c, 552, 17, 34, 34, "×", RESET_BACKGROUND);
            button(c, 593, 13, 105, 42, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 704, 13, 72, 42, "CLOSE", "GAME", CLOSE_GAME, false);

            // Centerpiece: the item pocket is deliberately the largest thing on the deck.
            drawHeroItem(c, 400, 204, 98, item);
            textFit(c, itemName(), 400, 337, 250, 15, ink(), true);
            textFit(c, actionHint(), 400, 350, 330, 8.5f, muted(), true);

            // Slot and state badges are secondary so they don't compete with the hero item.
            card(c, 36, 117, 152, 145, "SAVE SLOT");
            text(c, "SLOT", 112, 155, 9, muted(), true);
            text(c, String.format(Locale.US, "%02d", slot+1), 112, 199, 33, ink(), true);
            smallButton(c, 48, 214, 55, 35, "−", SLOT_PREV);
            smallButton(c, 121, 214, 55, 35, "+", SLOT_NEXT);
            text(c, "QUICK SELECT", 112, 252, 8, muted(), true);

            card(c, 612, 125, 152, 123, "ADVENTURE STATUS");
            drawMiniStar(c, 637, 164, 9);
            text(c, paused ? "PAUSED" : "IN PLAY", 654, 166, 11, ink());
            text(c, "LIVE GAME DATA", 626, 190, 8, muted());
            textFit(c, sml1() ? "RETRO LCD" : sml2() ? "GOLDEN QUEST" : "HANDHELD", 626, 216, 125, 9, accent());
            text(c, wallpaperPath.isEmpty() ? "DEFAULT SCENE" : "CUSTOM SCENE", 626, 235, 8, muted());

            drawDockLandscape(c);
        }

        private void drawDockLandscape(Canvas c) {
            float x0=28, y=354, w=118, h=66, step=124;
            button(c,94,y,145,h,"SAVE","SNAPSHOT",SAVE,false);
            button(c,247,y,145,h,"LOAD","RESTORE",LOAD,false);
            button(c,400,y,145,h,paused?"RESUME":"PAUSE",paused?"CONTINUE":"HOLD GAME",PAUSE,false);
            button(c,553,y,153,h,"REWIND","HOLD TO REWIND",REWIND,true);
        }

        private void drawPortrait(Canvas c) {
            headerPlate(c, 14, 7, 220, 43);
            text(c, "PIPE CLEAN  •  POCKET QUEST", 20, 25, 10, accent());
            text(c, title(), 20, 43, 13, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 238, 16, 20, 27, "×", RESET_BACKGROUND);
            button(c, 262, 12, 87, 37, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 355, 12, 75, 37, "CLOSE", "GAME", CLOSE_GAME, false);

            badge(c, 20, 55, 128, 47, "LIVES", String.format(Locale.US, "× %02d", lives));
            badge(c, 158, 55, 272, 47, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE", stageText());

            drawHeroItem(c, 225, 275, 103, item);
            textFit(c, itemName(), 225, 416, 380, 16, ink(), true);
            textFit(c, actionHint(), 225, 432, 400, 8.5f, muted(), true);

            card(c, 35, 439, 380, 70, "QUICK SAVE SLOT");
            smallButton(c, 47, 460, 65, 34, "−", SLOT_PREV);
            text(c, String.format(Locale.US, "SLOT %02d", slot+1), 225, 482, 16, ink(), true);
            smallButton(c, 338, 460, 65, 34, "+", SLOT_NEXT);

            button(c,20,527,198,58,"SAVE","SNAPSHOT",SAVE,false);
            button(c,232,527,198,58,"LOAD","RESTORE",LOAD,false);
            button(c,20,594,198,58,paused?"RESUME":"PAUSE",paused?"CONTINUE":"HOLD GAME",PAUSE,false);
            button(c,232,594,198,58,"REWIND","HOLD TO REWIND",REWIND,true);
            button(c,20,661,198,58,"BACKGROUND","CHANGE PNG / GIF",BACKGROUND,false);
            button(c,232,661,198,58,"CLOSE GAME","EXIT TO MENU",CLOSE_GAME,false);

            text(c, paused ? "GAME PAUSED" : "READY WHEN YOU ARE", 225, 757, 10, accent(), true);
            text(c, wallpaperPath.isEmpty() ? "DEFAULT ARTWORK" : "CUSTOM WALLPAPER ACTIVE",
                 225, 777, 8, muted(), true);
        }

        private void drawHeroItem(Canvas c, float cx, float cy, float radius, int itemId) {
            float t = SystemClock.uptimeMillis() / 1000f;
            float pulse = 1.0f + (float)Math.sin(t*3.1f)*0.025f;
            long now = SystemClock.uptimeMillis();
            if (itemPressed) {
                float held = Math.min(1f, (now - itemPressStart) / 75f);
                pulse *= 1f - 0.13f * held;
            } else if (itemReleaseStart > 0 && now - itemReleaseStart < 220) {
                float u = (now - itemReleaseStart) / 220f;
                pulse *= 0.87f + 0.13f * (1f - (1f-u)*(1f-u));
            }
            // Soft gold aura, layered discs and a pixelated gold orbit.
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x28FFD86A);
            c.drawCircle(cx,cy,radius+22,p);
            p.setColor(0x28FFD86A);
            c.drawCircle(cx,cy,radius+11,p);
            p.setColor(sml1()?0xFF263C20:sml2()?0xFF21142E:0xFF1A293A);
            c.drawCircle(cx,cy,radius-7,p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(2);
            p.setColor(0xA0FFE7A0);
            c.drawCircle(cx,cy,radius-16,p);
            p.setStyle(Paint.Style.FILL);

            int beads=44;
            float rotation=t*0.34f;
            for(int i=0;i<beads;i++) {
                double a=(Math.PI*2.0*i/beads)+rotation;
                float wobble=(i%4==0)?2.8f:0f;
                float orbit=radius+((i%3==0)?8:4)+wobble;
                float bx=cx+(float)Math.cos(a)*orbit;
                float by=cy+(float)Math.sin(a)*orbit;
                float sz=(i%5==0)?7f:(i%2==0)?5f:3.5f;
                int color=(i%4==0)?0xFFFFF0AE:(i%3==0)?goldDark():gold();
                rect(c,bx-sz*.5f,by-sz*.5f,sz,sz,color);
                if(i%7==0) rect(c,bx-1,by-1,2,2,0xFFFFFFFF);
            }

            // Little floating sparkles rotate more slowly than the ring.
            for(int i=0;i<6;i++) {
                double a=(Math.PI*2.0*i/6.0)-rotation*0.55;
                float sr=radius+28+(float)Math.sin(t*2+i)*3;
                float sx=cx+(float)Math.cos(a)*sr, sy=cy+(float)Math.sin(a)*sr;
                drawSparkle(c,sx,sy,(i%2==0)?5:3,(i%3==0)?0xFFFFF3C8:gold());
            }

            if(itemId>0) {
                c.save();
                c.translate(cx,cy);
                c.scale(pulse,pulse);
                drawPowerUpGlyph(c,-radius*0.46f,-radius*0.46f,radius*0.92f,itemId);
                c.restore();
            } else {
                // Empty pocket: softly glowing 8-bit star prompt.
                drawPixelStar(c,cx,cy,17,0xFFB4A1CB);
                text(c,"READY",cx,cy+radius*.45f,8,muted(),true);
            }
        }

        private void drawPowerUpGlyph(Canvas c, float x, float y, float size, int itemId) {
            float u=size/10f;
            if(itemId==1) {
                p.setColor(sml1()?0xFFF4F8DF:0xFFFFEDC9);
                c.drawRect(x+4*u,y+5*u,x+6*u,y+8*u,p);
                c.drawRect(x+3*u,y+7*u,x+7*u,y+8*u,p);
                p.setColor(sml1()?0xFF5F873F:0xFFE84F48);
                c.drawRect(x+2*u,y+3*u,x+8*u,y+5*u,p);
                c.drawRect(x+3*u,y+2*u,x+7*u,y+3*u,p);
                p.setColor(sml1()?0xFFF7F8E3:0xFFFFF1BA);
                c.drawRect(x+3*u,y+3*u,x+4*u,y+4*u,p);
                c.drawRect(x+6*u,y+3*u,x+7*u,y+4*u,p);
                rect(c,x+2*u,y+5*u,1.3f*u,1.3f*u,0xFFFFD66B);
            } else if(itemId==2) {
                p.setColor(sml1()?0xFFF0F4D4:0xFF71D47A);
                c.drawRect(x+4*u,y+5*u,x+6*u,y+9*u,p);
                c.drawRect(x+2*u,y+7*u,x+5*u,y+8*u,p);
                p.setColor(sml1()?0xFFDCE9C9:0xFFE84A3F);
                c.drawRect(x+3*u,y+2*u,x+7*u,y+4*u,p);
                c.drawRect(x+2*u,y+3*u,x+8*u,y+6*u,p);
                p.setColor(sml1()?0xFFF7F8E3:0xFFFFE28A);
                c.drawRect(x+4*u,y+3*u,x+6*u,y+5*u,p);
            } else if(itemId==3 && sml2()) {
                p.setColor(0xFFFFA348);
                c.drawRect(x+3*u,y+3*u,x+7*u,y+4*u,p);
                c.drawRect(x+4*u,y+4*u,x+7*u,y+6*u,p);
                c.drawRect(x+5*u,y+6*u,x+6*u,y+8*u,p);
                p.setColor(0xFF69C56B);
                c.drawRect(x+3*u,y+1*u,x+5*u,y+3*u,p);
                c.drawRect(x+5*u,y+1*u,x+7*u,y+3*u,p);
                c.drawRect(x+4*u,y+2*u,x+6*u,y+4*u,p);
            } else {
                drawPixelStar(c,x+size*.5f,y+size*.5f,size*.3f,gold());
            }
        }

        private void headerPlate(Canvas c,float x,float y,float w,float h) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x6A000000);
            c.drawRoundRect(x+2,y+3,x+w+2,y+h+3,13,13,p);
            p.setColor(panel());
            c.drawRoundRect(x,y,x+w,y+h,13,13,p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(1.5f);
            c.drawRoundRect(x+0.75f,y+0.75f,x+w-.75f,y+h-.75f,12,12,p);
            p.setStyle(Paint.Style.FILL);
            rect(c,x+10,y+5,w-20,2,accent());
        }

        private void card(Canvas c,float x,float y,float w,float h,String label) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x60000000);
            c.drawRoundRect(x+2,y+4,x+w+2,y+h+4,16,16,p);
            p.setColor(panel());
            c.drawRoundRect(x,y,x+w,y+h,16,16,p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2);
            c.drawRoundRect(x+1,y+1,x+w-1,y+h-1,15,15,p);
            p.setStyle(Paint.Style.FILL);
            rect(c,x+12,y+9,w-24,3,accent());
            text(c,label,x+12,y+28,9,muted());
        }

        private void badge(Canvas c,float x,float y,float w,float h,String label,String value) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x7C09101B);
            c.drawRoundRect(x,y,x+w,y+h,12,12,p);
            p.setColor(sml2()?0xB5382948:sml1()?0xBFE9F3D0:0xB82A3D56);
            c.drawRoundRect(x+2,y+2,x+w-2,y+h-2,10,10,p);
            text(c,label,x+10,y+15,7,muted());
            textFit(c,value,x+10,y+h-8,w-18,14,ink());
        }

        private void button(Canvas c,float x,float y,float w,float h,String label,String sub,int action,boolean hold) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x60000000);
            c.drawRoundRect(x+2,y+4,x+w+2,y+h+4,13,13,p);
            p.setColor(action==downAction?accent():panel());
            c.drawRoundRect(x,y,x+w,y+h,13,13,p);
            p.setColor(action==downAction?gold():edge());
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2);
            c.drawRoundRect(x+1,y+1,x+w-1,y+h-1,12,12,p);
            p.setStyle(Paint.Style.FILL);
            rect(c,x+10,y+6,w-20,2,action==downAction?ink():accent());
            drawIcon(c,x+13,y+h*.5f-3,action);
            textFit(c,label,x+40,y+h*.5f+1,w-48,10,ink());
            textFit(c,sub,x+40,y+h*.5f+15,w-48,6.8f,muted());
            if(hold) textRight(c,"HOLD",x+w-6,y+h-5,5.8f,gold());
        }

        private void smallButton(Canvas c,float x,float y,float w,float h,String label,int action) {
            p.setColor(action==downAction?gold():panel());
            c.drawRoundRect(x,y,x+w,y+h,9,9,p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(1.5f);
            c.drawRoundRect(x+0.75f,y+0.75f,x+w-.75f,y+h-.75f,8,8,p);
            p.setStyle(Paint.Style.FILL);
            text(c,label,x+w*.5f,y+h*.5f+4,h*.62f,action==downAction?bg():ink(),true);
        }

        private void drawIcon(Canvas c,float x,float y,int action) {
            p.setColor(action==RESET_BACKGROUND?gold():accent());
            if(action==SAVE) {
                c.drawRoundRect(x,y-7,x+18,y+8,3,3,p); p.setColor(bg());
                c.drawRect(x+4,y-5,x+13,y-1,p); c.drawRect(x+5,y+3,x+14,y+6,p);
            } else if(action==LOAD) {
                c.drawRect(x+2,y+5,x+19,y+8,p); c.drawRect(x+9,y-8,x+12,y+4,p);
                Path path=new Path(); path.moveTo(x+4,y); path.lineTo(x+10.5f,y+7); path.lineTo(x+17,y); path.close(); c.drawPath(path,p);
            } else if(action==PAUSE) {
                c.drawRoundRect(x+3,y-8,x+8,y+8,2,2,p); c.drawRoundRect(x+13,y-8,x+18,y+8,2,2,p);
            } else if(action==REWIND) {
                Path a=new Path(); a.moveTo(x+1,y); a.lineTo(x+11,y-8); a.lineTo(x+11,y+8); a.close(); c.drawPath(a,p);
                Path b=new Path(); b.moveTo(x+10,y); b.lineTo(x+20,y-8); b.lineTo(x+20,y+8); b.close(); c.drawPath(b,p);
            } else if(action==PALETTE) {
                c.drawCircle(x+9,y,9,p); p.setColor(bg());
                c.drawCircle(x+9,y,3,p); p.setColor(accent());
                c.drawCircle(x+5,y-4,2,p); c.drawCircle(x+13,y-4,2,p); c.drawCircle(x+5,y+4,2,p);
            } else if(action==FILTER) {
                p.setStrokeWidth(2); p.setStyle(Paint.Style.STROKE);
                c.drawLine(x,y-6,x+19,y-6,p); c.drawLine(x,y,x+19,y,p); c.drawLine(x,y+6,x+19,y+6,p);
                p.setStyle(Paint.Style.FILL);
                c.drawCircle(x+5,y-6,2.5f,p); c.drawCircle(x+14,y,2.5f,p); c.drawCircle(x+9,y+6,2.5f,p);
            } else if(action==BACKGROUND) {
                c.drawRoundRect(x,y-8,x+20,y+8,3,3,p);
                Path hill=new Path(); hill.moveTo(x+2,y+6); hill.lineTo(x+8,y-2); hill.lineTo(x+12,y+3); hill.lineTo(x+16,y-1); hill.lineTo(x+19,y+6); hill.close(); c.drawPath(hill,p);
            }
        }

        private void pixelCloud(Canvas c,float x,float y,float u,int color) {
            rect(c,x+u,y,4*u,2*u,color);
            rect(c,x,y+u,6*u,2*u,color);
            rect(c,x+2*u,y+2*u,3*u,u,color);
        }

        private void drawMiniStar(Canvas c,float cx,float cy,float r) { drawPixelStar(c,cx,cy,r,gold()); }

        private void drawSparkle(Canvas c,float x,float y,float size,int color) {
            rect(c,x-size*.22f,y-size*.5f,size*.44f,size,color);
            rect(c,x-size*.5f,y-size*.22f,size,size*.44f,color);
        }

        private void drawPixelStar(Canvas c,float cx,float cy,float s,int color) {
            rect(c,cx-s*.18f,cy-s*.5f,s*.36f,s,color);
            rect(c,cx-s*.5f,cy-s*.18f,s,s*.36f,color);
            rect(c,cx-s*.32f,cy-s*.32f,s*.64f,s*.64f,color);
        }

        private void rect(Canvas c,float x,float y,float w,float h,int color) {
            p.setColor(color); p.setStyle(Paint.Style.FILL); c.drawRect(x,y,x+w,y+h,p);
        }

        private void text(Canvas c,String s,float x,float y,float size,int color) {
            p.setColor(color); p.setTextSize(size); p.setTextAlign(Paint.Align.LEFT); c.drawText(s,x,y,p);
        }

        private void text(Canvas c,String s,float x,float y,float size,int color,boolean centered) {
            p.setColor(color); p.setTextSize(size);
            p.setTextAlign(centered?Paint.Align.CENTER:Paint.Align.LEFT); c.drawText(s,x,y,p);
        }

        private void textRight(Canvas c,String s,float x,float y,float size,int color) {
            p.setColor(color); p.setTextSize(size); p.setTextAlign(Paint.Align.RIGHT); c.drawText(s,x,y,p);
        }

        private void textFit(Canvas c,String s,float x,float y,float max,float size,int color) {
            p.setTextSize(size);
            while(p.measureText(s)>max && size>6) { size-=0.5f; p.setTextSize(size); }
            text(c,s,x,y,size,color);
        }

        private void textFit(Canvas c,String s,float x,float y,float max,float size,int color,
                             boolean centered) {
            p.setTextSize(size);
            while(p.measureText(s)>max && size>6) { size-=0.5f; p.setTextSize(size); }
            p.setColor(color);
            p.setTextAlign(centered ? Paint.Align.CENTER : Paint.Align.LEFT);
            c.drawText(s,x,y,p);
            p.setTextAlign(Paint.Align.LEFT);
        }

        private int hitTest(float x,float y) {
            if(getHeight()>getWidth()) {
                if(inside(x,y,238,16,20,27) && !wallpaperPath.isEmpty()) return RESET_BACKGROUND;
                if(inside(x,y,262,12,87,37)) return BACKGROUND;
                if(inside(x,y,355,12,75,37)) return CLOSE_GAME;
                if(inside(x,y,35,439,380,70)) {
                    if(inside(x,y,47,460,65,34)) return SLOT_PREV;
                    if(inside(x,y,338,460,65,34)) return SLOT_NEXT;
                }
                if(inside(x,y,20,527,198,58)) return SAVE;
                if(inside(x,y,232,527,198,58)) return LOAD;
                if(inside(x,y,20,594,198,58)) return PAUSE;
                if(inside(x,y,232,594,198,58)) return REWIND;
                if(inside(x,y,20,661,198,58)) return BACKGROUND;
                if(inside(x,y,232,661,198,58)) return CLOSE_GAME;
                if(inside(x,y,112,160,226,230)) return ITEM_USE;
            } else {
                if(inside(x,y,552,17,34,34) && !wallpaperPath.isEmpty()) return RESET_BACKGROUND;
                if(inside(x,y,593,13,105,42)) return BACKGROUND;
                if(inside(x,y,704,13,72,42)) return CLOSE_GAME;
                if(inside(x,y,36,117,152,145)) {
                    if(inside(x,y,48,214,55,35)) return SLOT_PREV;
                    if(inside(x,y,121,214,55,35)) return SLOT_NEXT;
                }
                if(inside(x,y,28,354,118,66)) return SAVE;
                if(inside(x,y,152,354,118,66)) return LOAD;
                if(inside(x,y,276,354,118,66)) return PAUSE;
                if(inside(x,y,400,354,118,66)) return REWIND;
                if(inside(x,y,94,354,145,66)) return SAVE;
                if(inside(x,y,247,354,145,66)) return LOAD;
                if(inside(x,y,400,354,145,66)) return PAUSE;
                if(inside(x,y,553,354,153,66)) return REWIND;
                if(inside(x,y,290,93,220,245)) return ITEM_USE;
            }
            return 0;
        }

        private boolean inside(float x,float y,float rx,float ry,float rw,float rh) {
            return x>=rx && x<rx+rw && y>=ry && y<ry+rh;
        }

        private void send(int action,int pressed) {
            if(action==BACKGROUND) {
                if(getContext() instanceof PipeCleanActivity)
                    ((PipeCleanActivity)getContext()).openCompanionBackgroundPicker();
            } else if(action==RESET_BACKGROUND) {
                if(getContext() instanceof PipeCleanActivity)
                    ((PipeCleanActivity)getContext()).resetCompanionBackground();
            } else if(action==CLOSE_GAME) {
                if(game>=0) PipeCleanActivity.nativeCompanionAction(CLOSE_GAME,1);
            } else if(action!=0 && game>=0) {
                PipeCleanActivity.nativeCompanionAction(action,pressed);
            }
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            boolean portrait=getHeight()>getWidth();
            float dw=portrait?450f:800f, dh=portrait?800f:450f;
            float scale=Math.min(getWidth()/dw,getHeight()/dh);
            float ox=(getWidth()-dw*scale)*0.5f, oy=(getHeight()-dh*scale)*0.5f;
            float x=(e.getX()-ox)/scale, y=(e.getY()-oy)/scale;
            if(e.getActionMasked()==MotionEvent.ACTION_DOWN) {
                downAction=hitTest(x,y);
                if(downAction==ITEM_USE) { itemPressed=true; itemPressStart=SystemClock.uptimeMillis(); }
                if(downAction==REWIND) send(REWIND,1);
                invalidate();
                return true;
            }
            if(e.getActionMasked()==MotionEvent.ACTION_UP) {
                int up=hitTest(x,y);
                if(downAction==REWIND) send(REWIND,0);
                else if(up!=0 && up==downAction) send(up,1);
                if(downAction==ITEM_USE) { itemPressed=false; itemReleaseStart=SystemClock.uptimeMillis(); }
                downAction=0;
                invalidate();
                performClick();
                return true;
            }
            if(e.getActionMasked()==MotionEvent.ACTION_CANCEL) {
                if(downAction==REWIND) send(REWIND,0);
                if(downAction==ITEM_USE) { itemPressed=false; itemReleaseStart=SystemClock.uptimeMillis(); }
                downAction=0; invalidate(); return true;
            }
            return true;
        }

        @Override public boolean performClick() { super.performClick(); return true; }
    }
}
