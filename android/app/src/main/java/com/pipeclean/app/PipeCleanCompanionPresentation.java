package com.pipeclean.app;

import android.app.Activity;
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
import android.media.MediaPlayer;
import android.os.SystemClock;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** A playful, game-specific second-screen companion deck. All artwork is original UI art. */
final class PipeCleanCompanionPresentation extends Presentation {
    private final Activity host;
    private final DeckView deck;
    private final GameFrameView gameFrameView;
    private boolean pipeSwapMode;

    PipeCleanCompanionPresentation(Context context, Display display) {
        super(context, display);
        // The Companion Deck is paired with the app's primary screen, not a pop-up.
        // Back/B and outside touches must never dismiss the secondary-screen surface.
        setCancelable(false);
        setCanceledOnTouchOutside(false);
        host = (Activity) context;
        deck = new DeckView(context);
        gameFrameView = new GameFrameView(context);
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
                     int slot, int palette, int filter, int item, int drMarioScore,
                     int drMarioLevel, int drMarioSpeed, int drMarioCapsule,
                     int warioCoins, int warioHearts, int warioPower,
                     int warioTreasures, int warioTime) {
        deck.setState(game, lives, stage, world, paused, slot, palette, filter, item,
            drMarioScore, drMarioLevel, drMarioSpeed, drMarioCapsule,
            warioCoins, warioHearts, warioPower, warioTreasures, warioTime);
    }

    void showIdleState() { deck.showIdleState(); }

    void setLauncherDashboardState(int selectedGame, String[] gameNames,
                                    int[] recentGames, int[] playSeconds,
                                    int previewWidth, int previewHeight, int[] previewPixels,
                                    boolean dualScreenEnabled) {
        deck.setLauncherDashboardState(selectedGame, gameNames, recentGames, playSeconds,
            previewWidth, previewHeight, previewPixels, dualScreenEnabled);
    }

    void setMusicPlaylist(String currentTitle, String[] playlistNames,
                          int selectedTrack, boolean playing,
                          int notificationSerial, boolean notificationAllowed) {
        deck.setMusicPlaylist(currentTitle, playlistNames, selectedTrack, playing,
            notificationSerial, notificationAllowed);
    }

    void setBackgroundFile(String path) { deck.setBackgroundFile(path); }

    void setDeckLayout(int layoutGame, int[] values, String[] paths,
                       boolean editorPreview, int editorPreviewItem) {
        deck.setDeckLayout(layoutGame, values, paths, editorPreview, editorPreviewItem);
    }

    void updateSaveSlots(int game, int selectedSlot, String[] statePaths, String[] thumbnailPaths) {
        deck.setSaveSlots(game, selectedSlot, statePaths, thumbnailPaths);
    }

