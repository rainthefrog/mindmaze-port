package io.github.mindmazeport;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import androidx.core.content.FileProvider;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;

/* MindMaze built-in updater, Android part (CHANGE 2026-11-20; desktop: src/platform/common/update.c). shell.js asks
 * GitHub for the newest release and, when the player agrees, calls Updater.install({url, sumUrl}): the APK and its
 * .sha256 are downloaded into the app's cache, the checksum compared, and Android's own install screen opened
 * (a sideloaded app cannot install itself without that screen). Android installs it over this app only when it is
 * signed with the same key (the release workflow checks that). The imported data stays.
 * If installing from this app is not allowed yet, the matching system settings page opens and install() fails with
 * "permission"; the player allows it and taps Update again. Progress: "progress" events {pct}. */
@CapacitorPlugin(name = "Updater")
public class UpdaterPlugin extends Plugin {

    @PluginMethod
    public void canInstall(PluginCall call) {
        JSObject r = new JSObject();
        r.put("allowed", Build.VERSION.SDK_INT < Build.VERSION_CODES.O || getContext().getPackageManager().canRequestPackageInstalls());
        call.resolve(r);
    }

    @PluginMethod
    public void install(PluginCall call) {
        String url = call.getString("url"), sumUrl = call.getString("sumUrl");
        if (url == null || sumUrl == null || !url.startsWith("https://") || !sumUrl.startsWith("https://")) {
            call.reject("bad link");
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && !getContext().getPackageManager().canRequestPackageInstalls()) {
            Intent s = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getContext().getPackageName()));
            s.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            getContext().startActivity(s);
            call.reject("permission");
            return;
        }
        new Thread(() -> {
            File dir = new File(getContext().getCacheDir(), "update");
            File apk = new File(dir, "MindMaze.apk");
            try {
                dir.mkdirs();
                String sum = new String(get(sumUrl, null, false), "UTF-8").trim();
                if (sum.length() < 64) throw new Exception("no checksum");
                MessageDigest md = MessageDigest.getInstance("SHA-256");
                try (OutputStream out = new FileOutputStream(apk)) { get(url, new Object[] {out, md}, true); }
                StringBuilder hex = new StringBuilder();
                for (byte b : md.digest()) hex.append(String.format(Locale.ROOT, "%02x", b));
                if (!sum.substring(0, 64).toLowerCase(Locale.ROOT).equals(hex.toString())) {
                    apk.delete();
                    throw new Exception("the download is damaged (checksum differs)");
                }
                Uri uri = FileProvider.getUriForFile(getContext(), getContext().getPackageName() + ".fileprovider", apk);
                Intent i = new Intent(Intent.ACTION_VIEW);
                i.setDataAndType(uri, "application/vnd.android.package-archive");
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                getContext().startActivity(i);
                call.resolve();
            } catch (Exception e) {
                call.reject(e.getMessage() != null ? e.getMessage() : "the download failed");
            }
        }).start();
    }

    /* GET (redirects followed, also https -> other https hosts as GitHub's download links do). sink null: returns the
     * bytes; else {OutputStream, MessageDigest}: streams into both and reports progress */
    private byte[] get(String link, Object[] sink, boolean progress) throws Exception {
        for (int hop = 0; hop < 6; hop++) {
            HttpURLConnection c = (HttpURLConnection) new URL(link).openConnection();
            c.setInstanceFollowRedirects(false);
            c.setConnectTimeout(20000);
            c.setReadTimeout(30000);
            c.setRequestProperty("User-Agent", "MindMaze-updater");
            int code = c.getResponseCode();
            if (code >= 300 && code < 400) {
                String next = c.getHeaderField("Location");
                c.disconnect();
                if (next == null || !next.startsWith("https://")) throw new Exception("bad redirect");
                link = next;
                continue;
            }
            if (code != 200) { c.disconnect(); throw new Exception("HTTP " + code); }
            long total = c.getContentLengthLong(), got = 0;
            int last = -1;
            ByteArrayOutputStream mem = sink == null ? new ByteArrayOutputStream() : null;
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) {
                    if (mem != null) mem.write(buf, 0, n);
                    else { ((OutputStream) sink[0]).write(buf, 0, n); ((MessageDigest) sink[1]).update(buf, 0, n); }
                    got += n;
                    int pct = total > 0 ? (int) (got * 100 / total) : -1;
                    if (progress && pct != last) {
                        last = pct;
                        JSObject p = new JSObject();
                        p.put("pct", pct);
                        notifyListeners("progress", p);
                    }
                }
            } finally {
                c.disconnect();
            }
            if (total > 0 && got != total) throw new Exception("the download stopped");
            return mem != null ? mem.toByteArray() : null;
        }
        throw new Exception("too many redirects");
    }
}
