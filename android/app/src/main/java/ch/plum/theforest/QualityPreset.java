package ch.plum.theforest;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Seeds or applies a crisp mobile graphics preset into the runtime's registry.dat
 * (Unity PlayerPrefs under HKCU\Software\SKS\TheForest) while keeping full-resolution
 * textures and FXAA enabled, and preserving all other keys (controls, audio, language, saves).
 */
final class QualityPreset {
    private static final String TAG = "TheForest";
    private static final String KEY = "hkcu\\software\\sks\\theforest";
    private static final String PREF_MIGRATED_V020 = "mobile_preset_v020_applied";
    private static final String SAVED_NAME = "Quality_v016_Saved_h1580554892";
    private static final int REG_DWORD = 4;

    private static final class Entry {
        final String key;
        final String name;
        final int type;
        final byte[] data;

        Entry(String key, String name, int type, byte[] data) {
            this.key = key;
            this.name = name;
            this.type = type;
            this.data = data;
        }
    }

    private static byte[] leInt(int v) {
        return ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array();
    }

    private static byte[] leDouble(double v) {
        return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putDouble(v).array();
    }

    private static String unityPrefName(String base) {
        long h = 5381L;
        for (int i = 0; i < base.length(); i++) {
            h = ((h * 33L) ^ (base.charAt(i) & 0xFFL)) & 0xFFFFFFFFL;
        }
        return base + "_h" + h;
    }

    private static Map<String, Entry> buildMobileEntries() {
        Map<String, Entry> m = new LinkedHashMap<>();
        putInt(m, "UnityGraphicsQuality", 4);
        putInt(m, "Quality_v016_Saved", 1);
        putInt(m, "Quality_v016_Version", 35);
        putInt(m, "Quality_v016_Preset", 5);                     // Custom
        putInt(m, "Quality_v016_TextureQuality", 0);             // FullRes (sharp textures)
        putInt(m, "Quality_v016_MaterialQuality", 1);            // Medium
        putInt(m, "Quality_v016_AntiAliasing", 1);               // FXAA
        putInt(m, "Quality_v016_ShadowLevel", 5);                // UltraLow
        putInt(m, "Quality_v016_CascadeCount", 5);               // NoCascades_laptop
        putInt(m, "Quality_v016_FarShadowMode", 1);              // Off
        putInt(m, "Quality_v016_DrawDistance", 5);               // UltraLow
        putInt(m, "Quality_v016_GrassMode", 0);                  // CPU
        putDouble(m, "Quality_v016_GrassDistance", 30.0);
        putDouble(m, "Quality_v016_GrassDensity", 0.15);
        putInt(m, "Quality_v016_SSAOType", 0);
        putInt(m, "Quality_v016_SSAO", 3);                       // Off
        putInt(m, "Quality_v016_SEBloom", 1);                    // None
        putInt(m, "Quality_v016_CA", 0);                         // Off
        putInt(m, "Quality_v016_Fg", 0);                         // Off
        putInt(m, "Quality_v016_DofTech", 1);                    // Off
        putInt(m, "Quality_v016_ScreenSpaceReflection", 1);      // Off
        putInt(m, "Quality_v016_VolumetricClouds", 1);           // Off
        putInt(m, "Quality_v016_SunshineOcclusion", 1);          // Off
        putInt(m, "Quality_v016_VolumetricsType", 0);
        putInt(m, "Quality_v016_PostEffectsSystem", 1);
        putInt(m, "Quality_v016_Caustics", 1);                   // Off
        putInt(m, "Quality_v016_LightmapResolution", 512);
        putDouble(m, "Quality_v016_LightDistance", 40.0);
        putInt(m, "Quality_v016_LightmapUpdateIntervalFrames", 4);
        putInt(m, "Quality_v016_ScatterResolution", 3);
        putInt(m, "Quality_v016_ScatterSamplingQuality", 0);
        putInt(m, "Quality_v016_TerrainQuality", 2);             // Simple
        putInt(m, "Quality_v016_ReflexionMode", 2);              // Off
        putInt(m, "Quality_v016_OceanQuality", 2);               // Flat
        putInt(m, "LowQualityPhysics", 1);                       // Low CPU physics
        return m;
    }

    private static void putInt(Map<String, Entry> m, String base, int v) {
        String name = unityPrefName(base);
        m.put(name.toLowerCase(Locale.ROOT), new Entry(KEY, name, REG_DWORD, leInt(v)));
    }

    private static void putDouble(Map<String, Entry> m, String base, double v) {
        String name = unityPrefName(base);
        m.put(name.toLowerCase(Locale.ROOT), new Entry(KEY, name, REG_DWORD, leDouble(v)));
    }