    void setPipeSwapMode(boolean enabled) {
        if (pipeSwapMode == enabled) return;
        pipeSwapMode = enabled;
        if (enabled) {
            // Put the actual interactive deck on the primary screen while the
            // game frame occupies this secondary Presentation.
            detachFromParent(deck);
            host.addContentView(deck, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            setContentView(gameFrameView);
        } else {
            // Return the deck to its usual secondary-screen home.
            detachFromParent(deck);
            setContentView(deck);
            gameFrameView.clear();
        }
    }

    void updateGameFrame(int width, int height, int[] pixels) {
        gameFrameView.updateGameFrame(width, height, pixels);
    }

    private static void detachFromParent(View view) {
        if (view.getParent() instanceof ViewGroup)
            ((ViewGroup) view.getParent()).removeView(view);
    }

    private static final class GameFrameView extends View {
        private final Paint framePaint = new Paint();
        private final Paint messagePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private Bitmap frame;

        GameFrameView(Context context) {
            super(context);
            framePaint.setFilterBitmap(false);
            framePaint.setAntiAlias(false);
            messagePaint.setColor(0xFFEAF5FF);
            messagePaint.setTextSize(16f);
            messagePaint.setTextAlign(Paint.Align.CENTER);
            messagePaint.setTypeface(Typeface.create("monospace", Typeface.BOLD));
        }

        void updateGameFrame(int width, int height, int[] pixels) {
            if (width <= 0 || height <= 0 || pixels == null ||
                (long) width * (long) height != pixels.length ||
                (long) width * (long) height > 1048576L) return;
            if (frame == null || frame.isRecycled() ||
                frame.getWidth() != width || frame.getHeight() != height) {
                if (frame != null && !frame.isRecycled()) frame.recycle();
                frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            }
            frame.setPixels(pixels, 0, width, 0, 0, width, height);
            postInvalidateOnAnimation();
        }

        void clear() {
            if (frame != null && !frame.isRecycled()) frame.recycle();
            frame = null;
            invalidate();
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            canvas.drawColor(0xFF07111F);
            if (frame == null || frame.isRecycled()) {
                canvas.drawText("Preparing gameplay display…",
                    getWidth() * 0.5f, getHeight() * 0.5f, messagePaint);
                return;
            }
            float fit = Math.min(getWidth() / (float) frame.getWidth(),
                                 getHeight() / (float) frame.getHeight());
            float dw = frame.getWidth() * fit;
            float dh = frame.getHeight() * fit;
            float left = (getWidth() - dw) * 0.5f;
            float top = (getHeight() - dh) * 0.5f;
            canvas.drawBitmap(frame, null, new RectF(left, top, left + dw, top + dh),
                              framePaint);
        }
    }

    private static final class DeckView extends View {
        private static final int SAVE = 1, LOAD = 2, PAUSE = 3, REWIND = 4;
        private static final int PALETTE = 5, FILTER = 6, SLOT_PREV = 7, SLOT_NEXT = 8;
        private static final int ITEM_USE = 9, BACKGROUND = 10, RESET_BACKGROUND = 11, CLOSE_GAME = 12;

        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int game = -1, lives, stage = -1, world = -1, slot, palette, filter, item, downAction;
        private int drMarioScore = -1, drMarioLevel = -1, drMarioSpeed = -1, drMarioCapsule = 0xFF;
        private int warioCoins = -1, warioHearts = -1, warioPower = -1;
        private int warioTreasureMask = -1, warioTime = -1;
        private long wario2TreasureMask = -1L, wario2PictureMask = -1L;
        private long previousWario2TreasureMask = -1L, previousWario2PictureMask = -1L;
        private boolean wario2BaselineSet, wario2AnimationActive;
        private int wario2AnimationId = -1, wario2AnimationKind;
        private long wario2AnimationStart;
        private int previousWario2Stage = -1;
        private final String[] expeditionLog = new String[5];
        private boolean warioBaselineSet, treasureAnimationActive;
        private int previousWarioCoins = -1, previousWarioHearts = -1, previousWarioPower = -1;
        private int previousWarioTreasureMask = -1, previousWarioStage = -1, previousWarioLives = -1;
        private int treasureAnimationId = -1;
        private long treasureAnimationStart;
        private static final long TREASURE_ANIMATION_MS = 1850L;
        private boolean paused, itemPressed, dashboardMode = true, dashboardDualScreen;
        private long itemPressStart, itemReleaseStart;
        private static final int ITEM_ANIM_IDLE = 0, ITEM_ANIM_ARRIVE = 1, ITEM_ANIM_SEND_OFF = 2;
        private static final long ITEM_ARRIVE_MS = 460L, ITEM_SEND_OFF_MS = 360L;
        private int displayedItem, lastObservedItem, itemAnimationKind;
        private long itemAnimationStart;
        private int dashboardGame = -1;
        private String[] dashboardGameNames = new String[0];
        private int[] dashboardRecentGames = new int[0], dashboardPlaySeconds = new int[0];
        private Bitmap dashboardPreview;
        private String musicTitle = "PipeClean Chiptune";
        private String[] musicPlaylist = new String[0];
        private int musicSelectedTrack;
        private boolean musicPlaying = true;
        private int musicNoticeSerial = -1;
        private boolean musicNoticeVisible, musicNoticeAllowed;
        private long musicNoticeStartedAt;
        private final boolean[] slotOccupied = new boolean[10];
        private final Bitmap[] slotThumbnails = new Bitmap[10];
        private final long[] slotSavedAt = new long[10];
        private final long[] slotStateModified = new long[10];
        private final long[] slotThumbModified = new long[10];
        private final String[] slotStatePaths = new String[10];
        private final String[] slotThumbnailPaths = new String[10];
        private final String[] slotTimeLabels = new String[10];
        private float animatedSlot = -1f;
        private boolean timelineTouchActive, timelineMoved;
        private float timelineDownX, timelineDownY, timelineStartFocus, timelineDragFocus;
        private static final int SLOT_SELECT_BASE = 20;
        private Bitmap wallpaperBitmap;
        private Movie wallpaperMovie;
        private String wallpaperPath = "";

        private static final int LAYOUT_GAME_COUNT = 7;
        private static final int LAYOUT_DECORATION_COUNT = 16;
        private static final int LAYOUT_ELEMENT_COUNT = 5 + LAYOUT_DECORATION_COUNT;
        private static final int LAYOUT_PATH_COUNT = 38;
        private static final int LAYOUT_STATUS_VALUE_BASE = 49 + LAYOUT_PATH_COUNT;
        private static final int LAYOUT_EXTENDED_DECORATION_COUNT = LAYOUT_DECORATION_COUNT - 8;
        private static final int LAYOUT_EXTENDED_DECORATION_VALUE_BASE = LAYOUT_STATUS_VALUE_BASE + 4;
        private static final int LAYOUT_EXTENDED_DECORATION_SCALE_Y_BASE =
            LAYOUT_EXTENDED_DECORATION_VALUE_BASE + LAYOUT_EXTENDED_DECORATION_COUNT * 3;
        private static final int LAYOUT_VALUE_COUNT =
            LAYOUT_EXTENDED_DECORATION_SCALE_Y_BASE + LAYOUT_EXTENDED_DECORATION_COUNT;
        private final int[][] layoutValues = new int[LAYOUT_GAME_COUNT][LAYOUT_VALUE_COUNT];
        private final String[][] layoutPaths = new String[LAYOUT_GAME_COUNT][LAYOUT_PATH_COUNT];
        private final Bitmap[][] layoutBitmaps = new Bitmap[LAYOUT_GAME_COUNT][LAYOUT_PATH_COUNT];
        private final Movie[][] layoutMovies = new Movie[LAYOUT_GAME_COUNT][LAYOUT_PATH_COUNT];
        private final String[][] loadedLayoutImagePaths = new String[LAYOUT_GAME_COUNT][LAYOUT_PATH_COUNT];
        private int layoutPreviewGame = -1;
        private boolean layoutEditorPreview;
        private int layoutPreviewItem;
        private MediaPlayer deckSoundPlayer;
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
                      int slot, int palette, int filter, int item, int drMarioScore,
                      int drMarioLevel, int drMarioSpeed, int drMarioCapsule,
                      int warioCoins, int warioHearts, int warioPower,
                      int warioTreasureMask, int warioTime) {
            layoutEditorPreview = false;
            dashboardMode = false;
            this.game = game;
            boolean wl2 = game == 4 || game == 6;
            this.lives = (game == 3 || wl2) && lives < 0 ? -1 : Math.max(0, Math.min(99, lives));
            this.stage = stage;
            this.world = world;
            this.paused = paused;
            this.drMarioScore = wl2 ? drMarioScore : (drMarioScore >= 0 ? Math.min(99999999, drMarioScore) : -1);
            this.drMarioLevel = wl2 ? drMarioLevel : (drMarioLevel >= 0 && drMarioLevel <= 99 ? drMarioLevel : -1);
            this.drMarioSpeed = drMarioSpeed;
            this.drMarioCapsule = wl2 ? drMarioCapsule :
                (drMarioCapsule >= 0 && drMarioCapsule <= 0x17 ? drMarioCapsule : 0xFF);
            this.warioCoins = warioCoins >= 0 && warioCoins <= 999999 ? warioCoins : -1;
            this.warioHearts = warioHearts >= 0 && warioHearts <= 999999 ? warioHearts : -1;
            this.warioPower = warioPower >= 0 && warioPower <= 4 ? warioPower : -1;
            this.warioTreasureMask = game == 3 && warioTreasureMask >= 0 ? (warioTreasureMask & 0xFFFF) : -1;
            this.warioTime = warioTime >= 0 && warioTime <= 999 ? warioTime : -1;
            this.wario2TreasureMask = wl2 && drMarioLevel >= 0
                ? (((long)drMarioLevel & 0x3FFFFL) << 32) | ((long)drMarioScore & 0xFFFFFFFFL) : -1L;
            this.wario2PictureMask = wl2 && drMarioCapsule >= 0
                ? (((long)drMarioCapsule & 0x3FFFFL) << 32) | ((long)drMarioSpeed & 0xFFFFFFFFL) : -1L;
            observeWarioState();
            this.slot = Math.max(0, Math.min(9, slot));
            if (animatedSlot < 0f) animatedSlot = this.slot;
            this.palette = Math.max(0, Math.min(PALETTES.length - 1, palette));
            this.filter = Math.max(0, Math.min(FILTERS.length - 1, filter));
            observeReserveItem(item);
            this.item = item;
            postInvalidate();
        }

        private void observeReserveItem(int nextItem) {
            nextItem = Math.max(0, Math.min(3, nextItem));
            if (nextItem == lastObservedItem) return;
            long now = SystemClock.uptimeMillis();
            if (nextItem > 0) {
                displayedItem = nextItem;
                itemAnimationKind = ITEM_ANIM_ARRIVE;
                itemAnimationStart = now;
            } else if (lastObservedItem > 0) {
                if (itemAnimationKind != ITEM_ANIM_SEND_OFF || displayedItem != lastObservedItem) {
                    displayedItem = lastObservedItem;
                    itemAnimationKind = ITEM_ANIM_SEND_OFF;
                    itemAnimationStart = now;
                }
            } else {
                displayedItem = 0;
                itemAnimationKind = ITEM_ANIM_IDLE;
            }
            lastObservedItem = nextItem;
        }

        private String warioPowerName(int power) {
            switch (power) {
                case 0: return "SMALL";
                case 1: return "GARLIC";
                case 2: return "BULL";
                case 3: return "JET";
                case 4: return "DRAGON";
                default: return "UNKNOWN";
            }
        }

        private void pushExpeditionLog(String entry) {
            for (int i = expeditionLog.length - 1; i > 0; i--)
                expeditionLog[i] = expeditionLog[i - 1];
            expeditionLog[0] = entry;
        }

        private int warioTreasureCount() {
            return warioTreasureMask < 0 ? 0 : Math.min(15, Integer.bitCount(warioTreasureMask & 0xFFFF));
        }

        private void observeWarioState() {
            if (wario2()) {
                observeWario2State();
                warioBaselineSet = false;
                treasureAnimationActive = false;
                return;
            }
            if (game != 3) {
                warioBaselineSet = false;
                wario2BaselineSet = false;
                treasureAnimationActive = false;
                wario2AnimationActive = false;
                return;
            }
            if (!warioBaselineSet) {
                for (int i = 0; i < expeditionLog.length; i++) expeditionLog[i] = null;
                pushExpeditionLog("EXPEDITION LINKED");
                if (stage >= 0)
                    pushExpeditionLog(String.format(Locale.US, "COURSE %02X ENTERED", stage & 0xFF));
                previousWarioCoins = warioCoins;
                previousWarioHearts = warioHearts;
                previousWarioPower = warioPower;
                previousWarioTreasureMask = warioTreasureMask;
                previousWarioStage = stage;
                previousWarioLives = lives;
                warioBaselineSet = true;
                return;
            }
            if (stage >= 0 && previousWarioStage >= 0 && stage != previousWarioStage)
                pushExpeditionLog(String.format(Locale.US, "COURSE %02X ENTERED", stage & 0xFF));

            if (warioTreasureMask >= 0 && previousWarioTreasureMask >= 0) {
                int oldMask = previousWarioTreasureMask & 0xFFFF;
                int nextMask = warioTreasureMask & 0xFFFF;
                if ((nextMask & oldMask) != oldMask) {
                    // Save-state loads/rewinds can roll the bitfield backward. Never
                    // celebrate a bit that was already known in the previous timeline.
                    treasureAnimationActive = false;
                } else {
                    int gained = (nextMask & ~oldMask) & 0xFFFF;
                    if (gained != 0) {
                        for (int bit = 0; bit < 16; bit++) {
                            if ((gained & (1 << bit)) != 0)
                                pushExpeditionLog(String.format(Locale.US, "TREASURE %02d FOUND", bit + 1));
                        }
                        treasureAnimationId = 32 - Integer.numberOfLeadingZeros(gained);
                        treasureAnimationStart = SystemClock.uptimeMillis();
                        treasureAnimationActive = true;
                        postInvalidateOnAnimation();
                    }
                }
            }
            if (warioPower >= 0 && previousWarioPower >= 0 && warioPower != previousWarioPower)
                pushExpeditionLog("POWER: " + warioPowerName(warioPower));
            if (lives > previousWarioLives && previousWarioLives >= 0)
                pushExpeditionLog("EXTRA LIFE + " + (lives - previousWarioLives));
            if (warioHearts >= 0 && previousWarioHearts >= 0 && warioHearts > previousWarioHearts)
                pushExpeditionLog("HEARTS RESTORED");
            if (warioCoins >= 0 && previousWarioCoins >= 0 &&
                warioCoins / 100 > previousWarioCoins / 100)
                pushExpeditionLog(String.format(Locale.US, "COIN BANK %05d", warioCoins / 100 * 100));

            if (warioCoins >= 0) previousWarioCoins = warioCoins;
            if (warioHearts >= 0) previousWarioHearts = warioHearts;
            if (warioPower >= 0) previousWarioPower = warioPower;
            if (warioTreasureMask >= 0) previousWarioTreasureMask = warioTreasureMask;
            if (stage >= 0) previousWarioStage = stage;
            if (lives >= 0) previousWarioLives = lives;
        }


        private int wario2TreasureCount() {
            return wario2TreasureMask < 0 ? 0 : Long.bitCount(wario2TreasureMask & 0x3FFFFFFFFFFFFL);
        }

        private int wario2PictureCount() {
            return wario2PictureMask < 0 ? 0 : Long.bitCount(wario2PictureMask & 0x3FFFFFFFFFFFFL);
        }

        private void observeWario2State() {
            if (wario2TreasureMask < 0 || wario2PictureMask < 0) {
                wario2BaselineSet = false;
                wario2AnimationActive = false;
                return;
            }
            if (!wario2BaselineSet) {
                for (int i = 0; i < expeditionLog.length; i++) expeditionLog[i] = null;
                pushExpeditionLog("EXPEDITION LINKED");
                if (stage >= 0)
                    pushExpeditionLog(String.format(Locale.US, "STAGE %02X ENTERED", stage & 0xFF));
                previousWario2Stage = stage;
                previousWario2TreasureMask = wario2TreasureMask;
                previousWario2PictureMask = wario2PictureMask;
                previousWarioCoins = warioCoins;
                previousWarioHearts = warioHearts;
                wario2BaselineSet = true;
                return;
            }
            if (stage >= 0 && previousWario2Stage >= 0 && stage != previousWario2Stage)
                pushExpeditionLog(String.format(Locale.US, "STAGE %02X ENTERED", stage & 0xFF));

            long oldT = previousWario2TreasureMask, nextT = wario2TreasureMask;
            long oldP = previousWario2PictureMask, nextP = wario2PictureMask;
            if ((nextT & oldT) != oldT || (nextP & oldP) != oldP) {
                // A state load or rewind can legitimately roll these flags backward.
                wario2AnimationActive = false;
            } else {
                long gainedT = nextT & ~oldT, gainedP = nextP & ~oldP;
                for (int bit = 0; bit < 50; bit++) {
                    if ((gainedT & (1L << bit)) != 0)
                        pushExpeditionLog(String.format(Locale.US, "TREASURE %02d FOUND", bit + 1));
                    if ((gainedP & (1L << bit)) != 0)
                        pushExpeditionLog(String.format(Locale.US, "PICTURE PIECE %02d", bit + 1));
                }
                if (gainedT != 0 || gainedP != 0) {
                    boolean picture = gainedT == 0;
                    long gained = picture ? gainedP : gainedT;
                    wario2AnimationKind = picture ? 2 : 1;
                    wario2AnimationId = Long.numberOfTrailingZeros(gained) + 1;
                    wario2AnimationStart = SystemClock.uptimeMillis();
                    wario2AnimationActive = true;
                    postInvalidateOnAnimation();
                }
            }
            if (warioCoins >= 0 && previousWarioCoins >= 0 && warioCoins > previousWarioCoins)
                pushExpeditionLog(String.format(Locale.US, "COIN BANK %06d", warioCoins));
            if (warioHearts >= 0 && previousWarioHearts >= 0 && warioHearts > previousWarioHearts)
                pushExpeditionLog("OVERWORLD COINS +");

            previousWario2Stage = stage;
            previousWario2TreasureMask = wario2TreasureMask;
            previousWario2PictureMask = wario2PictureMask;
            previousWarioCoins = warioCoins;
            previousWarioHearts = warioHearts;
        }

        private void startReserveItemSendOff(int itemId) {
            if (itemId <= 0) return;
            displayedItem = itemId;
            itemAnimationKind = ITEM_ANIM_SEND_OFF;
            itemAnimationStart = SystemClock.uptimeMillis();
        }

        void showIdleState() {
            layoutEditorPreview = false;
            dashboardMode = true;
            game = -1;
            lives = 0;
            stage = -1;
            world = -1;
            paused = false;
            drMarioScore = -1;
            drMarioLevel = -1;
            drMarioSpeed = -1;
            drMarioCapsule = 0xFF;
            warioCoins = warioHearts = warioPower = warioTime = -1;
            warioTreasureMask = -1;
            wario2TreasureMask = wario2PictureMask = -1L;
            wario2BaselineSet = false;
            wario2AnimationActive = false;
            previousWario2TreasureMask = previousWario2PictureMask = -1L;
            warioBaselineSet = false;
            treasureAnimationActive = false;
            for (int i = 0; i < expeditionLog.length; i++) expeditionLog[i] = null;
            item = 0;
            displayedItem = 0;
            lastObservedItem = 0;
            itemAnimationKind = ITEM_ANIM_IDLE;
            itemAnimationStart = 0;
            downAction = 0;
            itemPressed = false;
            postInvalidate();
        }

        void setLauncherDashboardState(int selectedGame, String[] gameNames,
                                       int[] recentGames, int[] playSeconds,
                                       int previewWidth, int previewHeight, int[] previewPixels,
                                       boolean dualScreenEnabled) {
            dashboardMode = true;
            game = -1;
            if (dashboardGame != selectedGame && dashboardPreview != null) {
                if (!dashboardPreview.isRecycled()) dashboardPreview.recycle();
                dashboardPreview = null;
            }
            dashboardGame = selectedGame;
            dashboardGameNames = gameNames != null ? gameNames.clone() : new String[0];
            dashboardRecentGames = recentGames != null ? recentGames.clone() : new int[0];
            dashboardPlaySeconds = playSeconds != null ? playSeconds.clone() : new int[0];
            dashboardDualScreen = dualScreenEnabled;
            if (!dashboardDualScreen) musicNoticeVisible = false;
            if (previewPixels != null && previewWidth > 0 && previewHeight > 0 &&
                (long) previewWidth * previewHeight == previewPixels.length &&
                (long) previewWidth * previewHeight <= 1048576L) {
                try {
                    int[] opaque = new int[previewPixels.length];
                    for (int i = 0; i < previewPixels.length; i++)
                        opaque[i] = 0xFF000000 | (previewPixels[i] & 0x00FFFFFF);
                    Bitmap next = Bitmap.createBitmap(opaque, previewWidth, previewHeight,
                        Bitmap.Config.ARGB_8888);
                    if (dashboardPreview != null && !dashboardPreview.isRecycled())
                        dashboardPreview.recycle();
                    dashboardPreview = next;
                } catch (Exception ex) {
                    android.util.Log.w("PipeClean", "Couldn't decode launcher preview frame.", ex);
                }
            }
            postInvalidate();
        }

        void setMusicPlaylist(String currentTitle, String[] playlistNames,
                              int selectedTrack, boolean playing,
                              int notificationSerial, boolean notificationAllowed) {
            String safeTitle = currentTitle == null || currentTitle.trim().isEmpty()
                ? "PipeClean Chiptune" : currentTitle.trim();
            musicTitle = safeTitle;
            musicPlaylist = playlistNames != null ? playlistNames.clone() : new String[0];
            musicSelectedTrack = musicPlaylist.length > 0
                ? Math.max(0, Math.min(musicPlaylist.length - 1, selectedTrack)) : 0;
            musicPlaying = playing;
            musicNoticeAllowed = notificationAllowed;
            if (!notificationAllowed) musicNoticeVisible = false;
            if (notificationSerial != musicNoticeSerial) {
                musicNoticeSerial = notificationSerial;
                if (notificationAllowed && dashboardDualScreen && playing) {
                    musicNoticeVisible = true;
                    musicNoticeStartedAt = SystemClock.uptimeMillis();
                }
            }
            postInvalidate();
        }

        void setSaveSlots(int game, int selectedSlot, String[] statePaths, String[] thumbnailPaths) {
            // Save-slot metadata can arrive before a game launches. Keep it updated
            // without changing the deck from its idle/active game state.
            this.slot = Math.max(0, Math.min(9, selectedSlot));
            for (int i = 0; i < 10; i++) {
                String statePath = statePaths != null && i < statePaths.length && statePaths[i] != null
                    ? statePaths[i] : "";
                String requestedThumbPath = thumbnailPaths != null && i < thumbnailPaths.length && thumbnailPaths[i] != null
                    ? thumbnailPaths[i] : "";
                File stateFile = statePath.isEmpty() ? null : new File(statePath);
                File thumbFile = requestedThumbPath.isEmpty() ? null : new File(requestedThumbPath);
                if (thumbFile != null && !thumbFile.isFile() && requestedThumbPath.endsWith(".thumb.png")) {
                    File legacyThumb = new File(requestedThumbPath.substring(0,
                        requestedThumbPath.length() - ".thumb.png".length()) + ".thumb.bmp");
                    if (legacyThumb.isFile()) thumbFile = legacyThumb;
                }
                boolean occupied = stateFile != null && stateFile.isFile();
                long stateModified = occupied ? stateFile.lastModified() : 0L;
                long thumbModified = occupied && thumbFile != null && thumbFile.isFile()
                    ? thumbFile.lastModified() : 0L;
                boolean pathsChanged = !statePath.equals(slotStatePaths[i]) ||
                                       !requestedThumbPath.equals(slotThumbnailPaths[i]);
                boolean contentChanged = stateModified != slotStateModified[i] ||
                                         thumbModified != slotThumbModified[i] ||
                                         occupied != slotOccupied[i];
                if (pathsChanged || contentChanged) {
                    if (slotThumbnails[i] != null && !slotThumbnails[i].isRecycled())
                        slotThumbnails[i].recycle();
                    slotThumbnails[i] = null;
                    if (occupied && thumbModified > 0L) {
                        try {
                            BitmapFactory.Options opts = new BitmapFactory.Options();
                            opts.inSampleSize = 2;
                            slotThumbnails[i] = BitmapFactory.decodeFile(thumbFile.getAbsolutePath(), opts);
                        } catch (Exception ex) {
                            android.util.Log.w("PipeClean", "Couldn't decode save-state thumbnail for slot " + (i + 1), ex);
                        }
                    }
                }
                slotStatePaths[i] = statePath;
                slotThumbnailPaths[i] = requestedThumbPath;
                slotOccupied[i] = occupied;
                slotStateModified[i] = stateModified;
                slotThumbModified[i] = thumbModified;
                slotSavedAt[i] = stateModified;
                slotTimeLabels[i] = occupied && stateModified > 0L
                    ? new SimpleDateFormat("HH:mm", Locale.US).format(new Date(stateModified))
                    : "EMPTY";
            }
            postInvalidate();
        }

        private float clampFocus(float focus) {
            return Math.max(0f, Math.min(9f, focus));
        }

        private float timelineVisualFocus() {
            if (timelineTouchActive && timelineMoved) return clampFocus(timelineDragFocus);
            return animatedSlot < 0f ? slot : clampFocus(animatedSlot);
        }

        private float[] timelineWidths(float focus, boolean portrait) {
            float baseWidth = portrait ? 42f : 72f;
            float activeWidth = portrait ? 108f : 162f;
            float extra = activeWidth - baseWidth;
            float[] widths = new float[10];
            focus = clampFocus(focus);
            for (int i = 0; i < widths.length; i++) {
                float weight = Math.max(0f, 1f - Math.abs(i - focus));
                widths[i] = baseWidth + extra * weight;
            }
            return widths;
        }

        private float timelineScrollOffset(float[] widths, float focus, float viewportWidth,
                                           float gap, boolean portrait) {
            float activeWidth = portrait ? 108f : 162f;
            // Symmetric end padding lets slots 1 and 10 center too, instead of getting
            // stuck against an edge while the carousel is still scrolling.
            float sidePadding = Math.max(0f, (viewportWidth - activeWidth) * 0.5f);
            float[] centers = new float[10];
            float cursor = 0f;
            for (int i = 0; i < widths.length; i++) {
                centers[i] = sidePadding + cursor + widths[i] * 0.5f;
                cursor += widths[i] + gap;
            }
            float totalWidth = sidePadding * 2f + cursor - gap;
            focus = clampFocus(focus);
            int left = (int)Math.floor(focus);
            int right = Math.min(9, left + 1);
            float blend = focus - left;
            float focusCenter = centers[left] + (centers[right] - centers[left]) * blend;
            return Math.max(0f, Math.min(Math.max(0f, totalWidth - viewportWidth),
                                        focusCenter - viewportWidth * 0.5f));
        }

        private boolean isTimelinePoint(float x, float y, boolean portrait) {
            return portrait
                ? inside(x, y, 14, 704 + 14, 422, 92 - 14)
                : inside(x, y, 24, 365 + 14, 752, 83 - 14);
        }

        private void drawSaveTimeline(Canvas c, float x, float y, float w, float h, boolean portrait) {
            text(c, "SAVE-STATE TIMELINE", x + 2, y + 9, 8, accent());
            textRight(c, "SWIPE TO BROWSE", x + w - 2, y + 9, 6.5f, muted());

            float focus = timelineVisualFocus();
            float gap = portrait ? 3f : 5f;
            float[] widths = timelineWidths(focus, portrait);
            float scroll = timelineScrollOffset(widths, focus, w, gap, portrait);
            float sidePadding = Math.max(0f, (w - (portrait ? 108f : 162f)) * 0.5f);
            float baseHeight = portrait ? 43f : 36f;
            float activeHeight = portrait ? h - 18f : h - 20f;
            float cursor = 0f;

            c.save();
            c.clipRect(x, y + 14f, x + w, y + h);
            for (int i = 0; i < 10; i++) {
                float weight = Math.max(0f, 1f - Math.abs(i - focus));
                float tileW = widths[i];
                float tileH = baseHeight + (activeHeight - baseHeight) * weight;
                float tileX = x + sidePadding + cursor - scroll;
                float tileY = y + h - tileH - 1f;
                float tileBottom = y + h - 1f;
                boolean selected = weight > 0.55f;

                p.setStyle(Paint.Style.FILL);
                p.setColor(0x60000000);
                c.drawRoundRect(tileX + 1f, tileY + 2f, tileX + tileW + 1f, tileBottom,
                                selected ? 9f : 5f, selected ? 9f : 5f, p);
                p.setColor(slotOccupied[i] ? panel() : 0xCC172337);
                c.drawRoundRect(tileX, tileY, tileX + tileW, tileBottom,
                                selected ? 9f : 5f, selected ? 9f : 5f, p);

                float imageMargin = selected ? 4f : 3f;
                float labelArea = selected ? (portrait ? 22f : 19f) : 9f;
                float imageTop = tileY + imageMargin;
                float imageBottom = Math.max(imageTop + 3f, tileBottom - labelArea - 2f);
                if (slotThumbnails[i] != null && !slotThumbnails[i].isRecycled()) {
                    p.setColor(0xFFFFFFFF);
                    c.drawBitmap(slotThumbnails[i], null,
                        new RectF(tileX + imageMargin, imageTop,
                                  tileX + tileW - imageMargin, imageBottom), p);
                } else {
                    p.setColor(slotOccupied[i] ? 0x4438D69B : 0x55304050);
                    c.drawRoundRect(tileX + imageMargin, imageTop,
                        tileX + tileW - imageMargin, imageBottom, 4f, 4f, p);
                    if (selected) {
                        String placeholder = slotOccupied[i] ? "SAVED" : "EMPTY";
                        textFit(c, placeholder, tileX + tileW * 0.5f,
                            imageTop + (imageBottom - imageTop) * 0.58f,
                            tileW - imageMargin * 2f, portrait ? 7f : 8f,
                            slotOccupied[i] ? accent() : muted(), true);
                    } else if (!slotOccupied[i]) {
                        rect(c, tileX + tileW * 0.5f - 4f, imageTop + (imageBottom-imageTop)*0.5f,
                             8f, 2f, 0x887C8CA5);
                    }
                }

                if (selected) {
                    textFit(c, String.format(Locale.US, "SLOT %02d", i + 1),
                        tileX + tileW * 0.5f, tileBottom - (portrait ? 13f : 12f),
                        tileW - 8f, portrait ? 8f : 9f, gold(), true);
                    textFit(c, slotOccupied[i] && slotTimeLabels[i] != null ? slotTimeLabels[i] : "READY TO SAVE",
                        tileX + tileW * 0.5f, tileBottom - 3f,
                        tileW - 8f, portrait ? 6.5f : 7f, muted(), true);
                } else {
                    textFit(c, String.format(Locale.US, "%02d", i + 1),
                        tileX + tileW * 0.5f, tileBottom - 3f, tileW - 4f, 7f,
                        slotOccupied[i] ? ink() : muted(), true);
                }

                p.setColor(weight > 0.55f ? gold() : edge());
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(weight > 0.55f ? 2f : 1f);
                c.drawRoundRect(tileX + 0.5f, tileY + 0.5f,
                    tileX + tileW - 0.5f, tileBottom - 0.5f,
                    selected ? 9f : 5f, selected ? 9f : 5f, p);
                p.setStyle(Paint.Style.FILL);
                cursor += tileW + gap;
            }
            c.restore();
        }

        private int timelineHit(float x, float y, float tx, float ty,
                                float tw, float th, boolean portrait) {
            if (!inside(x, y, tx, ty + 14f, tw, th - 14f)) return 0;
            float focus = timelineVisualFocus();
            float gap = portrait ? 3f : 5f;
            float[] widths = timelineWidths(focus, portrait);
            float scroll = timelineScrollOffset(widths, focus, tw, gap, portrait);
            float sidePadding = Math.max(0f, (tw - (portrait ? 108f : 162f)) * 0.5f);
            float cursor = 0f;
            for (int i = 0; i < widths.length; i++) {
                float tileX = tx + sidePadding + cursor - scroll;
                if (x >= tileX && x < tileX + widths[i]) return SLOT_SELECT_BASE + i;
                cursor += widths[i] + gap;
            }
            return 0;
        }

        void setDeckLayout(int layoutGame, int[] values, String[] paths,
                           boolean editorPreview, int previewItem) {
            if (values != null) {
                for (int g = 0; g < LAYOUT_GAME_COUNT; g++) {
                    int base = g * LAYOUT_VALUE_COUNT;
                    if (values.length >= base + LAYOUT_VALUE_COUNT)
                        System.arraycopy(values, base, layoutValues[g], 0, LAYOUT_VALUE_COUNT);
                }
            }
            if (paths != null) {
                for (int g = 0; g < LAYOUT_GAME_COUNT; g++) {
                    for (int i = 0; i < LAYOUT_PATH_COUNT; i++) {
                        int at = g * LAYOUT_PATH_COUNT + i;
                        String next = at < paths.length && paths[at] != null ? paths[at] : "";
                        if (!next.equals(layoutPaths[g][i])) {
                            layoutPaths[g][i] = next;
                            if (i < 4 || i >= 12) loadLayoutImage(g, i, next);
                        }
                    }
                }
            }
            layoutPreviewGame = layoutGame >= 0 && layoutGame < LAYOUT_GAME_COUNT ? layoutGame : -1;
            layoutEditorPreview = editorPreview && layoutPreviewGame >= 0;
            layoutPreviewItem = layoutEditorPreview && previewItem >= 1 && previewItem <= 3
                ? previewItem : 0;
            postInvalidateOnAnimation();
        }

        private void loadLayoutImage(int gameIndex, int imageIndex, String path) {
            Bitmap old = layoutBitmaps[gameIndex][imageIndex];
            if (old != null && !old.isRecycled()) old.recycle();
            layoutBitmaps[gameIndex][imageIndex] = null;
            layoutMovies[gameIndex][imageIndex] = null;
            loadedLayoutImagePaths[gameIndex][imageIndex] = path == null ? "" : path;
            if (path == null || path.isEmpty()) return;
            File file = new File(path);
            if (!file.isFile()) return;
            if (path.toLowerCase(Locale.US).endsWith(".gif")) {
                try (InputStream in = new FileInputStream(file)) {
                    layoutMovies[gameIndex][imageIndex] = Movie.decodeStream(in);
                } catch (Exception ex) {
                    android.util.Log.w("PipeClean", "Couldn't decode Companion Deck GIF.", ex);
                }
            } else {
                layoutBitmaps[gameIndex][imageIndex] = decodeSampledBitmap(path);
            }
        }

        private int activeLayoutGame() {
            if (layoutEditorPreview && layoutPreviewGame >= 0) return layoutPreviewGame;
            if (!dashboardMode && game >= 0 && game < LAYOUT_GAME_COUNT) return game;
            return -1;
        }

        private void applyLayoutTransform(Canvas canvas, int element, float w, float h,
                                          float centerX, float centerY) {
            int g = activeLayoutGame();
            if (g < 0 || element < 0 || element >= LAYOUT_ELEMENT_COUNT) return;
            /* Keep existing indices stable and put added layer transforms after status. */
            int base;
            if (element < 4) base = element * 3;
            else if (element == 4) base = LAYOUT_STATUS_VALUE_BASE;
            else if (element == 5) base = 12;
            else if (element < 13) base = 16 + (element - 6) * 3;
            else base = LAYOUT_EXTENDED_DECORATION_VALUE_BASE + (element - 13) * 3;
            float dx = layoutValues[g][base] * (w / 800f);
            float dy = layoutValues[g][base + 1] * (h / 450f);
            float scaleX = Math.max(0.25f, Math.min(2.5f, layoutValues[g][base + 2] / 100f));
            int scaleYIndex;
            if (element < 4) scaleYIndex = 37 + element;
            else if (element == 4) scaleYIndex = LAYOUT_STATUS_VALUE_BASE + 3;
            else if (element == 5) scaleYIndex = 41;
            else if (element < 13) scaleYIndex = 42 + (element - 6);
            else scaleYIndex = LAYOUT_EXTENDED_DECORATION_SCALE_Y_BASE + (element - 13);
            float scaleY = Math.max(0.25f, Math.min(2.5f, layoutValues[g][scaleYIndex] / 100f));
            canvas.translate(centerX + dx, centerY + dy);
            canvas.scale(scaleX, scaleY);
            canvas.translate(-centerX, -centerY);
        }

        private boolean hasLayoutImage(int g, int imageIndex) {
            boolean supportedImage = imageIndex >= 0 && imageIndex < 4 ||
                                     imageIndex >= 12 && imageIndex < LAYOUT_PATH_COUNT;
            return g >= 0 && g < LAYOUT_GAME_COUNT && supportedImage &&
                ((layoutBitmaps[g][imageIndex] != null && !layoutBitmaps[g][imageIndex].isRecycled()) ||
                 layoutMovies[g][imageIndex] != null);
        }

        private void drawLayoutImage(Canvas canvas, int g, int imageIndex,
                                     float x, float y, float w, float h) {
            if (!hasLayoutImage(g, imageIndex) || w <= 0 || h <= 0) return;
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFFFFFFFF);
            Bitmap bitmap = layoutBitmaps[g][imageIndex];
            if (bitmap != null && !bitmap.isRecycled()) {
                canvas.drawBitmap(bitmap, null, new RectF(x, y, x + w, y + h), p);
                return;
            }
            Movie movie = layoutMovies[g][imageIndex];
            if (movie != null && movie.width() > 0 && movie.height() > 0) {
                int duration = movie.duration() > 0 ? movie.duration() : 1000;
                movie.setTime((int)(SystemClock.uptimeMillis() % duration));
                canvas.save();
                canvas.translate(x, y);
                canvas.scale(w / movie.width(), h / movie.height());
                movie.draw(canvas, 0, 0, p);
                canvas.restore();
            }
        }

