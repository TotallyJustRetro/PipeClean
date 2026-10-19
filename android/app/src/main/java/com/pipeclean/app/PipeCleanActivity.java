package com.pipeclean.app;

import android.app.Activity;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/** SDL host activity with a Storage Access Framework bridge for native file dialogs. */
public final class PipeCleanActivity extends SDLActivity {
    private static final int PICK_DOCUMENT = 0x5043;
    private static final int PICK_TREE = 0x5044;
    private static final int PICK_COMPANION_BACKGROUND = 0x5045;
    private static final int PICK_COMPANION_LAYOUT = 0x5046;
    private static final int PICK_COMPANION_LAYOUT_EXPORT = 0x5047;
    private static final String COMPANION_PREFS = "PipeCleanCompanion";
    private static final String COMPANION_BACKGROUND_KEY = "background_path";
    private static volatile int pendingKind;
    private int pendingDeckGame = -1;
    private int pendingDeckKind;
    private String pendingDeckManifest = "";
    private String[] pendingDeckAssetPaths = new String[38];
    private DisplayManager companionDisplayManager;
    private PipeCleanCompanionPresentation companionPresentation;
    private boolean companionEnabled;
    private boolean companionPipeSwapMode;

    private final DisplayManager.DisplayListener companionDisplayListener =
        new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int displayId) {
                runOnUiThread(PipeCleanActivity.this::showCompanionDisplay);
            }
            @Override public void onDisplayRemoved(int displayId) {
                runOnUiThread(PipeCleanActivity.this::showCompanionDisplay);
            }
            @Override public void onDisplayChanged(int displayId) {
                runOnUiThread(PipeCleanActivity.this::showCompanionDisplay);
            }
        };

    public static native void nativeDocumentPicked(String path);
    public static native void nativeCompanionAction(int action, int pressed);
    public static native void nativeCompanionDisplayAvailable(int available);
    public static native void nativeCompanionLayoutPickerResult(int kind, int game, String path);

    /*
     * Android 12+ can expose individual vibrator IDs through VibratorManager.
     * Older Android versions expose a single default system vibrator.
     */
    private Vibrator pipeCleanVibratorForSlot(int slot) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager manager = (VibratorManager)getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            if (manager == null) return null;
            if (slot < 0) return manager.getDefaultVibrator();
            int[] ids = manager.getVibratorIds();
            if (slot >= ids.length) return null;
            return manager.getVibrator(ids[slot]);
        }
        if (slot > 0) return null;
        return (Vibrator)getSystemService(Context.VIBRATOR_SERVICE);
    }

    public int pipeCleanVibratorCount() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager manager = (VibratorManager)getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            if (manager != null) {
                int count = manager.getVibratorIds().length;
                if (count > 0) return count;
                Vibrator fallback = manager.getDefaultVibrator();
                return fallback != null && fallback.hasVibrator() ? 1 : 0;
            }
        }
        Vibrator vibrator = (Vibrator)getSystemService(Context.VIBRATOR_SERVICE);
        return vibrator != null && vibrator.hasVibrator() ? 1 : 0;
    }

    public String pipeCleanVibratorName(int slot) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager manager = (VibratorManager)getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            if (manager != null) {
                int[] ids = manager.getVibratorIds();
                if (slot >= 0 && slot < ids.length)
                    return "Phone actuator (ID " + ids[slot] + ")";
            }
        }
        return slot == 0 ? "Android phone vibrator" : "Phone actuator unavailable";
    }

    public boolean pipeCleanPulseVibrator(int slot, int durationMs, int amplitude) {
        Vibrator vibrator = pipeCleanVibratorForSlot(slot);
        if (vibrator == null || !vibrator.hasVibrator() || durationMs <= 0) return false;
        int safeAmplitude = Math.max(1, Math.min(255, amplitude));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            vibrator.vibrate(VibrationEffect.createOneShot(durationMs, safeAmplitude));
        } else {
            vibrator.vibrate(durationMs);
        }
        return true;
    }

    public void pipeCleanCancelVibrator(int slot) {
        Vibrator vibrator = pipeCleanVibratorForSlot(slot);
        if (vibrator != null) vibrator.cancel();
    }

    @Override
    protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        companionDisplayManager = (DisplayManager)getSystemService(DISPLAY_SERVICE);
        if (companionDisplayManager != null) {
            companionDisplayManager.registerDisplayListener(
                companionDisplayListener, new Handler(Looper.getMainLooper()));
            showCompanionDisplay();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        showCompanionDisplay();
    }

    @Override
    protected void onDestroy() {
        if (companionDisplayManager != null) {
            try { companionDisplayManager.unregisterDisplayListener(companionDisplayListener); }
            catch (Exception ignored) {}
        }
        dismissCompanionDisplay();
        super.onDestroy();
    }

    private void showCompanionDisplay() {
        // The companion screen is part of the app shell, not a temporary game dialog.
        // Keep it present whenever a secondary display exists, including the game picker
        // and idle state. Only the app's destruction or display removal tears it down.
        if (companionDisplayManager == null || isFinishing()) {
            dismissCompanionDisplay();
            return;
        }
        Display primary = getDisplay();
        int primaryId = primary != null ? primary.getDisplayId() : Display.DEFAULT_DISPLAY;
        Display target = null, fallback = null;
        for (Display display : companionDisplayManager.getDisplays()) {
            if (display == null || !display.isValid() || display.getDisplayId() == primaryId)
                continue;
            if (fallback == null) fallback = display;
            if ((display.getFlags() & Display.FLAG_PRESENTATION) != 0) {
                target = display;
                break;
            }
        }
        if (target == null) target = fallback;
        if (target == null) {
            dismissCompanionDisplay();
            nativeCompanionDisplayAvailable(0);
            return;
        }
        if (companionPresentation != null) {
            Display current = companionPresentation.getDisplay();
            if (current != null && current.getDisplayId() == target.getDisplayId()) {
                try {
                    // If Android or an input key dismissed the Presentation, recreate it
                    // on the same display instead of leaving a blank lower screen.
                    if (!companionPresentation.isShowing()) companionPresentation.show();
                    if (companionPresentation.isShowing()) {
                        companionPresentation.setPipeSwapMode(companionEnabled && companionPipeSwapMode);
                        nativeCompanionDisplayAvailable(1);
                        return;
                    }
                } catch (Exception ex) {
                    android.util.Log.w("PipeClean", "Couldn't restore the companion display.", ex);
                }
            }
            dismissCompanionDisplay();
        }
        try {
            PipeCleanCompanionPresentation created = new PipeCleanCompanionPresentation(this, target);
            created.setOnDismissListener(dialog -> new Handler(Looper.getMainLooper()).post(() -> {
                // Belt-and-suspenders recovery: accidental dismissals must not detach
                // the lower screen from the app. Explicit teardown clears our reference
                // before this callback runs, so closing the app remains allowed.
                if (!isFinishing() && !isDestroyed() &&
                    companionPresentation == created && !created.isShowing())
                    showCompanionDisplay();
            }));
            companionPresentation = created;
            created.show();
            created.setPipeSwapMode(companionEnabled && companionPipeSwapMode);
            nativeCompanionDisplayAvailable(1);
        } catch (Exception ex) {
            dismissCompanionDisplay();
            nativeCompanionDisplayAvailable(0);
            android.util.Log.w("PipeClean", "Couldn't open the companion display.", ex);
        }
    }

    private void dismissCompanionDisplay() {
        if (companionPresentation != null) {
            try {
                // The shared deck View may currently be overlaid on the primary
                // SDL surface. Return it to the Presentation before dismissing so
                // it cannot remain as a stale full-screen overlay.
                companionPresentation.setPipeSwapMode(false);
                companionPresentation.dismiss();
            } catch (Exception ignored) {}
            companionPresentation = null;
        }
        nativeCompanionDisplayAvailable(0);
    }

    /** Open Android's document picker for a custom second-screen wallpaper. */
    public void openCompanionBackgroundPicker() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("image/*");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"image/png", "image/gif"});
                intent.putExtra(Intent.EXTRA_TITLE, "Choose a PNG or GIF wallpaper");
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                startActivityForResult(intent, PICK_COMPANION_BACKGROUND);
            } catch (Exception ex) {
                Toast.makeText(this, "Couldn't open the wallpaper picker.", Toast.LENGTH_SHORT).show();
                android.util.Log.w("PipeClean", "Couldn't open companion wallpaper picker.", ex);
            }
        });
    }

    /** Restore the game's built-in artwork and forget the custom wallpaper. */
    public void resetCompanionBackground() {
        runOnUiThread(() -> {
            String previous = getSharedPreferences(COMPANION_PREFS, MODE_PRIVATE)
                .getString(COMPANION_BACKGROUND_KEY, "");
            getSharedPreferences(COMPANION_PREFS, MODE_PRIVATE).edit()
                .remove(COMPANION_BACKGROUND_KEY).apply();
            if (companionPresentation != null) companionPresentation.setBackgroundFile("");
            if (previous != null && !previous.isEmpty()) {
                try { new File(previous).delete(); } catch (Exception ignored) {}
            }
            Toast.makeText(this, "Default companion artwork restored.", Toast.LENGTH_SHORT).show();
        });
    }

    private String copyCompanionBackground(Uri uri) {
        if (uri == null) return "";
        String name = documentName(uri).toLowerCase(java.util.Locale.US);
        String mime = getContentResolver().getType(uri);
        boolean gif = name.endsWith(".gif") || "image/gif".equalsIgnoreCase(mime);
        boolean png = name.endsWith(".png") || "image/png".equalsIgnoreCase(mime);
        if (!gif && !png) {
            Toast.makeText(this, "Please choose a PNG or GIF image.", Toast.LENGTH_LONG).show();
            return "";
        }
        File folder = new File(getFilesDir(), "PipeCleanCompanion");
        if (!folder.exists() && !folder.mkdirs()) {
            Toast.makeText(this, "Couldn't create the wallpaper folder.", Toast.LENGTH_SHORT).show();
            return "";
        }
        String extension = gif ? ".gif" : ".png";
        File target = new File(folder, "wallpaper-" + System.currentTimeMillis() + extension);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(target)) {
            if (in == null) throw new Exception("No input stream");
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
            if (target.length() == 0) throw new Exception("Empty wallpaper");
        } catch (Exception ex) {
            try { target.delete(); } catch (Exception ignored) {}
            Toast.makeText(this, "Couldn't read that wallpaper.", Toast.LENGTH_LONG).show();
            android.util.Log.w("PipeClean", "Couldn't copy companion wallpaper.", ex);
            return "";
        }

        String previous = getSharedPreferences(COMPANION_PREFS, MODE_PRIVATE)
            .getString(COMPANION_BACKGROUND_KEY, "");
        getSharedPreferences(COMPANION_PREFS, MODE_PRIVATE).edit()
            .putString(COMPANION_BACKGROUND_KEY, target.getAbsolutePath()).apply();
        if (companionPresentation != null)
            companionPresentation.setBackgroundFile(target.getAbsolutePath());
        if (previous != null && !previous.isEmpty() && !previous.equals(target.getAbsolutePath())) {
            try { new File(previous).delete(); } catch (Exception ignored) {}
        }
        Toast.makeText(this, gif ? "Animated GIF wallpaper applied." : "PNG wallpaper applied.",
                       Toast.LENGTH_SHORT).show();
        return target.getAbsolutePath();
    }

    /** Native game-loop status bridge; UI work is moved onto Android's main thread. */
    public void updateCompanionState(int game, int lives, int stage, int world,
                                     int paused, int slot, int palette, int filter,
                                     int item, int dualScreenEnabled, int drMarioScore,
                                     int drMarioLevel, int drMarioSpeed, int drMarioCapsule,
                                     int warioCoins, int warioHearts, int warioPower,
                                     int warioTreasures, int warioTime,
                                      int wario3Keys, int wario3MusicCoins, int wario3Scene) {
        runOnUiThread(() -> {
            companionEnabled = dualScreenEnabled != 0 &&
                (game == 0 || game == 1 || game == 2 || game == 3 ||
                 game == 4 || game == 5 || game == 6);
            // Always retain the second-screen deck. Game-specific widgets are live only
            // when the selected game has Companion support enabled; otherwise show a
            // useful READY screen rather than dismissing the entire lower display.
            showCompanionDisplay();
            if (companionPresentation != null) {
                if (companionEnabled) {
                    companionPresentation.updateState(game, lives, stage, world,
                        paused != 0, slot, palette, filter, item, drMarioScore,
                        drMarioLevel, drMarioSpeed, drMarioCapsule, warioCoins,
                        warioHearts, warioPower, warioTreasures, warioTime,
                        wario3Keys, wario3MusicCoins, wario3Scene);
                    companionPresentation.setPipeSwapMode(companionPipeSwapMode);
                } else {
                    companionPipeSwapMode = false;
                    companionPresentation.showIdleState();
                    companionPresentation.setPipeSwapMode(false);
                }
            }
        });
    }

    /** Publish launcher metadata and the cached game preview to the pre-game dashboard. */
    public void updateLauncherDashboard(int selectedGame, String[] gameNames,
                                        int[] recentGames, int[] playSeconds,
                                        int previewWidth, int previewHeight, int[] previewPixels,
                                        int layoutGame, int[] layoutValues, String[] layoutPaths,
                                        boolean dualScreenEnabled, boolean editorPreview,
                                         int editorPreviewItem) {
        runOnUiThread(() -> {
            showCompanionDisplay();
            if (companionPresentation != null) {
                companionPresentation.setLauncherDashboardState(
                    selectedGame, gameNames, recentGames, playSeconds,
                    previewWidth, previewHeight, previewPixels, dualScreenEnabled);
                companionPresentation.setDeckLayout(layoutGame, layoutValues, layoutPaths,
                    editorPreview, editorPreviewItem);
            }
        });
    }

    /** Publish current track state and a one-shot now-playing notification to the deck. */
    public void updateCompanionMusicPlaylist(String currentTitle, String[] playlistNames,
                                             int selectedTrack, boolean playing,
                                             int notificationSerial, boolean notificationAllowed) {
        runOnUiThread(() -> {
            if (companionPresentation != null)
                companionPresentation.setMusicPlaylist(
                    currentTitle, playlistNames, selectedTrack, playing,
                    notificationSerial, notificationAllowed);
        });
    }

    /** Publish save-state paths for the Companion Deck timeline. */
    public void updateCompanionSaveSlots(int game, int selectedSlot,
                                         String[] statePaths, String[] thumbnailPaths) {
        if (statePaths == null || thumbnailPaths == null) return;
        runOnUiThread(() -> {
            if (companionPresentation != null)
                companionPresentation.updateSaveSlots(game, selectedSlot, statePaths, thumbnailPaths);
        });
    }

    /** Swap the two display roles while SML1 is traversing a pipe/bonus room. */
    public void setCompanionPipeSwapMode(boolean enabled) {
        runOnUiThread(() -> {
            companionPipeSwapMode = enabled;
            if (companionPresentation != null)
                companionPresentation.setPipeSwapMode(companionEnabled && companionPipeSwapMode);
        });
    }

    /** Push a filtered emulator frame to the secondary display at a capped rate. */
    public void updateCompanionGameFrame(int width, int height, int[] pixels) {
        if (width <= 0 || height <= 0 || pixels == null ||
            (long)width * (long)height != pixels.length) return;
        runOnUiThread(() -> {
            if (companionPipeSwapMode && companionPresentation != null)
                companionPresentation.updateGameFrame(width, height, pixels);
        });
    }

    /** Enable game-specific deck controls, or return the still-visible deck to idle. */
    public void setCompanionEnabled(boolean enabled) {
        runOnUiThread(() -> {
            companionEnabled = enabled;
            showCompanionDisplay();
            if (!companionEnabled && companionPresentation != null) {
                companionPipeSwapMode = false;
                companionPresentation.showIdleState();
                companionPresentation.setPipeSwapMode(false);
            } else if (companionPresentation != null) {
                companionPresentation.setPipeSwapMode(companionPipeSwapMode);
            }
        });
    }

    /** Opens image/audio selectors or a portable layout-bundle import/export flow. */
    public void openCompanionLayoutPicker(int kind, int game, String manifest, String[] assetPaths) {
        runOnUiThread(() -> {
            pendingDeckKind = kind;
            pendingDeckGame = game;
            pendingDeckManifest = manifest == null ? "" : manifest;
            pendingDeckAssetPaths = assetPaths == null ? new String[38] : assetPaths.clone();
            try {
                if (kind == 14) {
                    Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    intent.setType("application/zip");
                    intent.putExtra(Intent.EXTRA_TITLE, "PipeClean-CompanionDeck-" +
                        deckGameSlug(game) + ".zip");
                    intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, PICK_COMPANION_LAYOUT_EXPORT);
                } else {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    if ((kind >= 1 && kind <= 4) || (kind >= 15 && kind <= 25)) {
                        intent.setType("image/*");
                        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                            "image/png", "image/gif"
                        });
                    } else if (kind >= 5 && kind <= 12) {
                        intent.setType("audio/*");
                        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                            "audio/wav", "audio/x-wav", "audio/mpeg", "audio/ogg", "audio/flac"
                        });
                    } else {
                        intent.setType("application/zip");
                        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                            "application/zip", "application/x-zip-compressed", "application/octet-stream"
                        });
                    }
                    intent.putExtra(Intent.EXTRA_TITLE, deckPickerTitle(kind));
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, PICK_COMPANION_LAYOUT);
                }
            } catch (Exception ex) {
                Toast.makeText(this, "Couldn't open the Companion Deck picker.", Toast.LENGTH_SHORT).show();
                nativeCompanionLayoutPickerResult(kind, game, "");
                android.util.Log.w("PipeClean", "Couldn't open Companion Deck picker.", ex);
            }
        });
    }

    private String deckPickerTitle(int kind) {
        switch (kind) {
            case 1: return "Choose Companion Deck background (PNG/GIF)";
            case 2: return "Choose Companion Deck button texture (PNG/GIF)";
            case 3: return "Choose Companion Deck item texture (PNG/GIF)";
            case 4: return "Choose a decorative image (PNG/GIF)";
            case 15: return "Choose Mushroom item texture (PNG/GIF)";
            case 16: return "Choose Fire Flower / Superball Flower texture (PNG/GIF)";
            case 17: return "Choose Carrot item texture (PNG/GIF)";
            case 18: return "Choose custom header texture (PNG/GIF)";
            case 19: return "Choose Adventure Status texture (PNG/GIF)";
            case 20: return "Choose Save button texture (PNG/GIF)";
            case 21: return "Choose Load button texture (PNG/GIF)";
            case 22: return "Choose Pause/Resume button texture (PNG/GIF)";
            case 23: return "Choose Rewind button texture (PNG/GIF)";
            case 24: return "Choose Background button texture (PNG/GIF)";
            case 25: return "Choose Close Game button texture (PNG/GIF)";
            case 5: return "Choose Save button sound";
            case 6: return "Choose Load button sound";
            case 7: return "Choose Pause button sound";
            case 8: return "Choose Rewind button sound";
            case 9: return "Choose Background button sound";
            case 10: return "Choose Close button sound";
            case 11: return "Choose item-use sound";
            case 12: return "Choose empty-item-slot sound";
            default: return "Import Companion Deck layout bundle";
        }
    }

    private String deckGameSlug(int game) {
        switch (game) {
            case 0: return "dr-mario";
            case 1: return "super-mario-land";
            case 2: return "super-mario-land-2";
            case 3: return "wario-super-mario-land-3";
            case 4: return "wario-land-2-gb";
            case 5: return "wario-land-3";
            case 6: return "wario-land-2-gbc";
            default: return "game-" + game;
        }
    }

    private File deckLayoutRoot(int game, String suffix) throws Exception {
        File root = new File(getFilesDir(), "PipeCleanDeckLayouts/" + deckGameSlug(game) + suffix);
        if (!root.exists() && !root.mkdirs()) throw new Exception("Can't create Companion Deck asset folder");
        return root;
    }

    private static String lowerExtension(String name) {
        if (name == null) return "";
        int slash = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
        int dot = name.lastIndexOf('.');
        return dot > slash ? name.substring(dot).toLowerCase(java.util.Locale.US) : "";
    }

    private String copyDeckAsset(Uri uri, int game, int kind) {
        if (uri == null) return "";
        String name = documentName(uri);
        String ext = lowerExtension(name);
        boolean image = (kind >= 1 && kind <= 4) || (kind >= 15 && kind <= 25);
        boolean valid = image
            ? (".png".equals(ext) || ".gif".equals(ext))
            : (".wav".equals(ext) || ".mp3".equals(ext) || ".ogg".equals(ext) || ".flac".equals(ext));
        if (!valid) {
            Toast.makeText(this, image ? "Use a transparent PNG or animated GIF." :
                "Use a WAV, MP3, OGG, or FLAC sound.", Toast.LENGTH_LONG).show();
            return "";
        }
        try {
            File root = deckLayoutRoot(game, "/assets");
            File target = new File(root, "asset-" + kind + "-" +
                System.currentTimeMillis() + ext);
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(target)) {
                if (in == null) throw new Exception("No input stream");
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
            }
            if (target.length() == 0) throw new Exception("Empty asset");
            return target.getAbsolutePath();
        } catch (Exception ex) {
            Toast.makeText(this, "Couldn't import that asset.", Toast.LENGTH_LONG).show();
            android.util.Log.w("PipeClean", "Couldn't copy Companion Deck asset.", ex);
            return "";
        }
    }

    private String importDeckBundle(Uri uri, int game) {
        if (uri == null) return "";
        try {
            File root = deckLayoutRoot(game, "/import-" + System.currentTimeMillis());
            String canonicalRoot = root.getCanonicalPath() + File.separator;
            int totalBytes = 0, count = 0;
            try (InputStream raw = getContentResolver().openInputStream(uri);
                 ZipInputStream zip = new ZipInputStream(raw)) {
                if (raw == null) throw new Exception("No bundle input stream");
                ZipEntry entry;
                byte[] buffer = new byte[32 * 1024];
                while ((entry = zip.getNextEntry()) != null) {
                    if (++count > 128) throw new Exception("Too many archive entries");
                    File out = new File(root, entry.getName());
                    String canonical = out.getCanonicalPath();
                    if (!canonical.startsWith(canonicalRoot))
                        throw new Exception("Unsafe archive path");
                    if (entry.isDirectory()) {
                        if (!out.mkdirs() && !out.isDirectory()) throw new Exception("Can't create folder");
                    } else {
                        File parent = out.getParentFile();
                        if (parent != null && !parent.exists() && !parent.mkdirs())
                            throw new Exception("Can't create archive folder");
                        try (OutputStream stream = new FileOutputStream(out)) {
                            int n;
                            while ((n = zip.read(buffer)) >= 0) {
                                totalBytes += n;
                                if (totalBytes > 40 * 1024 * 1024)
                                    throw new Exception("Bundle is larger than 40 MB");
                                stream.write(buffer, 0, n);
                            }
                        }
                    }
                    zip.closeEntry();
                }
            }
            File manifest = new File(root, "manifest.ini");
            if (!manifest.isFile() || manifest.length() == 0)
                throw new Exception("Bundle is missing manifest.ini");
            return manifest.getAbsolutePath();
        } catch (Exception ex) {
            Toast.makeText(this, "Couldn't import that layout bundle.", Toast.LENGTH_LONG).show();
            android.util.Log.w("PipeClean", "Couldn't extract Companion Deck bundle.", ex);
            return "";
        }
    }

    private String deckAssetField(int index) {
        switch (index) {
            case 0: return "background_path";
            case 1: return "button_texture_path";
            case 2: return "item_texture_path";
            case 3: return "decoration_path";
            case 4: return "button_sound_path0";
            case 5: return "button_sound_path1";
            case 6: return "button_sound_path2";
            case 7: return "button_sound_path3";
            case 8: return "button_sound_path4";
            case 9: return "button_sound_path5";
            case 10: return "item_sound_path";
            case 11: return "empty_item_sound_path";
            case 12: return "item_texture_path_1";
            case 13: return "item_texture_path_2";
            case 14: return "item_texture_path_3";
            case 15: return "header_texture_path";
            case 16: return "status_texture_path";
            case 17: return "button_texture_save_path";
            case 18: return "button_texture_load_path";
            case 19: return "button_texture_pause_path";
            case 20: return "button_texture_rewind_path";
            case 21: return "decoration_path_1";
            case 22: return "decoration_path_2";
            case 23: return "decoration_path_3";
            case 24: return "decoration_path_4";
            case 25: return "decoration_path_5";
            case 26: return "decoration_path_6";
            case 27: return "decoration_path_7";
            case 28: return "button_texture_background_path";
            case 29: return "button_texture_close_path";
            case 30: return "decoration_path_8";
            case 31: return "decoration_path_9";
            case 32: return "decoration_path_10";
            case 33: return "decoration_path_11";
            case 34: return "decoration_path_12";
            case 35: return "decoration_path_13";
            case 36: return "decoration_path_14";
            case 37: return "decoration_path_15";
            default: return "empty_item_sound_path";
        }
    }

    private String deckAssetEntryName(int index, String path) {
        String ext = lowerExtension(path);
        if (ext.isEmpty() || ext.length() > 8) ext = ".bin";
        String[] names = {"background", "button-texture", "item-texture", "decoration",
            "save-sound", "load-sound", "pause-sound", "rewind-sound",
            "background-sound", "close-sound", "item-sound", "empty-item-sound",
            "mushroom-texture", "special-item-texture", "carrot-texture",
            "header-texture", "status-texture", "save-button-texture", "load-button-texture",
            "pause-button-texture", "rewind-button-texture",
            "decoration-2", "decoration-3", "decoration-4", "decoration-5",
            "decoration-6", "decoration-7", "decoration-8",
            "background-button-texture", "close-button-texture",
            "decoration-9", "decoration-10", "decoration-11", "decoration-12",
            "decoration-13", "decoration-14", "decoration-15", "decoration-16"};
        return "assets/" + names[index] + ext;
    }

    private void exportDeckBundle(Uri uri, String manifest, String[] paths) throws Exception {
        if (uri == null) throw new Exception("No export destination");
        if (paths == null) paths = new String[38];
        StringBuilder fullManifest = new StringBuilder(manifest == null ? "" : manifest);
        if (fullManifest.length() > 0 &&
            fullManifest.charAt(fullManifest.length() - 1) != '\n') fullManifest.append('\n');
        try (OutputStream raw = getContentResolver().openOutputStream(uri, "wt");
             ZipOutputStream zip = new ZipOutputStream(raw)) {
            if (raw == null) throw new Exception("No destination stream");
            byte[] buffer = new byte[32 * 1024];
            for (int i = 0; i < 38; i++) {
                String path = i < paths.length ? paths[i] : "";
                if (path == null || path.isEmpty()) continue;
                File file = new File(path);
                if (!file.isFile() || file.length() <= 0) continue;
                String entryName = deckAssetEntryName(i, path);
                fullManifest.append(deckAssetField(i)).append('=').append(entryName).append('\n');
                zip.putNextEntry(new ZipEntry(entryName));
                try (InputStream in = new FileInputStream(file)) {
                    int n;
                    while ((n = in.read(buffer)) >= 0) zip.write(buffer, 0, n);
                }
                zip.closeEntry();
            }
            zip.putNextEntry(new ZipEntry("manifest.ini"));
            zip.write(fullManifest.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            zip.closeEntry();
        }
    }

    public void openPipeCleanDocumentPicker(int kind, String title) {
        runOnUiThread(() -> {
            pendingKind = kind;
            try {
                Intent intent;
                if (kind == 4) {
                    intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, PICK_TREE);
                } else {
                    intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    if (kind == 2) intent.setType("image/*");
                    else if (kind == 3) intent.setType("audio/*");
                    else {
                        intent.setType("*/*");
                        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                            "application/octet-stream", "application/x-gameboy-rom",
                            "application/zip", "application/x-ips"
                        });
                    }
                    intent.putExtra(Intent.EXTRA_TITLE, title);
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, PICK_DOCUMENT);
                }
            } catch (Exception ex) {
                nativeDocumentPicked("");
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_COMPANION_LAYOUT) {
            String result = "";
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri uri = data.getData();
                result = pendingDeckKind == 13
                    ? importDeckBundle(uri, pendingDeckGame)
                    : copyDeckAsset(uri, pendingDeckGame, pendingDeckKind);
            }
            nativeCompanionLayoutPickerResult(pendingDeckKind, pendingDeckGame,
                result == null ? "" : result);
            return;
        }
        if (requestCode == PICK_COMPANION_LAYOUT_EXPORT) {
            String result = "";
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                try {
                    exportDeckBundle(data.getData(), pendingDeckManifest, pendingDeckAssetPaths);
                    result = "EXPORTED";
                    Toast.makeText(this, "Companion Deck bundle exported.", Toast.LENGTH_LONG).show();
                } catch (Exception ex) {
                    Toast.makeText(this, "Couldn't export that layout bundle.", Toast.LENGTH_LONG).show();
                    android.util.Log.w("PipeClean", "Couldn't export Companion Deck bundle.", ex);
                }
            }
            nativeCompanionLayoutPickerResult(pendingDeckKind, pendingDeckGame, result);
            return;
        }
        if (requestCode == PICK_COMPANION_BACKGROUND) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null)
                copyCompanionBackground(data.getData());
            return;
        }
        if (requestCode != PICK_DOCUMENT && requestCode != PICK_TREE) return;
        String result = "";
        if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION |
                         (requestCode == PICK_TREE ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0));
            } catch (Exception ignored) {
                // Some providers do not grant persistent permissions; copying avoids depending on them.
            }
            if (requestCode == PICK_TREE) result = copyTree(uri);
            else result = copyDocument(uri);
        }
        nativeDocumentPicked(result == null ? "" : result);
    }

    private File pickerRoot() throws Exception {
        File root = new File(getFilesDir(), "PipeCleanPicked");
        if (!root.exists() && !root.mkdirs()) throw new Exception("Can't create picker folder");
        return root;
    }

    private String safeName(String s) {
        if (s == null || s.trim().isEmpty()) return "picked-file";
        s = s.replaceAll("[\\\\/:*?\"<>|]", "_").trim();
        return s.isEmpty() ? "picked-file" : s;
    }

    private String documentName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int col = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (col >= 0) return safeName(c.getString(col));
            }
        } catch (Exception ignored) {}
        return "picked-file";
    }

    private String copyDocument(Uri uri) {
        try {
            File dest = new File(pickerRoot(), System.currentTimeMillis() + "_" + documentName(uri));
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(dest)) {
                if (in == null) return "";
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
            }
            return dest.getAbsolutePath();
        } catch (Exception ex) {
            return "";
        }
    }

    private String copyTree(Uri treeUri) {
        try {
            String rootId = DocumentsContract.getTreeDocumentId(treeUri);
            Uri rootUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootId);
            String label = documentName(rootUri);
            File target = new File(pickerRoot(), System.currentTimeMillis() + "_" + label);
            if (!target.mkdirs()) return "";
            if (!copyTreeChildren(treeUri, rootId, target)) return "";
            return target.getAbsolutePath();
        } catch (Exception ex) {
            return "";
        }
    }

    private boolean copyTreeChildren(Uri treeUri, String parentId, File target) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId);
        String[] columns = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                             DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                             DocumentsContract.Document.COLUMN_MIME_TYPE };
        try (Cursor c = getContentResolver().query(children, columns, null, null, null)) {
            if (c == null) return false;
            while (c.moveToNext()) {
                String id = c.getString(0);
                String name = safeName(c.getString(1));
                String mime = c.getString(2);
                Uri child = DocumentsContract.buildDocumentUriUsingTree(treeUri, id);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    File dir = new File(target, name);
                    if (!dir.mkdirs() && !dir.isDirectory()) return false;
                    if (!copyTreeChildren(treeUri, id, dir)) return false;
                } else {
                    File dest = new File(target, name);
                    try (InputStream in = getContentResolver().openInputStream(child);
                         OutputStream out = new FileOutputStream(dest)) {
                        if (in == null) return false;
                        byte[] buffer = new byte[64 * 1024];
                        int count;
                        while ((count = in.read(buffer)) >= 0) out.write(buffer, 0, count);
                    }
                }
            }
            return true;
        } catch (Exception ex) {
            return false;
        }
    }
}