    /**
     * Ensures the crisp mobile preset is applied on first install or once on upgrade to 0.2.0.
     * Any subsequent changes made by the player in OPTIONS -> GRAPHICS are preserved.
     */
    static void ensureDefault(Context ctx, File userDir) {
        File reg = new File(userDir, "registry.dat");
        SharedPreferences prefs = ctx.getSharedPreferences(Launcher.PREFS, Context.MODE_PRIVATE);
        boolean migrated = prefs.getBoolean(PREF_MIGRATED_V020, false);
        List<Entry> existing = readRegistry(reg);
        boolean hasSaved = false;
        for (Entry e : existing) {
            if (KEY.equals(e.key) && SAVED_NAME.equalsIgnoreCase(e.name)) {
                hasSaved = true;
                break;
            }
        }
        if (hasSaved && migrated) return;
        if (applyMobilePreset(userDir, existing)) {
            prefs.edit().putBoolean(PREF_MIGRATED_V020, true).apply();
            Log.i(TAG, "Applied crisp mobile graphics preset (hasSaved=" + hasSaved + ", migrated=" + migrated + ")");
        }
    }

    /** Explicitly applies the crisp mobile graphics preset, keeping all non-graphics registry keys. */
    static boolean applyMobilePreset(File userDir) {
        return applyMobilePreset(userDir, readRegistry(new File(userDir, "registry.dat")));
    }

    private static boolean applyMobilePreset(File userDir, List<Entry> existing) {
        try {
            userDir.mkdirs();
            Map<String, Entry> preset = buildMobileEntries();
            List<Entry> merged = new ArrayList<>();
            for (Entry e : existing) {
                if (KEY.equals(e.key) && preset.containsKey(e.name.toLowerCase(Locale.ROOT))) continue;
                merged.add(e);
            }
            merged.addAll(preset.values());
            writeRegistry(new File(userDir, "registry.dat"), merged);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "applyMobilePreset failed: " + e.getMessage());
            return false;
        }
    }

    private static List<Entry> readRegistry(File file) {
        List<Entry> out = new ArrayList<>();
        if (!file.isFile() || file.length() <= 0) return out;
        try (FileInputStream fis = new FileInputStream(file)) {
            ByteArrayOutputStream raw = new ByteArrayOutputStream((int) Math.min(file.length(), 1 << 20));
            byte[] buf = new byte[8192];
            int k;
            while ((k = fis.read(buf)) > 0) raw.write(buf, 0, k);
            ByteBuffer bb = ByteBuffer.wrap(raw.toByteArray()).order(ByteOrder.LITTLE_ENDIAN);
            while (bb.remaining() >= 4) {
                int kl = bb.getInt();
                if (kl < 0 || kl > 4096 || bb.remaining() < kl + 4) break;
                byte[] kb = new byte[kl];
                bb.get(kb);
                int nl = bb.getInt();
                if (nl < 0 || nl > 4096 || bb.remaining() < nl + 8) break;
                byte[] nb = new byte[nl];
                bb.get(nb);
                int ty = bb.getInt();
                int sz = bb.getInt();
                if (sz < 0 || sz > (64 << 20) || bb.remaining() < sz) break;
                byte[] db = new byte[sz];
                bb.get(db);
                out.add(new Entry(
                        new String(kb, StandardCharsets.UTF_8),
                        new String(nb, StandardCharsets.UTF_8),
                        ty, db));
            }
        } catch (Exception e) {
            Log.w(TAG, "readRegistry failed: " + e.getMessage());
        }
        return out;
    }

    private static void writeRegistry(File file, List<Entry> entries) throws Exception {
        ByteArrayOutputStream bos = new ByteArrayOutputStream(4096);
        ByteBuffer hdr = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        for (Entry e : entries) {
            byte[] kb = e.key.getBytes(StandardCharsets.UTF_8);
            byte[] nb = e.name.getBytes(StandardCharsets.UTF_8);
            hdr.clear();
            hdr.putInt(kb.length);
            bos.write(hdr.array(), 0, 4);
            bos.write(kb);
            hdr.clear();
            hdr.putInt(nb.length);
            bos.write(hdr.array(), 0, 4);
            bos.write(nb);
            hdr.clear();
            hdr.putInt(e.type);
            hdr.putInt(e.data.length);
            bos.write(hdr.array(), 0, 8);
            bos.write(e.data);
        }
        File tmp = new File(file.getParentFile(), file.getName() + ".tmp");
        try (FileOutputStream fos = new FileOutputStream(tmp)) {
            fos.write(bos.toByteArray());
            fos.getFD().sync();
        }
        if (!tmp.renameTo(file)) {
            try (FileOutputStream fos = new FileOutputStream(file)) {
                fos.write(bos.toByteArray());
            }
            tmp.delete();
        }
    }
}