        private void drawLayoutBackground(Canvas canvas, float w, float h) {
            int g = activeLayoutGame();
            if (g >= 0 && hasLayoutImage(g, 0))
                drawLayoutImageAsset(canvas, g, 0, 0, 0, w, h);
        }

        private void drawLayoutImageCover(Canvas canvas, int g, int imageIndex,
                                          float x, float y, float w, float h) {
            if (!hasLayoutImage(g, imageIndex)) return;
            Bitmap bitmap = layoutBitmaps[g][imageIndex];
            float iw, ih;
            if (bitmap != null && !bitmap.isRecycled()) {
                iw = bitmap.getWidth(); ih = bitmap.getHeight();
            } else {
                Movie movie = layoutMovies[g][imageIndex];
                iw = movie == null ? 1 : movie.width();
                ih = movie == null ? 1 : movie.height();
            }
            float scale = Math.max(w / iw, h / ih);
            float dw = iw * scale, dh = ih * scale;
            canvas.save();
            canvas.clipRect(x, y, x + w, y + h);
            drawLayoutImage(canvas, g, imageIndex,
                x + (w - dw) * 0.5f, y + (h - dh) * 0.5f, dw, dh);
            canvas.restore();
        }

        private void drawLayoutImageContain(Canvas canvas, int g, int imageIndex,
                                            float x, float y, float w, float h) {
            if (!hasLayoutImage(g, imageIndex) || w <= 0 || h <= 0) return;
            float iw, ih;
            Bitmap bitmap = layoutBitmaps[g][imageIndex];
            if (bitmap != null && !bitmap.isRecycled()) {
                iw = bitmap.getWidth(); ih = bitmap.getHeight();
            } else {
                Movie movie = layoutMovies[g][imageIndex];
                iw = movie == null ? 1 : movie.width();
                ih = movie == null ? 1 : movie.height();
            }
            float scale = Math.min(w / iw, h / ih);
            float dw = iw * scale, dh = ih * scale;
            drawLayoutImage(canvas, g, imageIndex, x + (w - dw) * 0.5f,
                y + (h - dh) * 0.5f, dw, dh);
        }

        /**
         * Draw assigned artwork using its per-asset mode. Fit is the safe default:
         * the whole source image remains visible, with no crop. Stretch deliberately
         * fills the target box and may change the image's aspect ratio.
         */
        private void drawLayoutImageAsset(Canvas canvas, int g, int imageIndex,
                                          float x, float y, float w, float h) {
            if (g < 0 || g >= LAYOUT_GAME_COUNT || imageIndex < 0 ||
                imageIndex >= LAYOUT_PATH_COUNT) return;
            int modeIndex = 49 + imageIndex;
            if (modeIndex < layoutValues[g].length && layoutValues[g][modeIndex] != 0)
                drawLayoutImage(canvas, g, imageIndex, x, y, w, h);
            else
                drawLayoutImageContain(canvas, g, imageIndex, x, y, w, h);
        }

        private boolean isThorAspect() {
            float aspect = getWidth() / (float) Math.max(1, getHeight());
            return aspect >= 1.0f && aspect <= 1.30f;
        }

        private boolean isThorCanvas(float w, float h) {
            float aspect = w / Math.max(1f, h);
            return aspect >= 1.0f && aspect <= 1.30f;
        }

        private void drawLayoutDecoration(Canvas canvas, float w, float h) {
            int g = activeLayoutGame();
            if (g < 0) return;
            boolean thor = isThorCanvas(w, h);
            boolean portrait = h > w;
            final float[] bxLandscape = {280,20,610,315,26,634,190,488,26,675,195,565,40,695,315,495};
            final float[] byLandscape = {153,78,78,245,218,218,85,260,135,135,290,290,35,35,175,175};
            final float[] bwLandscape = {240,170,170,170,140,140,122,122,100,100,100,100,95,95,112,112};
            final float[] bhLandscape = {144,120,120,120,92,92,86,86,70,70,70,70,65,65,74,74};
            final float[] bxPortrait = {230,20,510,365,25,535,190,388,22,578,210,455,30,590,325,430};
            final float[] byPortrait = {245,118,118,340,330,330,160,375,230,230,435,435,60,60,280,280};
            final float[] bwPortrait = {240,170,170,170,140,140,122,122,100,100,100,100,95,95,112,112};
            final float[] bhPortrait = {100,120,120,120,92,92,86,86,70,70,70,70,65,65,74,74};
            final float[] bxThor = {280,20,610,315,26,634,190,488,26,675,195,565,40,695,315,495};
            final float[] byThor = {150,78,78,295,218,218,95,285,135,135,410,410,35,35,190,190};
            final float[] bwThor = {240,170,170,170,140,140,122,122,100,100,100,100,95,95,112,112};
            final float[] bhThor = {135,115,115,115,90,90,84,84,68,68,68,68,64,64,72,72};
            float[] bxBase = thor ? bxThor : portrait ? bxPortrait : bxLandscape;
            float[] byBase = thor ? byThor : portrait ? byPortrait : byLandscape;
            float[] bwBase = thor ? bwThor : portrait ? bwPortrait : bwLandscape;
            float[] bhBase = thor ? bhThor : portrait ? bhPortrait : bhLandscape;
            float canvasW = thor ? 800f : portrait ? 700f : 800f;
            float canvasH = thor ? 800f * 27f / 31f : portrait ? 800f : 450f;
            for (int slot = 0; slot < LAYOUT_DECORATION_COUNT; slot++) {
                int imageIndex = slot == 0 ? 3 : slot <= 7 ? 20 + slot : 22 + slot;
                if (!hasLayoutImage(g, imageIndex)) continue;
                float bx = w * bxBase[slot] / canvasW;
                float by = h * byBase[slot] / canvasH;
                float bw = w * bwBase[slot] / canvasW;
                float bh = h * bhBase[slot] / canvasH;
                int element = 5 + slot;
                canvas.save();
                applyLayoutTransform(canvas, element, w, h, bx + bw * 0.5f, by + bh * 0.5f);
                drawLayoutImageAsset(canvas, g, imageIndex, bx, by, bw, bh);
                canvas.restore();
            }
        }

