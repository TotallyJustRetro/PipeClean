package com.pipeclean.app;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import java.util.Locale;

/** A second-screen utility deck, drawn from original PipeClean UI elements. */
final class PipeCleanCompanionPresentation extends Presentation {
    private final DeckView deck;

    PipeCleanCompanionPresentation(Context context, Display display) {
        super(context, display);
        deck = new DeckView(context);
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

    private static final class DeckView extends View {
        private static final int SAVE = 1, LOAD = 2, PAUSE = 3, REWIND = 4;
        private static final int PALETTE = 5, FILTER = 6, SLOT_PREV = 7, SLOT_NEXT = 8;
        private final Paint p = new Paint();
        private int game = -1, lives, stage = -1, world = -1, slot, palette, filter, item, downAction;
        private boolean paused;
        private static final String[] PALETTES = {
            "Classic Green", "Original DMG", "Pocket", "Black & White", "Ice Blue",
            "Cherry", "Gold", "Grape", "Mint", "Sepia", "Sunset", "Midnight",
            "Classic (color)", "Arcade (color)", "Candy (color)", "Ocean (color)"
        };
        private static final String[] FILTERS = {"None", "Game Boy LCD", "Pocket", "CRT TV", "Bloom", "HDR"};

        DeckView(Context context) {
            super(context);
            setFocusable(true);
            setClickable(true);
            p.setAntiAlias(false);
            p.setTypeface(Typeface.create("monospace", Typeface.BOLD));
            setContentDescription("PipeClean game status and utility controls");
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

        private boolean sml1() { return game == 1; }
        private boolean sml2() { return game == 2; }
        private int bg() { return sml1() ? 0xFF14251C : sml2() ? 0xFF241A2B : 0xFF171D2A; }
        private int panel() { return sml1() ? 0xFF263D2D : sml2() ? 0xFF48342D : 0xFF283347; }
        private int accent() { return sml1() ? 0xFFA8C98A : sml2() ? 0xFFFFC65C : 0xFF78DCE8; }
        private int ink() { return sml1() ? 0xFFE8F4D9 : sml2() ? 0xFFFFF1CF : 0xFFE8F4FF; }
        private int muted() { return sml1() ? 0xFF9FB59A : sml2() ? 0xFFD5B995 : 0xFFAAB9D2; }
        private int edge() { return sml1() ? 0xFF617D5B : sml2() ? 0xFF9A6A42 : 0xFF536A8E; }

        @Override protected void onDraw(Canvas c) {
            super.onDraw(c);
            c.drawColor(bg());
            boolean portrait = getHeight() > getWidth();
            float w = portrait ? 450f : 800f, h = portrait ? 800f : 450f;
            float scale = Math.min(getWidth() / w, getHeight() / h);
            c.save();
            c.translate((getWidth() - w * scale) * 0.5f, (getHeight() - h * scale) * 0.5f);
            c.scale(scale, scale);
            if (portrait) drawPortrait(c); else drawLandscape(c);
            c.restore();
        }

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
            if (item == 0) return "EMPTY";
            if (sml1()) {
                if (item == 1) return "MUSHROOM";
                if (item == 2) return "SUPERBALL FLOWER";
            }
            if (sml2()) {
                if (item == 1) return "MUSHROOM";
                if (item == 2) return "FIRE FLOWER";
                if (item == 3) return "CARROT";
            }
            return "ITEM";
        }
        private String paletteText() { return PALETTES[Math.max(0, Math.min(PALETTES.length - 1, palette))]; }
        private String filterText() { return FILTERS[Math.max(0, Math.min(FILTERS.length - 1, filter))]; }

        private void drawLandscape(Canvas c) {
            text(c, "PIPECLEAN  /  " + title(), 24, 34, 18, accent());
            textRight(c, "SECOND-SCREEN DECK", 776, 32, 10, muted());
            rect(c, 24, 44, 752, 5, accent());
            card(c, 24, 58, 225, 112, "LIVES");
            text(c, String.format(Locale.US, "x %02d", lives), 42, 133, 32, ink());
            text(c, "PLAYER 1", 42, 153, 10, muted());
            card(c, 261, 58, 250, 112, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE");
            text(c, stageText(), 279, 132, 25, ink());
            text(c, "LIVE GAME DATA", 279, 153, 10, muted());
            card(c, 523, 58, 253, 112, "RESERVE ITEM");
            drawItem(c, 543, 85, 77, item);
            textFit(c, itemName(), 634, 112, 132, item == 0 ? 20 : 14, item == 0 ? muted() : ink());
            text(c, item == 0 ? "Nothing stored" : "Power-up reserve", 634, 136, 10, muted());

            float x = 24, y = 190, w = 179, h = 94, gap = 12;
            button(c, x, y, w, h, "SAVE STATE", "WRITE SLOT " + (slot + 1), SAVE, false);
            button(c, x + w + gap, y, w, h, "LOAD STATE", "RESTORE SLOT", LOAD, false);
            button(c, x + 2*(w+gap), y, w, h, paused ? "RESUME" : "PAUSE",
                   paused ? "CONTINUE GAME" : "HOLD GAME", PAUSE, false);
            button(c, x + 3*(w+gap), y, w, h, "REWIND", "HOLD TO REWIND", REWIND, true);

            button(c, 24, 300, 235, 78, "PALETTE", paletteText(), PALETTE, false);
            button(c, 282, 300, 235, 78, "FILTER", filterText(), FILTER, false);
            card(c, 540, 300, 236, 78, "SAVE SLOT");
            text(c, "SLOT " + (slot + 1), 658, 350, 18, ink(), true);
            buttonArrow(c, 550, 321, 70, 45, SLOT_PREV, true);
            buttonArrow(c, 696, 321, 70, 45, SLOT_NEXT, false);
            text(c, "PHYSICAL CONTROLLER ACTIVE / NO VIRTUAL BUTTONS", 24, 420, 10, muted());
            textRight(c, paused ? "PAUSED" : "RUNNING", 776, 420, 10, accent());
        }

        private void drawPortrait(Canvas c) {
            text(c, "PIPECLEAN", 20, 28, 17, accent());
            textRight(c, title(), 430, 27, 9, muted());
            rect(c, 20, 39, 410, 5, accent());
            card(c, 20, 55, 195, 112, "LIVES");
            text(c, String.format(Locale.US, "x %02d", lives), 35, 123, 29, ink());
            text(c, "PLAYER 1", 35, 145, 10, muted());
            card(c, 225, 55, 205, 112, sml1() ? "WORLD / LEVEL" : "STAGE");
            text(c, stageText(), 239, 123, sml2() ? 17 : 26, ink());
            text(c, "LIVE DATA", 239, 145, 9, muted());
            card(c, 20, 179, 410, 140, "RESERVE ITEM");
            drawItem(c, 38, 220, 80, item);
            textFit(c, itemName(), 140, 255, 275, item == 0 ? 23 : 18, item == 0 ? muted() : ink());
            text(c, item == 0 ? "No stored item" : "Power-up reserve", 140, 280, 11, muted());
            button(c, 20, 339, 195, 88, "SAVE STATE", "WRITE SLOT " + (slot + 1), SAVE, false);
            button(c, 225, 339, 205, 88, "LOAD STATE", "RESTORE SLOT", LOAD, false);
            button(c, 20, 437, 195, 88, paused ? "RESUME" : "PAUSE",
                   paused ? "CONTINUE GAME" : "HOLD GAME", PAUSE, false);
            button(c, 225, 437, 205, 88, "REWIND", "HOLD TO REWIND", REWIND, true);
            button(c, 20, 535, 195, 78, "PALETTE", paletteText(), PALETTE, false);
            button(c, 225, 535, 205, 78, "FILTER", filterText(), FILTER, false);
            card(c, 20, 625, 410, 78, "SAVE SLOT");
            text(c, "SLOT " + (slot + 1), 225, 675, 18, ink(), true);
            buttonArrow(c, 31, 647, 100, 42, SLOT_PREV, true);
            buttonArrow(c, 319, 647, 100, 42, SLOT_NEXT, false);
            text(c, "PHYSICAL CONTROLS", 20, 755, 10, muted());
            textRight(c, paused ? "PAUSED" : "RUNNING", 430, 755, 10, accent());
        }

        private void card(Canvas c, float x, float y, float w, float h, String label) {
            p.setColor(panel()); c.drawRect(x, y, x+w, y+h, p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2);
            c.drawRect(x+1, y+1, x+w-1, y+h-1, p); p.setStyle(Paint.Style.FILL);
            rect(c, x+9, y+9, w-18, 2, accent());
            text(c, label, x+12, y+28, 10, muted());
        }

        private void button(Canvas c, float x, float y, float w, float h,
                            String label, String sub, int action, boolean hold) {
            p.setColor(action == downAction ? accent() : panel());
            c.drawRect(x,y,x+w,y+h,p);
            p.setColor(action == downAction ? ink() : edge());
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2);
            c.drawRect(x+1,y+1,x+w-1,y+h-1,p); p.setStyle(Paint.Style.FILL);
            drawIcon(c, x+12, y+h*0.5f-10, action);
            textFit(c, label, x+45, y+h*0.5f-1, w-48, 13, ink());
            textFit(c, sub, x+45, y+h*0.5f+21, w-48, 8, muted());
            if (hold) textRight(c, "HOLD", x+w-8, y+h-9, 7, accent());
        }

        private void buttonArrow(Canvas c, float x, float y, float w, float h, int action, boolean left) {
            p.setColor(action == downAction ? accent() : panel()); c.drawRect(x,y,x+w,y+h,p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2);
            c.drawRect(x+1,y+1,x+w-1,y+h-1,p); p.setStyle(Paint.Style.FILL);
            Path path = new Path(); float cx=x+w*0.5f, cy=y+h*0.5f;
            if (left) { path.moveTo(cx+10,cy-13); path.lineTo(cx-9,cy); path.lineTo(cx+10,cy+13); }
            else { path.moveTo(cx-10,cy-13); path.lineTo(cx+9,cy); path.lineTo(cx-10,cy+13); }
            path.close(); p.setColor(ink()); c.drawPath(path,p);
        }

        private void drawIcon(Canvas c, float x, float y, int action) {
            p.setColor(accent());
            if (action == SAVE) {
                c.drawRect(x,y-8,x+20,y+9,p); p.setColor(bg());
                c.drawRect(x+4,y-6,x+15,y-1,p); c.drawRect(x+5,y+3,x+15,y+7,p);
            } else if (action == LOAD) {
                c.drawRect(x+2,y+6,x+20,y+9,p); c.drawRect(x+10,y-8,x+13,y+4,p);
                Path path=new Path(); path.moveTo(x+5,y); path.lineTo(x+11,y+7); path.lineTo(x+18,y);
                path.close(); c.drawPath(path,p);
            } else if (action == PAUSE) {
                c.drawRect(x+3,y-9,x+8,y+9,p); c.drawRect(x+14,y-9,x+19,y+9,p);
            } else if (action == REWIND) {
                c.drawRect(x+1,y-10,x+4,y+10,p);
                Path path=new Path(); path.moveTo(x+5,y); path.lineTo(x+20,y-10); path.lineTo(x+20,y+10);
                path.close(); c.drawPath(path,p);
            } else if (action == PALETTE) {
                c.drawRect(x+1,y-8,x+8,y-1,p); c.drawRect(x+12,y-8,x+19,y-1,p);
                c.drawRect(x+1,y+3,x+8,y+10,p); c.drawRect(x+12,y+3,x+19,y+10,p);
            } else if (action == FILTER) {
                c.drawRect(x,y-7,x+21,y-4,p); c.drawRect(x,y,x+21,y+3,p);
                c.drawRect(x,y+7,x+21,y+10,p);
            }
        }

        private void drawItem(Canvas c, float x, float y, float size, int itemId) {
            p.setColor(bg()); c.drawRect(x,y,x+size,y+size,p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2);
            c.drawRect(x+1,y+1,x+size-1,y+size-1,p); p.setStyle(Paint.Style.FILL);
            if (itemId == 0) return;
            // Original pixel-art power-up icons, not extracted game sprites.
            float u=size/10f;
            if (itemId == 1) {
                // Mushroom
                p.setColor(sml1()?0xFFDCE6D1:0xFFFFF0C9);
                c.drawRect(x+4*u,y+5*u,x+6*u,y+8*u,p);
                c.drawRect(x+3*u,y+7*u,x+7*u,y+8*u,p);
                p.setColor(sml1()?0xFF698D55:0xFFE5523C);
                c.drawRect(x+2*u,y+3*u,x+8*u,y+5*u,p);
                c.drawRect(x+3*u,y+2*u,x+7*u,y+3*u,p);
                p.setColor(sml1()?0xFFF3F6E7:0xFFFFEAB5);
                c.drawRect(x+3*u,y+3*u,x+4*u,y+4*u,p);
                c.drawRect(x+6*u,y+3*u,x+7*u,y+4*u,p);
            } else if (itemId == 2) {
                // Superball Flower in SML1; Fire Flower in SML2.
                p.setColor(sml1()?0xFFDDEAD4:0xFF6FC86B);
                c.drawRect(x+4*u,y+5*u,x+6*u,y+9*u,p);
                c.drawRect(x+2*u,y+7*u,x+5*u,y+8*u,p);
                p.setColor(sml1()?0xFFCBDAC5:0xFFE84A3F);
                c.drawRect(x+3*u,y+2*u,x+7*u,y+4*u,p);
                c.drawRect(x+2*u,y+3*u,x+8*u,y+6*u,p);
                p.setColor(sml1()?0xFFF3F6E7:0xFFFFE28A);
                c.drawRect(x+4*u,y+3*u,x+6*u,y+5*u,p);
            } else if (itemId == 3 && sml2()) {
                // Carrot power-up
                p.setColor(0xFFFFA348);
                c.drawRect(x+3*u,y+3*u,x+7*u,y+4*u,p);
                c.drawRect(x+4*u,y+4*u,x+7*u,y+6*u,p);
                c.drawRect(x+5*u,y+6*u,x+6*u,y+8*u,p);
                p.setColor(0xFF69C56B);
                c.drawRect(x+3*u,y+1*u,x+5*u,y+3*u,p);
                c.drawRect(x+5*u,y+1*u,x+7*u,y+3*u,p);
                c.drawRect(x+4*u,y+2*u,x+6*u,y+4*u,p);
            }
        }

        private void rect(Canvas c,float x,float y,float w,float h,int color) {
            p.setColor(color); c.drawRect(x,y,x+w,y+h,p);
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
            p.setTextSize(size); while(p.measureText(s)>max && size>7){size-=0.5f;p.setTextSize(size);}
            text(c,s,x,y,size,color);
        }

        private int hitTest(float x,float y) {
            if(getHeight()>getWidth()) {
                if(inside(x,y,20,339,195,88)) return SAVE;
                if(inside(x,y,225,339,205,88)) return LOAD;
                if(inside(x,y,20,437,195,88)) return PAUSE;
                if(inside(x,y,225,437,205,88)) return REWIND;
                if(inside(x,y,20,535,195,78)) return PALETTE;
                if(inside(x,y,225,535,205,78)) return FILTER;
                if(inside(x,y,20,625,205,78)) return SLOT_PREV;
                if(inside(x,y,225,625,205,78)) return SLOT_NEXT;
            } else {
                if(inside(x,y,24,190,179,94)) return SAVE;
                if(inside(x,y,215,190,179,94)) return LOAD;
                if(inside(x,y,406,190,179,94)) return PAUSE;
                if(inside(x,y,597,190,179,94)) return REWIND;
                if(inside(x,y,24,300,235,78)) return PALETTE;
                if(inside(x,y,282,300,235,78)) return FILTER;
                if(inside(x,y,540,300,84,78)) return SLOT_PREV;
                if(inside(x,y,692,300,84,78)) return SLOT_NEXT;
            }
            return 0;
        }
        private boolean inside(float x,float y,float rx,float ry,float rw,float rh) {
            return x>=rx && x<rx+rw && y>=ry && y<ry+rh;
        }
        private void send(int action,int pressed) {
            if(action!=0 && game>=0) PipeCleanActivity.nativeCompanionAction(action,pressed);
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            boolean portrait=getHeight()>getWidth();
            float dw=portrait?450f:800f, dh=portrait?800f:450f;
            float scale=Math.min(getWidth()/dw,getHeight()/dh);
            float ox=(getWidth()-dw*scale)*0.5f, oy=(getHeight()-dh*scale)*0.5f;
            float x=(e.getX()-ox)/scale, y=(e.getY()-oy)/scale;
            if(e.getActionMasked()==MotionEvent.ACTION_DOWN) {
                downAction=hitTest(x,y);
                if(downAction==REWIND) send(REWIND,1);
                invalidate(); return true;
            }
            if(e.getActionMasked()==MotionEvent.ACTION_UP) {
                int up=hitTest(x,y);
                if(downAction==REWIND) send(REWIND,0);
                else if(up!=0 && up==downAction) send(up,1);
                downAction=0; invalidate(); performClick(); return true;
            }
            if(e.getActionMasked()==MotionEvent.ACTION_CANCEL) {
                if(downAction==REWIND) send(REWIND,0);
                downAction=0; invalidate(); return true;
            }
            return true;
        }
        @Override public boolean performClick() { super.performClick(); return true; }
    }
}
