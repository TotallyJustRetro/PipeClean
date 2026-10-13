package com.pipeclean.app;

import android.app.Activity;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/** SDL host activity with a Storage Access Framework bridge for native file dialogs. */
public final class PipeCleanActivity extends SDLActivity {
    private static final int PICK_DOCUMENT = 0x5043;
    private static final int PICK_TREE = 0x5044;
    private static final int PICK_COMPANION_BACKGROUND = 0x5045;
    private static final String COMPANION_PREFS = "PipeCleanCompanion";
    private static final String COMPANION_BACKGROUND_KEY = "background_path";
    private static volatile int pendingKind;
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
                if (!isFinishing() && companionPresentation == created && !created.isShowing())
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
                                     int item, int dualScreenEnabled) {
        runOnUiThread(() -> {
            companionEnabled = dualScreenEnabled != 0 && (game == 1 || game == 2);
            // Always retain the second-screen deck. Game-specific widgets are live only
            // when the selected game has Companion support enabled; otherwise show a
            // useful READY screen rather than dismissing the entire lower display.
            showCompanionDisplay();
            if (companionPresentation != null) {
                if (companionEnabled) {
                    companionPresentation.updateState(game, lives, stage, world,
                        paused != 0, slot, palette, filter, item);
                    companionPresentation.setPipeSwapMode(companionPipeSwapMode);
                } else {
                    companionPresentation.showIdleState();
                    companionPresentation.setPipeSwapMode(false);
                }
            }
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

    /** Called when gameplay exits so the second screen returns to the system display. */
    public void setCompanionEnabled(boolean enabled) {
        runOnUiThread(() -> {
            companionEnabled = enabled;
            showCompanionDisplay();
            if (!companionEnabled && companionPresentation != null) {
                companionPresentation.showIdleState();
                companionPresentation.setPipeSwapMode(false);
            } else if (companionPresentation != null) {
                companionPresentation.setPipeSwapMode(companionPipeSwapMode);
            }
        });
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