                private void playLayoutSound(int soundIndex) {
            int g = activeLayoutGame();
            if (g < 0 || layoutValues[g][15] == 0 ||
                soundIndex < 0 || soundIndex >= LAYOUT_PATH_COUNT) return;
            String path = layoutPaths[g][soundIndex];
            if (path == null || path.isEmpty() || !new File(path).isFile()) return;
            try {
                if (deckSoundPlayer != null) {
                    try { deckSoundPlayer.stop(); } catch (Exception ignored) {}
                    deckSoundPlayer.release();
                    deckSoundPlayer = null;
                }
                MediaPlayer player = new MediaPlayer();
                deckSoundPlayer = player;
                player.setDataSource(path);
                player.setOnCompletionListener(done -> {
                    done.release();
                    if (deckSoundPlayer == done) deckSoundPlayer = null;
                });
                player.setOnErrorListener((failed, what, extra) -> {
                    failed.release();
                    if (deckSoundPlayer == failed) deckSoundPlayer = null;
                    return true;
                });
                player.setOnPreparedListener(MediaPlayer::start);
                player.prepareAsync();
            } catch (Exception ex) {
                if (deckSoundPlayer != null) {
                    deckSoundPlayer.release();
                    deckSoundPlayer = null;
                }
                android.util.Log.w("PipeClean", "Couldn't play Companion Deck action sound.", ex);
            }
        }

