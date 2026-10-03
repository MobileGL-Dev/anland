package com.anland.consumer;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Where the MobileGL desktop gets started, and what starting it means.
 *
 * <p>{@link #ensureStarted} is the one trigger: today the Anland window calls it whenever it is
 * opened. It starts {@link MobileGLWorker} as a foreground service, which keeps the embedded
 * MobileGL server alive while the window is hidden or closed and runs {@link DesktopCommand.Verb#UP}
 * (display daemon, container, Plasma session) as root. Everything is idempotent, so a reopen with
 * the desktop already up only attaches the window. Starting the desktop at another moment (phone
 * boot, container start) means calling this from there.
 */
final class MobileGLDesktop {
    private static final String TAG = "AnlandMobileGL";
    static final String PREFS_NAME = "anland_settings";
    static final String KEY_CONTAINER = "mobilegl_container";
    static final String KEY_DAEMON = "mobilegl_display_daemon";
    static final String EXTRA_SOCKET = "com.anland.consumer.mobilegl.SOCKET";
    static final String EXTRA_CONTAINER = "com.anland.consumer.mobilegl.CONTAINER";
    static final String EXTRA_DAEMON = "com.anland.consumer.mobilegl.DAEMON";
    private static final String SCRIPT = "mobilegl-desktop.sh";
    /** The container start alone can take tens of seconds on a cold boot. */
    private static final long TIMEOUT_MS = 180_000L;

    private MobileGLDesktop() {
    }

    /** Starts (or confirms) the whole desktop for a window that targets {@code socket}. */
    static void ensureStarted(Context context, String socket) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        Intent intent = new Intent(context, MobileGLWorker.class)
                .setAction(MobileGLWorker.ACTION_START)
                .putExtra(EXTRA_SOCKET, socket)
                .putExtra(EXTRA_CONTAINER, DesktopCommand.containerOrDefault(
                        prefs.getString(KEY_CONTAINER, DesktopCommand.DEFAULT_CONTAINER)))
                .putExtra(EXTRA_DAEMON, prefs.getString(KEY_DAEMON, DesktopCommand.DEFAULT_DAEMON));
        try {
            context.startForegroundService(intent);
        } catch (RuntimeException error) {
            Log.e(TAG, "Could not start the MobileGL desktop service", error);
        }
    }

    /** Settings a running service keeps for its later commands (Stop from the notification). */
    static final class Target {
        final String socket, container, daemon;

        Target(String socket, String container, String daemon) {
            this.socket = socket;
            this.container = container;
            this.daemon = daemon;
        }

        static Target from(Intent intent, Target previous) {
            if (intent == null || intent.getStringExtra(EXTRA_SOCKET) == null) return previous;
            return new Target(intent.getStringExtra(EXTRA_SOCKET), intent.getStringExtra(EXTRA_CONTAINER),
                    intent.getStringExtra(EXTRA_DAEMON));
        }
    }

    /** Runs one verb of the packaged script as root; blocking, call it off the main thread. */
    static boolean run(Context context, DesktopCommand.Verb verb, Target target, String backend) {
        if (target == null) {
            Log.w(TAG, "MobileGL desktop " + verb.word + ": no window has asked for a desktop yet");
            return false;
        }
        File script = new File(context.getFilesDir(), SCRIPT);
        try (InputStream in = context.getAssets().open(SCRIPT);
             OutputStream out = new FileOutputStream(script)) {
            // A Windows checkout may have given the script CRLF endings; sh would choke on them.
            byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) > 0; ) {
                for (int i = 0; i < n; i++)
                    if (buffer[i] != (byte) 13) out.write(buffer[i]);
            }
        } catch (IOException error) {
            Log.e(TAG, "Could not unpack " + SCRIPT, error);
            return false;
        }
        String command = DesktopCommand.build(script.getAbsolutePath(), verb, target.socket, backend,
                target.container, target.daemon, DesktopCommand.DEFAULT_DROIDSPACES);
        long start = System.nanoTime();
        SuCommand.CommandResult result = new SuCommand.SuRunner().run(command, TIMEOUT_MS);
        long ms = (System.nanoTime() - start) / 1_000_000L;
        String output = (result.stdout + result.stderr).trim();
        if (result.exitCode == 0 && !result.timedOut && !result.unavailable) {
            Log.i(TAG, "MobileGL desktop " + verb.word + " done in " + ms + " ms: " + output);
            return true;
        }
        Log.e(TAG, "MobileGL desktop " + verb.word + " failed after " + ms + " ms (exit " + result.exitCode
                + (result.timedOut ? ", timed out" : "") + (result.unavailable ? ", no su" : "") + "): " + output);
        return false;
    }
}
