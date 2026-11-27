package ch.plum.theforest;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/** Start screen: where the game data is, importing it, and starting the game. */
public class Launcher extends Activity {
    private static final int PICK_FOLDER = 1, PICK_PACK = 2;
    static final String PREFS = "theforest";
    static final String PREF_FULL_RES = "full_res";
    private TextView status;
    private ProgressBar bar;
    private Button play, importBtn, packBtn;
    private android.os.ParcelFileDescriptor packFd;
    private GameImporter importer;
    private final Handler ui = new Handler(Looper.getMainLooper());

    static File importDir(Activity a) { return new File(a.getExternalFilesDir(null), "game"); }
    static File obbDir(Activity a) { return a.getObbDir(); }
    static File gameDir(Activity a) {
        File imp = importDir(a);
        if (GameImporter.installed(imp)) return imp;
        File obb = obbDir(a);
        if (obb != null && GameImporter.installed(obb)) return obb;
        return imp;
    }
    static File userDir(Activity a) { return new File(a.getExternalFilesDir(null), "user"); }

    /** Extract bundled open-source Mono 4.8 BCL DLLs into userDir/mono_bcl (refreshed on APK update). */
    static void ensureMonoBcl(Context ctx, File userDir) {
        File dst = new File(userDir, "mono_bcl");
        dst.mkdirs();
        try {
            long apkTs = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).lastUpdateTime;
            File stamp = new File(dst, ".apk_ts_" + apkTs);
            boolean refresh = !stamp.isFile();
            AssetManager am = ctx.getAssets();
            String[] names = am.list("mono_bcl");
            if (names == null) return;
            byte[] buf = new byte[65536];
            for (String n : names) {
                File out = new File(dst, n);
                if (!refresh && out.isFile() && out.length() > 0) continue;
                try (InputStream in = am.open("mono_bcl/" + n);
                     OutputStream os = new FileOutputStream(out)) {
                    int k;
                    while ((k = in.read(buf)) > 0) os.write(buf, 0, k);
                }
            }
            if (refresh) {
                File[] old = dst.listFiles();
                if (old != null) for (File f : old) if (f.getName().startsWith(".apk_ts_")) f.delete();
                try (FileOutputStream os = new FileOutputStream(stamp)) { os.write('1'); }
            }
        } catch (Exception e) {
            Log.e("TheForest", "ensureMonoBcl failed: " + e.getMessage());
        }
    }

    private int dp(int v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (!isTaskRoot() && Intent.ACTION_MAIN.equals(getIntent().getAction())
                && getIntent().hasCategory(Intent.CATEGORY_LAUNCHER)) {
            finish();
            return;
        }
        importDir(this).mkdirs();
        userDir(this).mkdirs();
        ensureMonoBcl(this, userDir(this));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(32), dp(28), dp(32), dp(28));
        col.setBackgroundColor(Color.rgb(14, 22, 18));

        TextView title = new TextView(this);
        title.setText("THE FOREST");
        title.setTextColor(Color.rgb(225, 232, 220));
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 34);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);
        TextView sub = new TextView(this);
        sub.setText("Statische Rekompilierung für Android");
        sub.setTextColor(Color.rgb(155, 180, 160));
        sub.setPadding(0, 0, 0, dp(20));
        col.addView(sub);

        status = new TextView(this);
        status.setTextColor(Color.rgb(230, 235, 230));
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        status.setPadding(0, 0, 0, dp(12));
        col.addView(status);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        col.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(14)));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(12), 0, 0);
        play = new Button(this);
        play.setText("Spielen");
        play.setOnClickListener(v -> startGame());
        importBtn = new Button(this);
        importBtn.setText("Ordner wählen");
        importBtn.setOnClickListener(v -> pickFolder());
        packBtn = new Button(this);
        packBtn.setText("Datenpaket (ZIP) wählen");
        packBtn.setOnClickListener(v -> pickPack());
        LinearLayout.LayoutParams wp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        row.addView(play, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.8f));
        row.addView(importBtn, wp);
        row.addView(packBtn, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f));
        col.addView(row);

        CheckBox fullRes = new CheckBox(this);
        fullRes.setText("Volle Bildschirmauflösung (schärfer, aber langsamer; sonst max. " + ForestActivity.RENDER_MAX + " px Breite)");
        fullRes.setTextColor(Color.rgb(225, 232, 225));
        fullRes.setPadding(dp(4), dp(12), 0, 0);
        fullRes.setChecked(getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_FULL_RES, false));
        fullRes.setOnCheckedChangeListener((v, on) ->
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_FULL_RES, on).apply());
        col.addView(fullRes);

        TextView help = new TextView(this);
        help.setTextColor(Color.rgb(145, 165, 150));
        help.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        help.setPadding(0, dp(20), 0, 0);
        help.setText("Benötigt wird der Ordner deiner eigenen The-Forest-PC-Installation "
                + "(mit TheForest.exe und TheForest_Data; ca. 1,9 GB). "
                + "Entweder über „Ordner wählen“ importieren, per USB nach\n"
                + importDir(this).getAbsolutePath() + "\nkopieren, oder das Datenpaket in Plum Hub laden: es landet als ZIP in\n"
                + (obbDir(this) != null ? obbDir(this).getAbsolutePath() : "Android/obb/ch.plum.theforest")
                + " und wird beim nächsten Öffnen automatisch entpackt."
                + "\n\nSpielstände und Einstellungen: " + userDir(this).getAbsolutePath());
        col.addView(help);

        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setBackgroundColor(Color.rgb(14, 22, 18));
        sv.addView(col);
        setContentView(sv);
        refresh();
    }

    @Override protected void onResume() {
        super.onResume();
        if (play != null && importer == null) refresh();
    }

    private void refresh() {
        boolean ok = GameImporter.installed(gameDir(this));
        File pack = ok ? null : GameImporter.findPack(obbDir(this), GameImporter.HUB_FALLBACK);
        if (pack != null && importer == null) { unpack(pack, pack::delete); return; }
        play.setEnabled(ok);
        if (ok) {
            status.setText(String.format(Locale.ROOT, "Spieldaten gefunden (%s).",
                    gameDir(this).equals(importDir(this)) ? "importiert" : "von Plum Hub"));
        } else {
            status.setText("Noch keine Spieldaten. Bitte importieren.");
        }
    }

    private void unpack(File pack, Runnable afterwards) {
        play.setEnabled(false);
        importBtn.setEnabled(false);
        packBtn.setEnabled(false);
        bar.setVisibility(View.VISIBLE);
        importer = new GameImporter(getContentResolver(), obbDir(this), (file, done, total, bps) -> ui.post(() -> {
            bar.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
            status.setText(String.format(Locale.ROOT, "Entpacke Datenpaket: %s … %.2f / %.2f GB%s", file, done / 1e9, total / 1e9,
                    bps > 0 ? String.format(Locale.ROOT, " (%.0f MB/s)", bps / 1e6) : ""));
        }));
        final GameImporter imp = importer;
        final android.os.ParcelFileDescriptor pfd = packFd;
        new Thread(() -> {
            GameImporter.Result r;
            if (pfd != null) {
                try (java.io.FileInputStream fis = new java.io.FileInputStream(pfd.getFileDescriptor())) {
                    r = imp.extractPack(fis.getChannel(), afterwards);
                } catch (Exception e) {
                    r = new GameImporter.Result(false, "Entpacken fehlgeschlagen: " + e.getMessage());
                }
            } else {
                r = imp.extractPack(pack, afterwards);
            }
            final GameImporter.Result res = r;
            ui.post(() -> {
                importer = null;
                if (packFd != null) { try { packFd.close(); } catch (Exception ignored) { } packFd = null; }
                bar.setVisibility(View.GONE);
                importBtn.setEnabled(true);
                packBtn.setEnabled(true);
                boolean ok = GameImporter.installed(gameDir(this));
                play.setEnabled(ok);
                status.setText(ok ? res.message + " Bereit zum Spielen." : res.message);
            });
        }, "unpack").start();
    }

    private void startGame() {
        startActivity(new Intent(this, ForestActivity.class));
    }

    private void pickPack() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed", "application/octet-stream" });
        startActivityForResult(i, PICK_PACK);
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, PICK_FOLDER);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == PICK_PACK && res == RESULT_OK && data != null && data.getData() != null) {
            Uri doc = data.getData();
            try {
                packFd = getContentResolver().openFileDescriptor(doc, "r");
                if (packFd == null) { status.setText("Datenpaket nicht lesbar."); return; }
                unpack(null, () -> {
                    try { android.provider.DocumentsContract.deleteDocument(getContentResolver(), doc); } catch (Exception ignored) { }
                });
            } catch (Exception e) {
                status.setText("Datenpaket nicht lesbar: " + e.getMessage());
            }
            return;
        }
        if (req != PICK_FOLDER || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        play.setEnabled(false);
        importBtn.setEnabled(false);
        bar.setVisibility(View.VISIBLE);
        importer = new GameImporter(getContentResolver(), importDir(this), (file, done, total, bps) -> ui.post(() -> {
            bar.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
            status.setText(String.format(Locale.ROOT, "Kopiere %s … %.2f / %.2f GB%s", file, done / 1e9, total / 1e9,
                    bps > 0 ? String.format(Locale.ROOT, " (%.0f MB/s)", bps / 1e6) : ""));
        }));
        new Thread(() -> {
            GameImporter.Result r = importer.importTree(tree);
            ui.post(() -> {
                importer = null;
                bar.setVisibility(View.GONE);
                importBtn.setEnabled(true);
                refresh();
                if (!r.ok || !GameImporter.installed(gameDir(this))) status.setText(r.message);
                else status.setText(r.message + " Bereit zum Spielen.");
            });
        }, "import").start();
    }
}