        private void drawLayoutButtonTexture(Canvas canvas, int action,
                                             float x, float y, float w, float h) {
            int g = activeLayoutGame();
            if (g < 0) return;
            int actionIndex = action == SAVE ? 17 : action == LOAD ? 18 :
                action == PAUSE ? 19 : action == REWIND ? 20 :
                action == BACKGROUND ? 28 : action == CLOSE_GAME ? 29 : 1;
            if (!hasLayoutImage(g, actionIndex) && !hasLayoutImage(g, 1)) return;
            if (!hasLayoutImage(g, actionIndex)) actionIndex = 1;
            Path clip = new Path();
            clip.addRoundRect(new RectF(x, y, x + w, y + h), 13, 13, Path.Direction.CW);
            canvas.save();
            canvas.clipPath(clip);
            drawLayoutImageAsset(canvas, g, actionIndex, x, y, w, h);
            p.setStyle(Paint.Style.FILL);
            p.setColor(action == downAction ? 0x26000000 : 0x16000000);
            canvas.drawRoundRect(x, y, x + w, y + h, 13, 13, p);
            canvas.restore();
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

        private boolean drMario() { return game == 0; }
        private boolean sml1() { return game == 1; }
        private boolean sml2() { return game == 2; }
        private boolean wario() { return game == 3; }
        private boolean wario2() { return game == 4 || game == 6; }
        private int bg() { return drMario() ? 0xFFF0F7F7 : sml1() ? 0xFFB8D991 : sml2() ? 0xFF25152F : (wario() || wario2()) ? 0xFF211A13 : 0xFF172337; }
        private int panel() { return drMario() ? 0xF8FFFFFF : sml1() ? 0xF0F0F5D5 : sml2() ? 0xEE352342 : (wario() || wario2()) ? 0xEE36302A : 0xEE24354C; }
        private int accent() { return drMario() ? 0xFF159DAD : sml1() ? 0xFF397C30 : sml2() ? 0xFFFFCA5B : (wario() || wario2()) ? 0xFFFFC857 : 0xFF6EE5EF; }
        private int ink() { return drMario() ? 0xFF173849 : sml1() ? 0xFF253C20 : sml2() ? 0xFFFFF0D2 : (wario() || wario2()) ? 0xFFFFF1D4 : 0xFFEAF5FF; }
        private int muted() { return drMario() ? 0xFF627F8B : sml1() ? 0xFF57764D : sml2() ? 0xFFD1B7D5 : (wario() || wario2()) ? 0xFFB8AA95 : 0xFFAFC3DD; }
        private int edge() { return drMario() ? 0xFF8BCACC : sml1() ? 0xFF80A951 : sml2() ? 0xFFAD7B41 : (wario() || wario2()) ? 0xFFBD8537 : 0xFF577D9D; }
        private int gold() { return 0xFFFFD66B; }
        private int goldDark() { return 0xFFAC6B22; }

        private String title() {
            if (drMario()) return "DR. MARIO";
            if (sml1()) return "SUPER MARIO LAND";
            if (sml2()) return "6 GOLDEN COINS";
            if (wario()) return "WARIO LAND: SUPER MARIO LAND 3";
            if (wario2()) return "WARIO LAND II // STOLEN TREASURE";
            return "PIPECLEAN // READY";
        }

        private String stageText() {
            if (stage < 0) return "—";
            if (sml1() && world > 0 && stage > 0)
                return String.format(Locale.US, "%d - %d", world, stage);
            if (sml2()) return String.format(Locale.US, "STAGE %02d", stage);
            if (wario()) return String.format(Locale.US, "COURSE %02X", stage & 0xFF);
            if (wario2()) return String.format(Locale.US, "STAGE %02X", stage & 0xFF);
            return "—";
        }

        private String itemName() { return itemName(item); }

        private String itemName(int itemId) {
            if (itemId == 0) return "";
            if (sml1()) {
                if (itemId == 1) return "MUSHROOM";
                if (itemId == 2) return "SUPERBALL FLOWER";
            }
            if (sml2()) {
                if (itemId == 1) return "MUSHROOM";
                if (itemId == 2) return "FIRE FLOWER";
                if (itemId == 3) return "CARROT";
            }
            return "POWER-UP";
        }

        private String actionHint() { return actionHint(item); }

        private String actionHint(int itemId) {
            if (itemId == 0) return "";
            return sml1() ? "Tap to release into the level" : "Tap when you're ready";
        }

        private String paletteText() { return PALETTES[palette]; }
        private String filterText() { return FILTERS[filter]; }

        private void drawThorHeader(Canvas c, float h) {
            c.save();
            applyLayoutTransform(c, 0, 800f, h, 400f, 34f);
            headerPlate(c, 18, 7, 390, 49);
            text(c, "PIPE CLEAN  •  " +
                ((wario() || wario2()) ? "TREASURE EXPEDITION" : drMario() ? "MEDICAL DECK" : "POCKET QUEST"),
                27, 22, 8.8f, accent());
            textFit(c, title(), 26, 42, 372, 14, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 502, 16, 31, 31, "×", RESET_BACKGROUND);
            button(c, 542, 12, 112, 39, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 662, 12, 120, 39, game >= 0 ? "CLOSE" : "DECK",
                game >= 0 ? "GAME" : "LINKED", game >= 0 ? CLOSE_GAME : 0, false);
            c.restore();
        }

        private void drawThorControls(Canvas c, float h) {
            boolean idle = game < 0;
            c.save();
            applyLayoutTransform(c, 2, 800f, h, 400f, 449f);
            float bx = 20f, by = 420f, bw = 184f, bh = 48f, gap = 8f;
            button(c, bx, by, bw, bh, "SAVE", idle ? "START A GAME FIRST" : "SNAPSHOT", SAVE, false);
            button(c, bx + bw + gap, by, bw, bh, "LOAD", idle ? "START A GAME FIRST" : "RESTORE", LOAD, false);
            button(c, bx + (bw + gap) * 2, by, bw, bh,
                idle ? "PAUSE" : paused ? "RESUME" : "PAUSE",
                idle ? "AVAILABLE IN GAME" : paused ? "CONTINUE" : "HOLD GAME", PAUSE, false);
            button(c, bx + (bw + gap) * 3, by, bw, bh, "REWIND",
                idle ? "AVAILABLE IN GAME" : "HOLD TO REWIND", REWIND, true);
            c.restore();

            c.save();
            applyLayoutTransform(c, 3, 800f, h, 400f, 596f);
            drawSaveTimeline(c, 20f, 510f, 760f, h - 520f, false);
            c.restore();
        }

        private void drawThorDefault(Canvas c, float h) {
            drawThorHeader(c, h);
            if (game < 0) {
                badge(c, 20, 70, 210, 50, "DECK STATUS", "LINKED");
                badge(c, 244, 70, 320, 50, "NEXT STEP", "LAUNCH A GAME");
                badge(c, 578, 70, 202, 50, "QUICK ACTIONS", "READY");
            } else {
                badge(c, 20, 70, 210, 50, "LIVES", String.format(Locale.US, "× %02d", lives));
                badge(c, 244, 70, 320, 50, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE", stageText());
                badge(c, 578, 70, 202, 50, "SESSION", paused ? "PAUSED" : "IN PLAY");
            }

            card(c, 20, 136, 174, 242, "RESERVE ITEM");
            if (item > 0) {
                textFit(c, itemName(item), 32, 181, 150, 11, accent());
                text(c, "TAP TO USE", 32, 204, 8, muted());
                textFit(c, actionHint(item), 32, 230, 150, 8, ink());
            } else {
                text(c, "NO ITEM STORED", 32, 184, 9, muted());
                textFit(c, "Collect a power-up", 32, 207, 150, 8, ink());
            }
            c.save();
            applyLayoutTransform(c, 1, 800f, h, 400f, 254f);
            drawHeroItem(c, 400, 255, 96, item);
            c.restore();

            c.save();
            applyLayoutTransform(c, 4, 800f, h, 690f, 255f);
            card(c, 606, 136, 174, 242, game >= 0 ? "ADVENTURE STATUS" : "QUICK TIP");
            if (game >= 0) {
                text(c, "LIVES", 620, 183, 8, muted());
                text(c, String.format(Locale.US, "× %02d", lives), 620, 204, 14, ink());
                text(c, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE", 620, 237, 7.3f, muted());
                textFit(c, stageText(), 620, 258, 145, 11, accent());
                textFit(c, paused ? "GAME PAUSED" : "PLAYING NOW", 620, 294, 145, 8, muted());
            } else {
                text(c, "SECOND SCREEN", 620, 183, 8, muted());
                text(c, "CONNECTED", 620, 204, 12, ink());
                textFit(c, "Launch a game to activate live information and controls.", 620, 235, 145, 8.5f, accent());
            }
            c.restore();
            drawThorControls(c, h);
        }

        private void drawThorDrMario(Canvas c, float h) {
            drawThorHeader(c, h);
            c.save();
            applyLayoutTransform(c, 1, 800f, h, 199f, 228f);
            card(c, 20, 78, 358, 304, "NEXT CAPSULE");
            drawMedicalCross(c, 344, 101, 14f, 0xFF159DAD);
            drawDrMarioCapsule(c, 199, 226, 166, 70, drMarioCapsule);
            text(c, drMarioCapsule >= 0 && drMarioCapsule <= 0x17 ?
                "READY TO DROP" : "WAITING FOR PILL", 199, 318, 8.5f, muted(), true);
            c.restore();

            c.save();
            applyLayoutTransform(c, 4, 800f, h, 590f, 230f);
            card(c, 397, 78, 383, 91, "SCORE");
            textFit(c, drMarioScoreText(), 416, 139, 344, 24f, accent());
            drawDrMarioMetric(c, 397, 183, 184, 133, "VIRUS LEVEL", drMarioLevelText(), 19f);
            drawDrMarioMetric(c, 594, 183, 186, 133, "SPEED", drMarioSpeedText(), 19f);
            card(c, 397, 330, 383, 52, "SESSION");
            textFit(c, paused ? "PAUSED — RESUME WHEN READY" : "CLINIC STATUS  •  LIVE", 412, 361, 350, 10, ink());
            c.restore();
            drawThorControls(c, h);
        }

        private void drawThorWario(Canvas c, float h) {
            drawThorHeader(c, h);
            drawWarioVault(c, 20, 77, 350, 160);
            drawWarioEmblem(c, 400, 157, 58);
            c.save();
            applyLayoutTransform(c, 4, 800f, h, 600f, 157f);
            drawWarioStatus(c, 430, 77, 350, 160);
            c.restore();
            drawExpeditionLog(c, 20, 255, 370, 143, 5);
            card(c, 410, 255, 370, 143, "CURRENT COURSE");
            textFit(c, stageText(), 428, 300, 330, 20, accent());
            textFit(c, "COIN BANK  " + warioCoinsText(), 428, 330, 330, 11, ink());
            textFit(c, "TREASURE EXPEDITION  •  " + (paused ? "PAUSED" : "IN PLAY"),
                428, 362, 330, 9, muted());
            drawThorControls(c, h);
        }

        private void drawThorWario2(Canvas c, float h) {
            drawThorHeader(c, h);
            drawWario2Grid(c, 20, 77, 365, 175, "TREASURE VAULT", wario2TreasureMask, false);
            drawWario2Grid(c, 415, 77, 365, 175, "PICTURE PANEL", wario2PictureMask, true);
            drawExpeditionLog(c, 20, 268, 365, 128, 5);
            card(c, 415, 268, 365, 128, "ADVENTURE TRACKER");
            textFit(c, stageText(), 431, 311, 330, 16, accent());
            textFit(c, "COIN BANK  " + (warioCoins < 0 ? "------" :
                String.format(Locale.US, "%06d", warioCoins)), 431, 342, 330, 10, ink());
            textFit(c, "POCKET COINS  " + (warioHearts < 0 ? "----" :
                String.format(Locale.US, "%04d", warioHearts)), 431, 366, 330, 9, muted());
            drawThorControls(c, h);
        }

        @Override protected void onDraw(Canvas c) {
            super.onDraw(c);
            boolean thor = isThorAspect();
            boolean portrait = !thor && getHeight() > getWidth();
            float w = portrait ? 700f : 800f;
            float h = thor ? 800f * 27f / 31f : portrait ? 800f : 450f;
            float scale = Math.min(getWidth() / w, getHeight() / h);
            boolean preview = layoutEditorPreview && layoutPreviewGame >= 0;
            int previousGame = game;
            int previousItem = item;
            boolean previousDashboardMode = dashboardMode;
            if (preview) {
                game = layoutPreviewGame;
                item = layoutPreviewItem;
                dashboardMode = false;
            }
            c.save();
            c.translate((getWidth() - w * scale) * 0.5f, (getHeight() - h * scale) * 0.5f);
            c.scale(scale, scale);
            drawWallpaper(c, w, h);
            if (!dashboardMode) drawLayoutBackground(c, w, h);
            if (thor) {
                if (dashboardMode) drawLauncherDashboardLandscape(c);
                else if (wario()) drawThorWario(c, h);
                else if (wario2()) drawThorWario2(c, h);
                else if (drMario()) drawThorDrMario(c, h);
                else drawThorDefault(c, h);
            } else if (dashboardMode) {
                if (portrait) drawLauncherDashboardPortrait(c); else drawLauncherDashboardLandscape(c);
            } else if (wario()) {
                if (portrait) drawWarioPortrait(c); else drawWarioLandscape(c);
            } else if (wario2()) {
                if (portrait) drawWario2Portrait(c); else drawWario2Landscape(c);
            } else {
                if (portrait) drawPortrait(c); else drawLandscape(c);
            }
            if (!dashboardMode && wario()) drawTreasureCelebration(c, w, h);
            if (!dashboardMode && wario2()) drawWario2Celebration(c, w, h);
            drawLayoutDecoration(c, w, h);
            drawMusicNotification(c, portrait, w, h);
            drawCornerGems(c, w, h);
            c.restore();
            game = previousGame;
            item = previousItem;
            dashboardMode = previousDashboardMode;
            if (animatedSlot < 0f) animatedSlot = slot;
            if (!timelineTouchActive) {
                float delta = slot - animatedSlot;
                if (Math.abs(delta) > 0.02f) {
                    animatedSlot = clampFocus(animatedSlot + delta * 0.24f);
                    postInvalidateOnAnimation();
                } else {
                    animatedSlot = slot;
                }
            }
            boolean animatedLayout = false;
            int activeGame = preview ? layoutPreviewGame : activeLayoutGame();
            if (activeGame >= 0) {
                for (int i = 0; i < LAYOUT_PATH_COUNT; i++)
                    if (layoutMovies[activeGame][i] != null) animatedLayout = true;
            }
            if (wallpaperMovie != null || animatedLayout) postInvalidateOnAnimation();
            else postInvalidateDelayed(40);
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
            } else if (drMario()) {
                for (int i = 0; i < 24; i++) {
                    float sx = (i * 71 + 23) % w;
                    float sy = (i * 43 + 17) % h;
                    drawMedicalCross(c, sx, sy, 8f + (i % 3) * 2f,
                        i % 2 == 0 ? 0x1C159DAD : 0x18E65B69);
                }
                for (int y = 0; y < h; y += 24) rect(c, 0, y, w, 1, 0x0D159DAD);
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

        private String dashboardGameName(int index) {
            return index >= 0 && index < dashboardGameNames.length &&
                   dashboardGameNames[index] != null && !dashboardGameNames[index].isEmpty()
                ? dashboardGameNames[index] : "Choose a game";
        }

        private String playTimeText(int seconds) {
            if (seconds < 0) seconds = 0;
            int hours = seconds / 3600;
            int minutes = (seconds % 3600) / 60;
            if (hours > 0) return String.format(Locale.US, "%dh %02dm played", hours, minutes);
            return String.format(Locale.US, "%dm played", minutes);
        }

        private int dashboardRecentCount() {
            int count = 0;
            for (int id : dashboardRecentGames)
                if (id >= 0 && id < dashboardGameNames.length) count++;
            return count;
        }

        private void drawDashboardPreview(Canvas c, RectF target) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFF07111F);
            c.drawRoundRect(target, 8, 8, p);
            if (dashboardPreview == null || dashboardPreview.isRecycled()) {
                drawPixelStar(c, target.centerX(), target.centerY() - 5, 18, accent());
                text(c, "GAME PREVIEW", target.centerX(), target.centerY() + 22, 9, muted(), true);
                return;
            }
            float scale = Math.min(target.width() / dashboardPreview.getWidth(),
                                   target.height() / dashboardPreview.getHeight());
            float dw = dashboardPreview.getWidth() * scale;
            float dh = dashboardPreview.getHeight() * scale;
            float left = target.centerX() - dw * 0.5f;
            float top = target.centerY() - dh * 0.5f;
            p.setColor(0xFFFFFFFF);
            p.setFilterBitmap(false);
            c.drawBitmap(dashboardPreview, null, new RectF(left, top, left + dw, top + dh), p);
            p.setFilterBitmap(true);
        }

        private void drawLauncherDashboardLandscape(Canvas c) {
            headerPlate(c, 18, 7, 282, 48);
            text(c, "PIPE CLEAN  •  GAME LIBRARY", 27, 22, 9, accent());
            text(c, "READY TO PLAY", 27, 42, 15, ink());

            float previewH = 362f;
            card(c, 20, 65, 460, previewH, "SELECTED GAME");
            float imageBottom = 293f;
            drawDashboardPreview(c, new RectF(34, 94, 466, imageBottom));
            float titleY = imageBottom + 21f;
            textFit(c, dashboardGameName(dashboardGame), 36, titleY, 420, 16, ink());
            int played = dashboardGame >= 0 && dashboardGame < dashboardPlaySeconds.length
                ? dashboardPlaySeconds[dashboardGame] : 0;
            text(c, "TOTAL PLAY TIME", 36, titleY + 25, 7.5f, muted());
            textFit(c, playTimeText(played), 36, titleY + 39, 420, 12, accent());

            card(c, 492, 65, 288, previewH, "RECENTLY PLAYED");
            int rows = dashboardRecentCount();
            if (rows == 0) {
                drawMiniStar(c, 523, 132, 9);
                text(c, "YOUR ADVENTURE", 545, 126, 8, muted());
                textFit(c, "Games you launch will appear here.", 510, 153, 246, 11, ink());
                textFit(c, "Play time is saved automatically.", 510, 173, 246, 9, muted());
            } else {
                for (int i = 0; i < rows; i++) {
                    int id = dashboardRecentGames[i];
                    if (id < 0) continue;
                    float ry = 99 + i * (dashboardDualScreen ? 27f : 63f);
                    p.setColor(i == 0 ? 0x4038D69B : 0x30172237);
                    c.drawRoundRect(504, ry, 768, ry + (dashboardDualScreen ? 24f : 56f), 7, 7, p);
                    drawMiniStar(c, 518, ry + 12, 5);
                    textFit(c, dashboardGameName(id), 531, ry + 3, 226, 9, i == 0 ? accent() : ink());
                    int secs = id < dashboardPlaySeconds.length ? dashboardPlaySeconds[id] : 0;
                    textFit(c, playTimeText(secs), 531, ry + 14, 226, 7.5f, muted());
                }
            }

        }

        private void drawLauncherDashboardPortrait(Canvas c) {
            headerPlate(c, 14, 7, 410, 45);
            text(c, "PIPE CLEAN  •  LIBRARY", 20, 23, 9, accent());
            text(c, "READY TO PLAY", 20, 42, 13, ink());

            float previewH = 380f;
            card(c, 16, 60, 668, previewH, "SELECTED GAME");
            float imageBottom = 60 + previewH - 88f;
            drawDashboardPreview(c, new RectF(30, 91, 670, imageBottom));
            float titleY = imageBottom + 17f;
            textFit(c, dashboardGameName(dashboardGame), 31, titleY, 638, 13, ink());
            int played = dashboardGame >= 0 && dashboardGame < dashboardPlaySeconds.length
                ? dashboardPlaySeconds[dashboardGame] : 0;
            text(c, "TOTAL PLAY TIME", 31, titleY + 20, 7.5f, muted());
            textFit(c, playTimeText(played), 31, titleY + 32, 638, 11, accent());

            float recentY = 449f;
            float recentH = 800f - recentY - 14f;
            card(c, 16, recentY, 668, recentH, "RECENTLY PLAYED");
            int rows = dashboardRecentCount();
            if (rows == 0) {
                textFit(c, "Your recent games and play time will appear here.", 30,
                    recentY + 50, 624, 11, muted());
            } else {
                float rowH = dashboardDualScreen ? 26f : 44f;
                for (int i = 0; i < rows; i++) {
                    int id = dashboardRecentGames[i];
                    if (id < 0) continue;
                    float ry = recentY + 35 + i * rowH;
                    if (ry + rowH - 3 > recentY + recentH - 6) break;
                    p.setColor(i == 0 ? 0x4038D69B : 0x30172237);
                    c.drawRoundRect(27, ry, 673, ry + rowH - 3, 7, 7, p);
                    drawMiniStar(c, 40, ry + rowH * 0.5f, 5);
                    textFit(c, dashboardGameName(id), 54, ry + 3, 590, 9, i == 0 ? accent() : ink());
                    int secs = id < dashboardPlaySeconds.length ? dashboardPlaySeconds[id] : 0;
                    textFit(c, playTimeText(secs), 54, ry + 15, 590, 7.5f, muted());
                }
            }
        }

                private void drawMusicNotification(Canvas c, boolean portrait, float w, float h) {
            if (!dashboardDualScreen || !musicNoticeAllowed || !musicNoticeVisible) return;
            long elapsed = Math.max(0L, SystemClock.uptimeMillis() - musicNoticeStartedAt);
            final long totalMs = 2750L;
            if (elapsed >= totalMs) {
                musicNoticeVisible = false;
                return;
            }

            float width = portrait ? 640f : 368f;
            float height = 58f;
            float targetX = portrait ? 18f : 18f;
            float y = h - height - 11f;
            final float inMs = 300f, outAtMs = 2340f, outMs = 360f;
            float offset = 0f;
            if (elapsed < inMs) {
                float q = elapsed / inMs;
                float ease = 1f - (1f - q) * (1f - q) * (1f - q);
                offset = -width * (1f - ease);
            } else if (elapsed > outAtMs) {
                float q = Math.min(1f, (elapsed - outAtMs) / outMs);
                float ease = q * q * (3f - 2f * q);
                offset = -width * ease;
            }
            float x = targetX + offset;
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xE8111722);
            c.drawRoundRect(x, y, x + width, y + height, 12f, 12f, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(1f);
            p.setColor(0xD85C6F93);
            c.drawRoundRect(x, y, x + width, y + height, 12f, 12f, p);
            p.setStyle(Paint.Style.FILL);

            float cx = x + 31f;
            float cy = y + height * 0.5f;
            drawVinylRecord(c, cx, cy, 21f, musicPlaying);
            text(c, "NOW PLAYING", x + 61f, y + 17f, 7.5f, 0xFF71D7A3);
            textFit(c, musicTitle, x + 61f, y + 34f, width - 73f, 11.5f, ink());
            postInvalidateOnAnimation();
        }

        private void drawVinylRecord(Canvas c, float cx, float cy, float radius, boolean playing) {
            long now = SystemClock.uptimeMillis();
            float angle = playing ? (now % 6000L) * (float)(Math.PI * 2.0 / 6000.0) : -0.45f;
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFF07090F);
            c.drawCircle(cx, cy, radius, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(1.3f);
            p.setColor(0xFF3B4357);
            c.drawCircle(cx, cy, radius - 3, p);
            c.drawCircle(cx, cy, radius * 0.74f, p);
            c.drawCircle(cx, cy, radius * 0.61f, p);
            c.drawCircle(cx, cy, radius * 0.46f, p);
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFF242D43);
            c.drawCircle(cx, cy, radius * 0.30f, p);
            p.setColor(0xFF80D9E3);
            c.drawLine(cx, cy, cx + (float)Math.cos(angle) * radius * 0.83f,
                       cy + (float)Math.sin(angle) * radius * 0.83f, p);
            p.setColor(playing ? accent() : 0xFF858DA2);
            c.drawCircle(cx, cy, radius * 0.23f, p);
            p.setColor(0xFF07090F);
            c.drawCircle(cx, cy, radius * 0.045f, p);
            p.setStyle(Paint.Style.FILL);
        }


        private String drMarioScoreText() {
            return drMarioScore < 0 ? "--------" : String.format(Locale.US, "%08d", drMarioScore);
        }
        private String drMarioLevelText() {
            return drMarioLevel < 0 ? "--" : String.format(Locale.US, "%02d", drMarioLevel);
        }
        private String drMarioSpeedText() {
            if (drMarioSpeed == 2) return "LOW";
            if (drMarioSpeed == 3) return "MED";
            if (drMarioSpeed == 4) return "HI";
            return "--";
        }
        private int drMarioPillColor(int colorId) {
            if (colorId == 0) return 0xFF35B8D1;
            if (colorId == 1) return 0xFFEF6470;
            return 0xFFF2C94C;
        }
        private void drawMedicalCross(Canvas c, float cx, float cy, float size, int color) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(color);
            float arm = size * 0.30f;
            c.drawRoundRect(cx-arm, cy-size*0.5f, cx+arm, cy+size*0.5f, arm*0.35f, arm*0.35f, p);
            c.drawRoundRect(cx-size*0.5f, cy-arm, cx+size*0.5f, cy+arm, arm*0.35f, arm*0.35f, p);
        }
        private void drawDrMarioCapsule(Canvas c, float cx, float cy, float width, float height, int rawCapsule) {
            boolean valid = rawCapsule >= 0 && rawCapsule <= 0x17;
            int first=0, second=0, rotation=0;
            if (valid) {
                switch (rawCapsule & 0xFC) {
                    case 0x00: first=0; second=0; break;
                    case 0x04: first=0; second=1; break;
                    case 0x08: first=0; second=2; break;
                    case 0x0C: first=1; second=1; break;
                    case 0x10: first=1; second=2; break;
                    case 0x14: first=2; second=2; break;
                    default: valid=false; break;
                }
                rotation=(rawCapsule & 0x03)*90;
            }
            if (!valid) { first=0; second=2; rotation=0; }
            c.save();
            c.rotate(rotation,cx,cy);
            RectF body = new RectF(cx-width*0.5f, cy-height*0.5f, cx+width*0.5f, cy+height*0.5f);
            p.setStyle(Paint.Style.FILL); p.setColor(0x1C153849);
            c.drawRoundRect(body.left+3f,body.top+5f,body.right+3f,body.bottom+5f,height*0.5f,height*0.5f,p);
            Path shell = new Path();
            shell.addRoundRect(body,height*0.5f,height*0.5f,Path.Direction.CW);
            c.save(); c.clipPath(shell);
            p.setColor(valid?drMarioPillColor(first):0xFFCFDDE0);
            c.drawRect(body.left,body.top,body.centerX(),body.bottom,p);
            p.setColor(valid?drMarioPillColor(second):0xFFE2E9EB);
            c.drawRect(body.centerX(),body.top,body.right,body.bottom,p);
            c.restore();
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(Math.max(2f,height*0.035f)); p.setColor(0xFF314B5B);
            c.drawRoundRect(body,height*0.5f,height*0.5f,p);
            p.setStrokeWidth(Math.max(1.5f,height*0.025f)); p.setColor(0xCCFFFFFF);
            c.drawLine(body.left+height*0.55f,body.top+height*0.23f,body.centerX()-height*0.12f,body.top+height*0.23f,p);
            p.setStrokeWidth(1.5f); p.setColor(0xAA314B5B);
            c.drawLine(body.centerX(),body.top+height*0.08f,body.centerX(),body.bottom-height*0.08f,p);
            p.setStyle(Paint.Style.FILL); c.restore();
        }
        private void drawDrMarioMetric(Canvas c,float x,float y,float w,float h,String label,String value,float valueSize) {
            card(c,x,y,w,h,label);
            textFit(c,value,x+8f,y+h-17f,w-16f,valueSize,ink(),true);
        }
        private void drawDrMarioLandscape(Canvas c) {
            c.save(); applyLayoutTransform(c,0,800f,450f,400f,31.5f);
            headerPlate(c,18,7,265,49);
            text(c,"PIPE CLEAN  •  MEDICAL DECK",26,22,8.5f,accent());
            text(c,"DR. MARIO",25,43,16,ink());
            if (!wallpaperPath.isEmpty()) smallButton(c,552,17,34,34,"×",RESET_BACKGROUND);
            button(c,593,13,105,42,"BACKGROUND","PNG / GIF",BACKGROUND,false);
            button(c,704,13,72,42,"CLOSE","GAME",CLOSE_GAME,false);
            c.restore();
            c.save(); applyLayoutTransform(c,1,800f,450f,400f,164f);
            card(c,20,70,270,235,"NEXT CAPSULE");
            drawMedicalCross(c,258,94,13f,0xFF159DAD);
            drawDrMarioCapsule(c,155,184,160,66,drMarioCapsule);
            text(c,drMarioCapsule>=0&&drMarioCapsule<=0x17?"READY TO DROP":"WAITING FOR PILL",155,265,8f,muted(),true);
            c.restore();
            c.save(); applyLayoutTransform(c,4,800f,450f,541f,187.5f);
            drawDrMarioMetric(c,306,70,470,105,"SCORE",drMarioScoreText(),25f);
            drawDrMarioMetric(c,306,188,224,117,"VIRUS LEVEL",drMarioLevelText(),27f);
            drawDrMarioMetric(c,542,188,234,117,"SPEED",drMarioSpeedText(),24f);
            text(c,paused?"PAUSED":"IN SESSION",760,60,6.5f,muted(),true);
            c.restore();
            c.save(); applyLayoutTransform(c,2,800f,450f,400f,343f); drawDockLandscape(c); c.restore();
            c.save(); applyLayoutTransform(c,3,800f,450f,400f,406.5f); drawSaveTimeline(c,24,365,752,83,false); c.restore();
        }
        private void drawDrMarioPortrait(Canvas c) {
            c.save(); applyLayoutTransform(c, 0, 700f, 800f, 350f, 30f);
            headerPlate(c, 14, 7, 400, 43);
            text(c, "PIPE CLEAN  •  MEDICAL DECK", 20, 23, 8.2f, accent());
            text(c, "DR. MARIO", 20, 41, 13, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 416, 16, 20, 27, "×", RESET_BACKGROUND);
            button(c, 442, 12, 105, 37, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 555, 12, 121, 37, "CLOSE", "GAME", CLOSE_GAME, false);
            c.restore();

            c.save(); applyLayoutTransform(c, 1, 700f, 800f, 350f, 238f);
            card(c, 20, 104, 660, 235, "NEXT CAPSULE");
            drawMedicalCross(c, 637, 128, 15f, 0xFF159DAD);
            drawDrMarioCapsule(c, 350, 218, 188, 76, drMarioCapsule);
            text(c, drMarioCapsule >= 0 && drMarioCapsule <= 0x17 ? "READY TO DROP" : "WAITING FOR PILL",
                 350, 315, 9f, muted(), true);
            c.restore();

            c.save(); applyLayoutTransform(c, 4, 700f, 800f, 350f, 394f);
            drawDrMarioMetric(c, 16, 354, 210, 78, "SCORE", drMarioScoreText(), 18f);
            drawDrMarioMetric(c, 245, 354, 210, 78, "VIRUS LEVEL", drMarioLevelText(), 20f);
            drawDrMarioMetric(c, 474, 354, 210, 78, "SPEED", drMarioSpeedText(), 19f);
            c.restore();

            c.save(); applyLayoutTransform(c, 2, 700f, 800f, 350f, 507f);
            boolean idle = game < 0;
            button(c, 20, 455, 320, 48, "SAVE", idle ? "START A GAME FIRST" : "SNAPSHOT", SAVE, false);
            button(c, 360, 455, 320, 48, "LOAD", idle ? "START A GAME FIRST" : "RESTORE", LOAD, false);
            button(c, 20, 511, 320, 48, idle ? "PAUSE" : paused ? "RESUME" : "PAUSE",
                idle ? "AVAILABLE IN GAME" : paused ? "CONTINUE" : "HOLD GAME", PAUSE, false);
            button(c, 360, 511, 320, 48, "REWIND", idle ? "AVAILABLE IN GAME" : "HOLD TO REWIND", REWIND, true);
            c.restore();

            c.save(); applyLayoutTransform(c, 3, 700f, 800f, 350f, 750f);
            drawSaveTimeline(c, 14, 704, 672, 92, true);
            c.restore();
        }

