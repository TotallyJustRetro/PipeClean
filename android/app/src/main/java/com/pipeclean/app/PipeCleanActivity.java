package com.pipeclean.app;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/** SDL host activity with a Storage Access Framework bridge for native file dialogs. */
public final class PipeCleanActivity extends SDLActivity {
    private static final int PICK_DOCUMENT = 0x5043;
    private static final int PICK_TREE = 0x5044;
    private static volatile int pendingKind;

    public static native void nativeDocumentPicked(String path);

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