                private String warioCoinsText() {
            return warioCoins >= 0 ? String.format(Locale.US, "%05d", warioCoins) : "-----";
        }

        private String warioTimeText() {
            return warioTime >= 0 ? String.format(Locale.US, "%03d", warioTime) : "---";
        }

        private void drawTreasureMark(Canvas c, float cx, float cy, float size, boolean found) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(found ? 0xFFFFD66B : 0x665B554B);
            Path gem = new Path();
            gem.moveTo(cx, cy - size);
            gem.lineTo(cx + size * 0.78f, cy - size * 0.18f);
            gem.lineTo(cx + size * 0.58f, cy + size * 0.72f);
            gem.lineTo(cx, cy + size);
            gem.lineTo(cx - size * 0.58f, cy + size * 0.72f);
            gem.lineTo(cx - size * 0.78f, cy - size * 0.18f);
            gem.close();
            c.drawPath(gem, p);
            p.setColor(found ? 0xFFFFF4C2 : 0x55352B1D);
            c.drawCircle(cx, cy, size * 0.28f, p);
            if (found) {
                p.setColor(0xFFFFF9E6);
                c.drawCircle(cx - size * 0.18f, cy - size * 0.28f, Math.max(1f, size * 0.10f), p);
            }
        }


        private void drawWario2Grid(Canvas c, float x, float y, float w, float h,
                                    String heading, long flags, boolean pictures) {
            card(c, x, y, w, h, heading);
            int count = pictures ? wario2PictureCount() : wario2TreasureCount();
            text(c, (flags < 0 ? "-- / 50" : count + " / 50") + (pictures ? " PIECES" : " TREASURES"),
                 x + 12, y + 42, 8.2f, accent());
            float bx = x + 12, by = y + 49, bw = w - 24;
            p.setColor(0x553A2B1B);
            c.drawRoundRect(bx, by, bx + bw, by + 5, 3, 3, p);
            if (count > 0) {
                p.setColor(accent());
                c.drawRoundRect(bx, by, bx + bw * count / 50f, by + 5, 3, 3, p);
            }
            float stepX = (w - 28f) / 10f, stepY = (h - 64f) / 5f;
            for (int i = 0; i < 50; i++) {
                boolean found = flags >= 0 && (flags & (1L << i)) != 0;
                float cx = x + 14f + stepX * (i % 10) + stepX * 0.5f;
                float cy = y + 57f + stepY * (i / 10) + stepY * 0.5f;
                if (pictures) {
                    p.setStyle(Paint.Style.FILL);
                    p.setColor(found ? 0xFFFFD66B : 0xFF4A3927);
                    c.drawRoundRect(cx - 3.2f, cy - 3.2f, cx + 3.2f, cy + 3.2f, 1.2f, 1.2f, p);
                    p.setStyle(Paint.Style.STROKE);
                    p.setStrokeWidth(0.8f);
                    p.setColor(found ? 0xFFFFF4C2 : 0xFF8A7251);
                    c.drawRoundRect(cx - 3.2f, cy - 3.2f, cx + 3.2f, cy + 3.2f, 1.2f, 1.2f, p);
                    p.setStyle(Paint.Style.FILL);
                    if (found) rect(c, cx - 1.0f, cy - 1.0f, 2.0f, 2.0f, 0xFFFFF8E2);
                } else {
                    drawTreasureMark(c, cx, cy, 3.2f, found);
                }
            }
        }

        private void drawWario2Landscape(Canvas c) {
            c.save();
            headerPlate(c, 18, 7, 265, 49);
            text(c, "PIPE CLEAN  •  STOLEN TREASURE", 26, 23, 8.4f, accent());
            textFit(c, title(), 25, 43, 260, 15, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 552, 17, 34, 34, "×", RESET_BACKGROUND);
            button(c, 593, 13, 105, 42, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 704, 13, 72, 42, "CLOSE", "GAME", CLOSE_GAME, false);
            c.restore();

            drawWario2Grid(c, 20, 70, 230, 133, "TREASURE VAULT", wario2TreasureMask, false);
            drawWarioEmblem(c, 400, 139, 44);
            drawWario2Grid(c, 550, 70, 230, 133, "PICTURE PANEL", wario2PictureMask, true);

            drawExpeditionLog(c, 20, 213, 230, 94, 4);
            card(c, 260, 213, 260, 94, "ADVENTURE TRACKER");
            textFit(c, stageText(), 276, 258, 226, 12, accent());
            text(c, "COIN BANK  " + (warioCoins < 0 ? "------" : String.format(Locale.US, "%06d", warioCoins)),
                 276, 278, 8, ink());
            text(c, "POCKET COINS  " + (warioHearts < 0 ? "----" : String.format(Locale.US, "%04d", warioHearts)),
                 276, 294, 8, muted());

            card(c, 530, 213, 250, 94, "TRANSFORMATION MONITOR");
            text(c, "CURRENT FORM", 544, 258, 7, muted());
            textFit(c, "FORM MEMORY NOT MAPPED", 544, 276, 222, 9, accent());
            text(c, "TRACKING READY", 544, 294, 7, muted());

            c.save(); applyLayoutTransform(c, 2, 800f, 450f, 400f, 343f); drawDockLandscape(c); c.restore();
            c.save(); applyLayoutTransform(c, 3, 800f, 450f, 400f, 406.5f); drawSaveTimeline(c, 24, 365, 752, 83, false); c.restore();
        }

        private void drawWario2Portrait(Canvas c) {
            c.save(); applyLayoutTransform(c, 0, 700f, 800f, 350f, 30f);
            headerPlate(c, 14, 7, 400, 43);
            text(c, "PIPE CLEAN  •  STOLEN TREASURE", 20, 23, 8.0f, accent());
            textFit(c, title(), 20, 41, 385, 12.5f, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 416, 16, 20, 27, "×", RESET_BACKGROUND);
            button(c, 442, 12, 105, 37, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 555, 12, 121, 37, "CLOSE", "GAME", CLOSE_GAME, false);
            c.restore();

            badge(c, 20, 55, 205, 47, "CURRENT STAGE", stageText());
            badge(c, 238, 55, 442, 47, "TREASURES", wario2TreasureCount() + " / 50");

            drawWario2Grid(c, 20, 111, 320, 150, "TREASURE VAULT", wario2TreasureMask, false);
            drawWario2Grid(c, 360, 111, 320, 150, "PICTURE PANEL", wario2PictureMask, true);

            card(c, 20, 270, 320, 61, "COINS");
            text(c, "BANK  " + (warioCoins < 0 ? "------" : String.format(Locale.US, "%06d", warioCoins)),
                 32, 298, 8.5f, ink());
            text(c, "POCKET  " + (warioHearts < 0 ? "----" : String.format(Locale.US, "%04d", warioHearts)),
                 32, 317, 8, muted());
            card(c, 360, 270, 320, 61, "TRANSFORMATION");
            text(c, "FORM NOT MAPPED", 374, 298, 8.5f, accent());
            text(c, "LIVE TRACKING READY", 374, 317, 7.5f, muted());
            drawExpeditionLog(c, 20, 340, 660, 88, 4);

            c.save(); applyLayoutTransform(c, 2, 700f, 800f, 350f, 507f);
            button(c, 20, 455, 320, 48, "SAVE", "SNAPSHOT", SAVE, false);
            button(c, 360, 455, 320, 48, "LOAD", "RESTORE", LOAD, false);
            button(c, 20, 511, 320, 48, paused ? "RESUME" : "PAUSE",
                paused ? "CONTINUE" : "HOLD GAME", PAUSE, false);
            button(c, 360, 511, 320, 48, "REWIND", "HOLD TO REWIND", REWIND, true);
            c.restore();

            c.save(); applyLayoutTransform(c, 3, 700f, 800f, 350f, 750f);
            drawSaveTimeline(c, 14, 704, 672, 92, true);
            c.restore();
        }

                private void drawWario2Celebration(Canvas c, float w, float h) {
            if (!wario2AnimationActive) return;
            long elapsed = SystemClock.uptimeMillis() - wario2AnimationStart;
            if (elapsed >= TREASURE_ANIMATION_MS) {
                wario2AnimationActive = false;
                return;
            }
            float phase = Math.max(0f, Math.min(1f, elapsed / (float)TREASURE_ANIMATION_MS));
            float cx = w * 0.5f, cy = h * (h > w ? 0.39f : 0.37f);
            int alpha = Math.max(0, Math.min(170, (int)((1f - phase) * 170f)));
            p.setStyle(Paint.Style.STROKE);
            for (int i = 0; i < 3; i++) {
                p.setStrokeWidth(2.5f - i * 0.45f);
                p.setColor((Math.max(0, alpha - i * 28) << 24) | 0x00FFD66B);
                c.drawCircle(cx, cy, 22f + phase * (95f + i * 20f), p);
            }
            p.setStyle(Paint.Style.FILL);
            float bannerW = Math.min(w - 28f, h > w ? 330f : 360f);
            float bannerY = cy + (h > w ? 58f : 72f);
            p.setColor(0xE82B2115);
            c.drawRoundRect(cx - bannerW * 0.5f, bannerY, cx + bannerW * 0.5f,
                            bannerY + 43f, 12, 12, p);
            p.setColor(0xFFFFD66B);
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(1.8f);
            c.drawRoundRect(cx - bannerW * 0.5f, bannerY, cx + bannerW * 0.5f,
                            bannerY + 43f, 12, 12, p);
            p.setStyle(Paint.Style.FILL);
            text(c, wario2AnimationKind == 2 ? "PICTURE PANEL FOUND!" : "TREASURE FOUND!",
                 cx, bannerY + 18f, 10, 0xFFFFEBA5, true);
            text(c, String.format(Locale.US, "%s %02d / 50",
                 wario2AnimationKind == 2 ? "PICTURE PIECE" : "TREASURE",
                 Math.max(1, wario2AnimationId)), cx, bannerY + 33f, 7, 0xFFF7E9D0, true);
            for (int i = 0; i < 12; i++) {
                double a = (Math.PI * 2.0 * i / 12.0) + phase * 1.3;
                float distance = 32f + phase * 70f + (i % 3) * 7f;
                float sx = cx + (float)Math.cos(a) * distance;
                float sy = cy + (float)Math.sin(a) * distance;
                float size = Math.max(1.5f, 4.5f * (1f - phase));
                rect(c, sx - size * 0.5f, sy - size * 0.5f, size, size,
                     (i % 3 == 0) ? 0xFFFFF3C5 : 0xFFFFC857);
            }
            postInvalidateOnAnimation();
        }

        private void drawWarioEmblem(Canvas c, float cx, float cy, float radius) {
            float t = SystemClock.uptimeMillis() / 1000f;
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x503B260B);
            c.drawCircle(cx, cy + 4, radius + 17, p);
            p.setColor(0x663A260F);
            c.drawCircle(cx, cy, radius + 8, p);
            p.setColor(0xFF30261B);
            c.drawCircle(cx, cy, radius, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(Math.max(2f, radius * 0.055f));
            p.setColor(0xFFFFD66B);
            c.drawCircle(cx, cy, radius - 4, p);
            p.setColor(0x99BD8537);
            c.drawCircle(cx, cy, radius - 12, p);
            p.setStyle(Paint.Style.FILL);

            // Original treasure-chest icon made from simple geometric shapes.
            float chestW = radius * 1.10f, chestH = radius * 0.55f;
            float left = cx - chestW * 0.5f, top = cy - chestH * 0.05f;
            p.setColor(0xFF8E4E22);
            c.drawRoundRect(left, top, left + chestW, top + chestH, 7, 7, p);
            p.setColor(0xFFC78335);
            c.drawRoundRect(left, top - chestH * 0.36f, left + chestW, top + chestH * 0.13f, 7, 7, p);
            p.setColor(0xFFFFD66B);
            c.drawRect(cx - radius * 0.08f, top - chestH * 0.34f,
                       cx + radius * 0.08f, top + chestH * 0.93f, p);
            c.drawRect(left + chestW * 0.12f, top + chestH * 0.54f,
                       left + chestW * 0.88f, top + chestH * 0.65f, p);
            p.setColor(0xFFFFF2B7);
            c.drawCircle(cx, top + chestH * 0.38f, Math.max(2f, radius * 0.075f), p);
            for (int i = 0; i < 6; i++) {
                double a = t * 0.7 + (Math.PI * 2.0 * i / 6.0);
                float orbit = radius + 11f + (float)Math.sin(t * 2.1f + i) * 2.5f;
                float sx = cx + (float)Math.cos(a) * orbit;
                float sy = cy + (float)Math.sin(a) * orbit;
                p.setColor((i % 2 == 0) ? 0xFFFFF0A8 : 0xFFDDA24B);
                c.drawRect(sx - 2, sy - 2, sx + 2, sy + 2, p);
            }
        }

        private void drawWarioVault(Canvas c, float x, float y, float w, float h) {
            card(c, x, y, w, h, "TREASURE VAULT");
            int count = warioTreasureCount();
            textFit(c, (warioTreasureMask >= 0 ? count + " / 15 TREASURES" : "-- / 15 TREASURES"),
                    x + 14, y + 49, w - 28, 12, ink());
            float bx = x + 14, by = y + 58, bw = w - 28;
            p.setColor(0x553A2B1B);
            c.drawRoundRect(bx, by, bx + bw, by + 7, 4, 4, p);
            p.setColor(accent());
            if (warioTreasureMask >= 0 && count > 0)
                c.drawRoundRect(bx, by, bx + bw * Math.min(1f, count / 15f), by + 7, 4, 4, p);
            float step = Math.min(28f, (w - 34f) / 15f);
            float first = x + (w - step * 15f) * 0.5f + step * 0.5f;
            float my = y + h - 18f;
            for (int i = 0; i < 15; i++) {
                boolean found = warioTreasureMask >= 0 && (warioTreasureMask & (1 << i)) != 0;
                drawTreasureMark(c, first + step * i, my, Math.min(5.5f, step * 0.22f), found);
            }
        }

        private void drawWarioStatus(Canvas c, float x, float y, float w, float h) {
            card(c, x, y, w, h, "EXPEDITION STATUS");
            float left = x + 14, right = x + w - 12;
            text(c, "LIVES", left, y + 48, 7.5f, muted());
            textRight(c, lives >= 0 ? String.format(Locale.US, "%02d", lives) : "--",
                      right, y + 48, 11, ink());
            text(c, "HEARTS", left, y + 68, 7.5f, muted());
            textRight(c, warioHearts >= 0 ? String.format(Locale.US, "%02d", warioHearts) : "--",
                      right, y + 68, 11, ink());
            text(c, "POWER", left, y + 88, 7.5f, muted());
            textRight(c, warioPowerName(warioPower), right, y + 88, 9.5f, accent());
            text(c, "TIME LEFT", left, y + 108, 7.5f, muted());
            textRight(c, warioTimeText(), right, y + 108, 11, ink());
        }

        private void drawExpeditionLog(Canvas c, float x, float y, float w, float h, int rows) {
            card(c, x, y, w, h, "EXPEDITION LOG");
            int available = Math.max(0, Math.min(rows, expeditionLog.length));
            float firstY = y + 45;
            float rowH = Math.min(15f, (h - 49f) / Math.max(1, available));
            for (int i = 0; i < available; i++) {
                String entry = expeditionLog[i];
                if (entry == null || entry.isEmpty()) continue;
                float ry = firstY + i * rowH;
                boolean treasure = entry.startsWith("TREASURE");
                rect(c, x + 14, ry - 6, 4, 4, treasure ? 0xFFFFD66B : edge());
                textFit(c, entry, x + 25, ry, w - 40, Math.min(8.5f, rowH * 0.67f),
                        treasure ? accent() : (i == 0 ? ink() : muted()));
            }
            if (available == 0 || expeditionLog[0] == null)
                text(c, "NO EVENTS RECORDED YET", x + 16, y + 48, 8, muted());
        }

        private void drawWarioLandscape(Canvas c) {
            c.save();
            headerPlate(c, 18, 7, 265, 49);
            text(c, "PIPE CLEAN  •  TREASURE EXPEDITION", 26, 23, 8.4f, accent());
            textFit(c, title(), 25, 43, 260, 15, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 552, 17, 34, 34, "×", RESET_BACKGROUND);
            button(c, 593, 13, 105, 42, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 704, 13, 72, 42, "CLOSE", "GAME", CLOSE_GAME, false);
            c.restore();

            drawWarioVault(c, 20, 70, 230, 133);
            drawWarioEmblem(c, 400, 139, 57);
            drawWarioStatus(c, 556, 70, 220, 133);
            card(c, 267, 213, 266, 94, "CURRENT COURSE");
            textFit(c, stageText(), 400, 255, 230, 20, accent(), true);
            textFit(c, "COIN BANK  " + warioCoinsText(), 400, 278, 230, 9.5f, ink(), true);
            drawExpeditionLog(c, 20, 213, 232, 94, 4);

            // Four familiar Companion Deck actions remain available without obscuring the log.
            c.save(); applyLayoutTransform(c, 2, 800f, 450f, 400f, 343f); drawDockLandscape(c); c.restore();
            c.save(); applyLayoutTransform(c, 3, 800f, 450f, 400f, 406.5f); drawSaveTimeline(c, 24, 365, 752, 83, false); c.restore();
        }

        private void drawWarioPortrait(Canvas c) {
            c.save(); applyLayoutTransform(c, 0, 700f, 800f, 350f, 30f);
            headerPlate(c, 14, 7, 400, 43);
            text(c, "PIPE CLEAN  •  TREASURE EXPEDITION", 20, 23, 8.2f, accent());
            textFit(c, title(), 20, 41, 385, 12.5f, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 416, 16, 20, 27, "×", RESET_BACKGROUND);
            button(c, 442, 12, 105, 37, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            button(c, 555, 12, 121, 37, "CLOSE", "GAME", CLOSE_GAME, false);
            c.restore();

            badge(c, 20, 55, 205, 47, "LIVES",
                  lives >= 0 ? String.format(Locale.US, "× %02d", lives) : "× --");
            badge(c, 238, 55, 442, 47, "CURRENT COURSE", stageText());

            card(c, 20, 111, 660, 96, "TREASURE VAULT");
            int count = warioTreasureCount();
            text(c, (warioTreasureMask >= 0 ? count + " / 15 FOUND" : "-- / 15 FOUND"), 34, 148, 11, accent());
            textFit(c, "TOTAL COINS  " + warioCoinsText(), 205, 148, 450, 9.5f, ink());
            float step = 42f, first = 42f, markY = 184f;
            for (int i = 0; i < 15; i++) {
                boolean found = warioTreasureMask >= 0 && (warioTreasureMask & (1 << i)) != 0;
                drawTreasureMark(c, first + step * i, markY, 7f, found);
            }

            p.setColor(panel());
            c.drawRoundRect(20, 216, 680, 265, 11, 11, p);
            p.setColor(edge()); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(1.2f);
            c.drawRoundRect(20.6f, 216.6f, 679.4f, 264.4f, 10, 10, p);
            p.setStyle(Paint.Style.FILL);
            text(c, "HEARTS  " + (warioHearts >= 0 ? String.format(Locale.US, "%02d", warioHearts) : "--"),
                 34, 236, 8.5f, ink());
            text(c, "POWER  " + warioPowerName(warioPower), 34, 254, 8.5f, accent());
            textRight(c, "TIME LEFT  " + warioTimeText(), 662, 254, 8.5f, ink());

            drawWarioEmblem(c, 350, 318, 38);
            drawExpeditionLog(c, 20, 363, 660, 76, 3);

            boolean idle = game < 0;
            c.save(); applyLayoutTransform(c, 2, 700f, 800f, 350f, 507f);
            button(c, 20, 455, 320, 48, "SAVE", idle ? "START A GAME FIRST" : "SNAPSHOT", SAVE, false);
            button(c, 360, 455, 320, 48, "LOAD", idle ? "START A GAME FIRST" : "RESTORE", LOAD, false);
            button(c, 20, 511, 320, 48, idle ? "PAUSE" : paused ? "RESUME" : "PAUSE",
                idle ? "AVAILABLE IN GAME" : paused ? "CONTINUE" : "HOLD GAME", PAUSE, false);
            button(c, 360, 511, 320, 48, "REWIND", idle ? "AVAILABLE IN GAME" : "HOLD TO REWIND", REWIND, true);
            c.restore();

            c.save(); applyLayoutTransform(c, 3, 700f, 800f, 350f, 750f);
            drawSaveTimeline(c, 14, 704, 672, 92, true);
            c.restore();
        }

                private void drawTreasureCelebration(Canvas c, float w, float h) {
            if (!treasureAnimationActive) return;
            long elapsed = SystemClock.uptimeMillis() - treasureAnimationStart;
            if (elapsed >= TREASURE_ANIMATION_MS) {
                treasureAnimationActive = false;
                return;
            }
            float phase = Math.max(0f, Math.min(1f, elapsed / (float)TREASURE_ANIMATION_MS));
            float cx = w * 0.5f, cy = h * (h > w ? 0.39f : 0.37f);
            int alpha = Math.max(0, Math.min(170, (int)((1f - phase) * 170f)));
            p.setStyle(Paint.Style.STROKE);
            for (int i = 0; i < 3; i++) {
                p.setStrokeWidth(2.5f - i * 0.45f);
                p.setColor((Math.max(0, alpha - i * 28) << 24) | 0x00FFD66B);
                c.drawCircle(cx, cy, 22f + phase * (95f + i * 20f), p);
            }
            p.setStyle(Paint.Style.FILL);
            float bannerW = Math.min(w - 28f, h > w ? 330f : 360f);
            float bannerY = cy + (h > w ? 58f : 72f);
            p.setColor(0xE82B2115);
            c.drawRoundRect(cx - bannerW * 0.5f, bannerY, cx + bannerW * 0.5f,
                            bannerY + 43f, 12, 12, p);
            p.setColor(0xFFFFD66B);
            p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(1.8f);
            c.drawRoundRect(cx - bannerW * 0.5f, bannerY, cx + bannerW * 0.5f,
                            bannerY + 43f, 12, 12, p);
            p.setStyle(Paint.Style.FILL);
            text(c, "TREASURE FOUND!", cx, bannerY + 18f, 10, 0xFFFFEBA5, true);
            text(c, String.format(Locale.US, "RELIC %02d ADDED TO YOUR VAULT", Math.max(1, treasureAnimationId)),
                 cx, bannerY + 33f, 7, 0xFFF7E9D0, true);
            for (int i = 0; i < 12; i++) {
                double a = (Math.PI * 2.0 * i / 12.0) + phase * 1.3;
                float distance = 32f + phase * 70f + (i % 3) * 7f;
                float sx = cx + (float)Math.cos(a) * distance;
                float sy = cy + (float)Math.sin(a) * distance;
                float size = Math.max(1.5f, 4.5f * (1f - phase));
                rect(c, sx - size * 0.5f, sy - size * 0.5f, size, size,
                     (i % 3 == 0) ? 0xFFFFF3C5 : 0xFFFFC857);
            }
            postInvalidateOnAnimation();
        }

        private void drawLandscape(Canvas c) {
            if (drMario()) { drawDrMarioLandscape(c); return; }
            c.save();
            applyLayoutTransform(c, 0, 800f, 450f, 400f, 31.5f);
            headerPlate(c, 18, 7, 265, 49);
            text(c, "PIPE CLEAN  •  POCKET QUEST", 26, 23, 10, accent());
            text(c, title(), 25, 43, 16, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 552, 17, 34, 34, "×", RESET_BACKGROUND);
            button(c, 593, 13, 105, 42, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            if (game >= 0) button(c, 704, 13, 72, 42, "CLOSE", "GAME", CLOSE_GAME, false);
            else button(c, 704, 13, 72, 42, "DECK", "LINKED", 0, false);
            c.restore();

            // Save-state thumbnails are represented by the timeline, avoiding a duplicate preview card.

            // Keep the reserve-item control as the visual centerpiece of the deck.
            c.save();
            applyLayoutTransform(c, 1, 800f, 450f, 400f, 164f);
            drawHeroItem(c, 400, 164, 128, item);
            c.restore();

            if (game < 0) {
                card(c, 620, 76, 156, 128, "DECK STATUS");
                drawMiniStar(c, 637, 112, 8);
                text(c, "SECOND SCREEN", 652, 110, 6.5f, muted());
                text(c, "ATTACHED", 652, 128, 12, ink());
                text(c, "NEXT STEP", 634, 151, 7.3f, muted());
                textFit(c, "LAUNCH A GAME", 634, 171, 126, 10, accent());
                text(c, "QUICK ACTIONS IN GAME", 634, 191, 6.2f, muted());
            } else {
                c.save();
                applyLayoutTransform(c, 4, 800f, 450f, 698f, 140f);
                card(c, 620, 76, 156, 128, "ADVENTURE STATUS");
                drawMiniStar(c, 637, 112, 8);
                text(c, "LIVES", 652, 110, 7, muted());
                text(c, String.format(Locale.US, "× %02d", lives), 652, 128, 12, ink());
                text(c, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE", 634, 151, 7.3f, muted());
                textFit(c, stageText(), 634, 171, 126, 11, accent());
                text(c, paused ? "PAUSED" : "IN PLAY", 634, 191, 8, muted());
                c.restore();
            }

            // Pin the timeline to the bottom edge; controls remain above it.
            c.save();
            applyLayoutTransform(c, 3, 800f, 450f, 400f, 406.5f);
            drawSaveTimeline(c, 24, 365, 752, 83, false);
            c.restore();
            c.save();
            applyLayoutTransform(c, 2, 800f, 450f, 400f, 343f);
            drawDockLandscape(c);
            c.restore();
        }

        private void drawDockLandscape(Canvas c) {
            float y=320, h=42;
            boolean idle = game < 0;
            button(c,94,y,145,h,"SAVE",idle?"START A GAME FIRST":"SNAPSHOT",SAVE,false);
            button(c,247,y,145,h,"LOAD",idle?"START A GAME FIRST":"RESTORE",LOAD,false);
            button(c,400,y,145,h,idle?"PAUSE":paused?"RESUME":"PAUSE",
                idle?"AVAILABLE IN GAME":paused?"CONTINUE":"HOLD GAME",PAUSE,false);
            button(c,553,y,153,h,"REWIND",idle?"AVAILABLE IN GAME":"HOLD TO REWIND",REWIND,true);
        }

        private void drawPortrait(Canvas c) {
            if (drMario()) { drawDrMarioPortrait(c); return; }
            if (wario2()) { drawWario2Portrait(c); return; }
            if (wario()) { drawWarioPortrait(c); return; }

            c.save();
            applyLayoutTransform(c, 0, 700f, 800f, 350f, 30f);
            headerPlate(c, 14, 7, 400, 43);
            text(c, "PIPE CLEAN  •  POCKET QUEST", 20, 23, 8.2f, accent());
            textFit(c, title(), 20, 41, 385, 13, ink());
            if (!wallpaperPath.isEmpty()) smallButton(c, 416, 16, 20, 27, "×", RESET_BACKGROUND);
            button(c, 442, 12, 105, 37, "BACKGROUND", "PNG / GIF", BACKGROUND, false);
            if (game >= 0) button(c, 555, 12, 121, 37, "CLOSE", "GAME", CLOSE_GAME, false);
            else button(c, 555, 12, 121, 37, "DECK", "LINKED", 0, false);
            c.restore();

            if (game < 0) {
                badge(c, 20, 55, 205, 47, "DECK STATUS", "LINKED");
                badge(c, 238, 55, 442, 47, "NEXT STEP", "LAUNCH A GAME");
            } else {
                badge(c, 20, 55, 205, 47, "LIVES", String.format(Locale.US, "× %02d", lives));
                badge(c, 238, 55, 442, 47, sml1() ? "WORLD / LEVEL" : "CURRENT STAGE", stageText());
            }

            c.save();
            applyLayoutTransform(c, 1, 700f, 800f, 350f, 280f);
            drawHeroItem(c, 350, 280, 132, item);
            c.restore();

            if (game >= 0) {
                card(c, 22, 143, 150, 134, "RESERVE ITEM");
                textFit(c, itemName(item), 34, 183, 126, 10, accent());
                text(c, "TAP TO USE", 34, 209, 8, muted());
                card(c, 528, 143, 150, 134, "ADVENTURE");
                text(c, "LIVES", 542, 181, 8, muted());
                text(c, String.format(Locale.US, "× %02d", lives), 542, 202, 13, ink());
                textFit(c, stageText(), 542, 230, 126, 9, accent());
            } else {
                card(c, 22, 143, 150, 134, "READY");
                text(c, "SECOND", 34, 183, 8, muted());
                text(c, "SCREEN", 34, 200, 8, muted());
                card(c, 528, 143, 150, 134, "QUICK TIP");
                textFit(c, "Launch a game", 540, 183, 126, 9, accent());
                textFit(c, "to enable actions", 540, 202, 126, 8, muted());
            }

            boolean idle = game < 0;
            c.save();
            applyLayoutTransform(c, 2, 700f, 800f, 350f, 507f);
            button(c, 20, 455, 320, 48, "SAVE", idle ? "START A GAME FIRST" : "SNAPSHOT", SAVE, false);
            button(c, 360, 455, 320, 48, "LOAD", idle ? "START A GAME FIRST" : "RESTORE", LOAD, false);
            button(c, 20, 511, 320, 48, idle ? "PAUSE" : paused ? "RESUME" : "PAUSE",
                idle ? "AVAILABLE IN GAME" : paused ? "CONTINUE" : "HOLD GAME", PAUSE, false);
            button(c, 360, 511, 320, 48, "REWIND", idle ? "AVAILABLE IN GAME" : "HOLD TO REWIND", REWIND, true);
            c.restore();

            c.save();
            applyLayoutTransform(c, 3, 700f, 800f, 350f, 750f);
            drawSaveTimeline(c, 14, 704, 672, 92, true);
            c.restore();
        }

                private void drawHeroItem(Canvas c, float cx, float cy, float radius, int itemId) {
            long now = SystemClock.uptimeMillis();
            float t = now / 1000f;
            float pulse = 1.0f + (float)Math.sin(t * 3.1f) * 0.025f;
            if (itemPressed) {
                float held = Math.min(1f, (now - itemPressStart) / 75f);
                pulse *= 1f - 0.13f * held;
            } else if (itemReleaseStart > 0 && now - itemReleaseStart < 220) {
                float u = (now - itemReleaseStart) / 220f;
                pulse *= 0.87f + 0.13f * (1f - (1f-u)*(1f-u));
            }

            int animatedItem = itemId;
            float itemDy = 0f, itemScale = 1f, phase = 1f;
            int itemAlpha = 255;
            int animation = layoutEditorPreview ? ITEM_ANIM_IDLE : itemAnimationKind;
            if (animation == ITEM_ANIM_ARRIVE && displayedItem > 0) {
                phase = Math.min(1f, Math.max(0f,
                    (now - itemAnimationStart) / (float)ITEM_ARRIVE_MS));
                float eased = 1f - (float)Math.pow(1f - phase, 3);
                itemDy = -radius * 0.48f * (1f - eased);
                itemScale = 0.25f + 0.75f * eased + (float)Math.sin(phase * Math.PI) * 0.16f;
                itemAlpha = Math.min(255, (int)(phase * 510f));
                animatedItem = displayedItem;
                if (phase >= 1f) {
                    itemAnimationKind = ITEM_ANIM_IDLE;
                    displayedItem = this.item;
                    animatedItem = itemId;
                    itemDy = 0f; itemScale = 1f; itemAlpha = 255;
                } else {
                    postInvalidateOnAnimation();
                }
            } else if (animation == ITEM_ANIM_SEND_OFF && displayedItem > 0) {
                phase = Math.min(1f, Math.max(0f,
                    (now - itemAnimationStart) / (float)ITEM_SEND_OFF_MS));
                itemDy = -radius * 1.12f * phase;
                itemScale = 1f - 0.48f * phase;
                itemAlpha = Math.max(0, (int)((1f - phase) * 255f));
                animatedItem = displayedItem;
                if (phase >= 1f) {
                    itemAnimationKind = ITEM_ANIM_IDLE;
                    displayedItem = this.item;
                    animatedItem = itemId;
                    itemDy = 0f; itemScale = 1f; itemAlpha = 255;
                } else {
                    postInvalidateOnAnimation();
                }
            } else if (!layoutEditorPreview) {
                itemDy = (float)Math.sin(t * 2.25f) * radius * 0.025f;
                itemScale = 1f + (float)Math.sin(t * 3.1f) * 0.018f;
            }

            c.save();
            c.translate(cx, cy);
            c.scale(pulse, pulse);
            c.translate(-cx, -cy);
            // The reserve socket and orbit stay stable while the item itself animates.
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

            for(int i=0;i<6;i++) {
                double a=(Math.PI*2.0*i/6.0)-rotation*0.55;
                float sr=radius+28+(float)Math.sin(t*2+i)*3;
                float sx=cx+(float)Math.cos(a)*sr, sy=cy+(float)Math.sin(a)*sr;
                drawSparkle(c,sx,sy,(i%2==0)?5:3,(i%3==0)?0xFFFFF3C8:gold());
            }
            if (animation == ITEM_ANIM_ARRIVE && phase < 1f && !layoutEditorPreview) {
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(3f);
                int alpha = Math.max(0, Math.min(170, (int)((1f - phase) * 170f)));
                p.setColor((alpha << 24) | (gold() & 0x00FFFFFF));
                c.drawCircle(cx, cy + itemDy, radius * (0.16f + phase * 0.72f), p);
                p.setStyle(Paint.Style.FILL);
            }

            int itemLayer = itemAlpha < 255 ? c.saveLayerAlpha(
                cx-radius*0.9f, cy-radius*1.75f, cx+radius*0.9f, cy+radius*0.8f, itemAlpha) : -1;
            c.save();
            c.translate(cx, cy + itemDy);
            c.scale(itemScale, itemScale);
            c.translate(-cx, -cy);
            /* The live SML2 RAM IDs are opposite to the Deck's semantic texture
             * slots: RAM 2 is Carrot and RAM 3 is Fire Flower. Keep editor
             * previews semantic (2 = Fire Flower, 3 = Carrot), swap only live data. */
            int shownItem = animatedItem;
            if (!layoutEditorPreview && sml2()) {
                if (shownItem == 2) shownItem = 3;
                else if (shownItem == 3) shownItem = 2;
            }
            if(shownItem>0) {
                int layoutGame = activeLayoutGame();
                int textureIndex = 11 + shownItem;
                if (layoutGame >= 0 && hasLayoutImage(layoutGame, textureIndex))
                    drawLayoutImageAsset(c, layoutGame, textureIndex, cx-radius*0.46f, cy-radius*0.46f,
                        radius*0.92f, radius*0.92f);
                else if (layoutGame >= 0 && hasLayoutImage(layoutGame, 2))
                    drawLayoutImageAsset(c, layoutGame, 2, cx-radius*0.46f, cy-radius*0.46f,
                        radius*0.92f, radius*0.92f);
                else
                    drawPowerUpGlyph(c,-radius*0.46f+cx,-radius*0.46f+cy,radius*0.92f,shownItem);
                textFit(c, itemName(shownItem), cx, cy + radius * 0.66f, radius * 1.55f,
                    Math.max(8f, radius * 0.11f), ink(), true);
                textFit(c, actionHint(shownItem), cx, cy + radius * 0.81f, radius * 1.68f,
                    Math.max(6.5f, radius * 0.065f), muted(), true);
            } else {
                drawPixelStar(c,cx,cy-radius*0.08f,17,0xFFB4A1CB);
            }
            c.restore();
            if (itemLayer >= 0) c.restoreToCount(itemLayer);
            c.restore();
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
            int layoutGame = activeLayoutGame();
            if (layoutGame >= 0 && hasLayoutImage(layoutGame, 15))
                drawLayoutImageAsset(c, layoutGame, 15, x + 2, y + 2, w - 4, h - 4);
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
            int layoutGame = activeLayoutGame();
            if ("ADVENTURE STATUS".equals(label) && layoutGame >= 0 && hasLayoutImage(layoutGame, 16))
                drawLayoutImageAsset(c, layoutGame, 16, x + 2, y + 2, w - 4, h - 4);
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
            int layoutGame = activeLayoutGame();
            p.setColor(action==downAction?accent():panel());
            c.drawRoundRect(x,y,x+w,y+h,13,13,p);
            if (layoutGame >= 0) drawLayoutButtonTexture(c, action, x, y, w, h);
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
            drawLayoutButtonTexture(c, action, x, y, w, h);
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

        private int dashboardHitTest(float x, float y, boolean portrait) {
            return 0; // The now-playing banner is informational; playlist controls stay in Audio & Menu.
        }

        private void inverseLayoutPoint(float[] point, int element, boolean portrait,
                                        float centerX, float centerY) {
            int g = activeLayoutGame();
            if (g < 0) return;
            int base = element * 3;
            boolean thor = isThorAspect();
            float referenceW = portrait ? 700f : 800f;
            float referenceH = thor ? 800f * 27f / 31f : portrait ? 800f : 450f;
            float dx = layoutValues[g][base] * (referenceW / 800f);
            float dy = layoutValues[g][base + 1] * (referenceH / 450f);
            float scaleX = Math.max(0.25f, Math.min(2.5f, layoutValues[g][base + 2] / 100f));
            float scaleY = Math.max(0.25f, Math.min(2.5f, layoutValues[g][37 + element] / 100f));
            point[0] = centerX + (point[0] - centerX - dx) / scaleX;
            point[1] = centerY + (point[1] - centerY - dy) / scaleY;
        }

        private int hitTest(float x,float y) {
            boolean thor = isThorAspect();
            boolean portrait = !thor && getHeight() > getWidth();
            if (layoutEditorPreview) return 0;
            if (dashboardMode) return dashboardHitTest(x, y, portrait);
            float[] point = new float[] {x, y};
            if (thor) {
                inverseLayoutPoint(point, 0, false, 400f, 34f);
                float hx = point[0], hy = point[1];
                if (inside(hx, hy, 542, 12, 112, 39)) return BACKGROUND;
                if (inside(hx, hy, 662, 12, 120, 39)) return game >= 0 ? CLOSE_GAME : 0;
                if (inside(hx, hy, 502, 16, 31, 31) && !wallpaperPath.isEmpty()) return RESET_BACKGROUND;

                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 3, false, 400f, 596f);
                int thorSlot = timelineHit(point[0], point[1], 20f, 510f, 760f,
                    (800f * 27f / 31f) - 520f, false);
                if (thorSlot != 0) return thorSlot;

                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 2, false, 400f, 449f);
                if (inside(point[0], point[1], 20, 420, 184, 48)) return SAVE;
                if (inside(point[0], point[1], 212, 420, 184, 48)) return LOAD;
                if (inside(point[0], point[1], 404, 420, 184, 48)) return PAUSE;
                if (inside(point[0], point[1], 596, 420, 184, 48)) return REWIND;

                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 1, false, 400f, 254f);
                if (inside(point[0], point[1], 240, 130, 320, 270)) return ITEM_USE;
                return 0;
            }
            inverseLayoutPoint(point, 0, portrait, portrait ? 350f : 400f, portrait ? 30f : 31.5f);
            float hx = point[0], hy = point[1];
            if (portrait) {
                if (inside(hx, hy, 416, 16, 20, 27) && !wallpaperPath.isEmpty()) return RESET_BACKGROUND;
                if (inside(hx, hy, 442, 12, 105, 37)) return BACKGROUND;
                if (inside(hx, hy, 555, 12, 121, 37)) return CLOSE_GAME;
                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 3, true, 350f, 750f);
                int selected = timelineHit(point[0], point[1], 14, 704, 672, 92, true);
                if (selected != 0) return selected;
                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 2, true, 350f, 507f);
                if (inside(point[0], point[1], 20, 455, 320, 48)) return SAVE;
                if (inside(point[0], point[1], 360, 455, 320, 48)) return LOAD;
                if (inside(point[0], point[1], 20, 511, 320, 48)) return PAUSE;
                if (inside(point[0], point[1], 360, 511, 320, 48)) return REWIND;
                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 1, true, 350f, drMario() ? 238f : 280f);
                if (drMario() && inside(point[0], point[1], 20, 104, 660, 235)) return ITEM_USE;
                if (!drMario() && inside(point[0], point[1], 200, 118, 300, 324)) return ITEM_USE;
            } else {
                if(inside(hx,hy,552,17,34,34) && !wallpaperPath.isEmpty()) return RESET_BACKGROUND;
                if(inside(hx,hy,593,13,105,42)) return BACKGROUND;
                if(inside(hx,hy,704,13,72,42)) return CLOSE_GAME;
                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 3, false, 400f, 406.5f);
                int selected = timelineHit(point[0], point[1], 24, 365, 752, 83, false);
                if (selected != 0) return selected;
                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 2, false, 400f, 343f);
                if(inside(point[0],point[1],94,320,145,42)) return SAVE;
                if(inside(point[0],point[1],247,320,145,42)) return LOAD;
                if(inside(point[0],point[1],400,320,145,42)) return PAUSE;
                if(inside(point[0],point[1],553,320,153,42)) return REWIND;
                point[0] = x; point[1] = y;
                inverseLayoutPoint(point, 1, false, 400f, 164f);
                if(inside(point[0],point[1],236,8,328,310)) return ITEM_USE;
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
            /* The editor manipulates the deck from the primary launcher; the
               secondary screen is a live preview, not a second control surface. */
            if (layoutEditorPreview) return true;
            boolean thor = isThorAspect();
            boolean portrait = !thor && getHeight() > getWidth();
            float dw = portrait ? 700f : 800f;
            float dh = thor ? 800f * 27f / 31f : portrait ? 800f : 450f;
            float scale = Math.min(getWidth() / dw, getHeight() / dh);
            float ox=(getWidth()-dw*scale)*0.5f, oy=(getHeight()-dh*scale)*0.5f;
            float x=(e.getX()-ox)/scale, y=(e.getY()-oy)/scale;
            int action = e.getActionMasked();

            if(action==MotionEvent.ACTION_DOWN) {
                downAction=hitTest(x,y);
                timelineTouchActive = !dashboardMode && (thor
                    ? inside(x, y, 20f, 524f, 760f, (800f * 27f / 31f) - 534f)
                    : isTimelinePoint(x, y, portrait));
                timelineMoved=false;
                if(timelineTouchActive) {
                    timelineDownX=x;
                    timelineDownY=y;
                    timelineStartFocus=timelineVisualFocus();
                    timelineDragFocus=timelineStartFocus;
                }
                if(downAction==ITEM_USE) { itemPressed=true; itemPressStart=SystemClock.uptimeMillis(); }
                if(downAction==REWIND) {
                    playLayoutSound(7);
                    send(REWIND,1);
                }
                invalidate();
                return true;
            }

            if(action==MotionEvent.ACTION_MOVE && timelineTouchActive) {
                float dx=x-timelineDownX;
                if(Math.abs(dx)>=14f && Math.abs(dx)>=Math.abs(y-timelineDownY)*1.10f)
                    timelineMoved=true;
                if(timelineMoved) {
                    float step = portrait ? 45f : thor ? 95f : 78f;
                    timelineDragFocus=clampFocus(timelineStartFocus-dx/step);
                    downAction=0;
                    invalidate();
                }
                return true;
            }

            if(action==MotionEvent.ACTION_UP) {
                if(timelineTouchActive) {
                    float dx=x-timelineDownX;
                    if(timelineMoved) {
                        float dragFocus=timelineDragFocus;
                        int selected=Math.max(0,Math.min(9,(int)Math.floor(dragFocus+0.5f)));
                        if(Math.abs(dx)>=18f && selected==(int)timelineStartFocus) {
                            selected=Math.max(0,Math.min(9,(int)timelineStartFocus+(dx<0?1:-1)));
                        }
                        animatedSlot=dragFocus;
                        slot=selected;
                        send(SLOT_SELECT_BASE+selected,1);
                    } else if(downAction>=SLOT_SELECT_BASE && downAction<SLOT_SELECT_BASE+10) {
                        int selected=downAction-SLOT_SELECT_BASE;
                        slot=selected;
                        send(downAction,1);
                    }
                    timelineTouchActive=false;
                    timelineMoved=false;
                    downAction=0;
                    invalidate();
                    performClick();
                    return true;
                }

                int up=hitTest(x,y);
                if(downAction==REWIND) send(REWIND,0);
                else if(up!=0 && up==downAction) {
                    if (up == SAVE) playLayoutSound(4);
                    else if (up == LOAD) playLayoutSound(5);
                    else if (up == PAUSE) playLayoutSound(6);
                    else if (up == BACKGROUND) playLayoutSound(8);
                    else if (up == CLOSE_GAME) playLayoutSound(9);
                    else if (up == ITEM_USE && item > 0) startReserveItemSendOff(item);
                    send(up,1);
                }
                if(downAction==ITEM_USE) {
                    itemPressed=false;
                    itemReleaseStart=SystemClock.uptimeMillis();
                    if (up == ITEM_USE) playLayoutSound(item > 0 ? 10 : 11);
                }
                downAction=0;
                invalidate();
                performClick();
                return true;
            }

            if(action==MotionEvent.ACTION_CANCEL) {
                if(downAction==REWIND) send(REWIND,0);
                if(downAction==ITEM_USE) { itemPressed=false; itemReleaseStart=SystemClock.uptimeMillis(); }
                timelineTouchActive=false;
                timelineMoved=false;
                downAction=0;
                invalidate();
                return true;
            }
            return true;
        }

        @Override public boolean performClick() { super.performClick(); return true; }
    }
}
