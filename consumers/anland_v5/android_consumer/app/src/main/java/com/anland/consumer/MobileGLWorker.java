package com.anland.consumer;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.system.Os;
import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The unified renderer host. This private, same-UID {@code :mobilegl} service
 * loads MobileGL and serves the single embedded endpoint that every Anland
 * window and every container client connects to. MobileGL serializes the
 * sessions inside the library; Anland does not fork a renderer per client.
 *
 * <p>It runs as a foreground service once {@link MobileGLDesktop#ensureStarted} has started it, so
 * the server - and with it the Linux desktop - outlives a hidden or closed Anland window; KWin
 * powers its output down while nobody looks. Its notification's "Stop desktop" ends the Plasma
 * session, stops the container and then this service and its process.
 */
public final class MobileGLWorker extends Service {
    static final String INTERFACE = "com.anland.consumer.MobileGLWorker";
    static final int ATTACH = IBinder.FIRST_CALL_TRANSACTION;
    static final int DETACH = ATTACH + 1;
    public static final String ENDPOINT = "@anland-mobilegl";
    private static final String TAG = "AnlandMobileGL";
    static final String ACTION_START = "com.anland.consumer.mobilegl.START";
    static final String ACTION_STOP = "com.anland.consumer.mobilegl.STOP";
    private static final String CHANNEL_ID = "mobilegl_desktop";
    private static final int NOTIFICATION_ID = 0x4d474c;
    // Script runs are serialized; an "up" asked for while one is queued adds nothing.
    private final ExecutorService desktopThread = Executors.newSingleThreadExecutor();
    private final AtomicBoolean upQueued = new AtomicBoolean();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private volatile MobileGLDesktop.Target desktopTarget;
    private volatile boolean stopping;
    // The backend this process serves with, read once before the library loads.
    private String backend = DEFAULT_BACKEND;
    private Thread serverThread;
    private RuntimeException startupError;
    private volatile IBinder geometryCallback;
    private Surface attachedSurface;
    private long surfaceGeneration, surfaceIdentity;

    static {
        System.loadLibrary("anland_mobilegl");
    }

    // The MobileGL backend this process serves with: "DirectGLES" (the default) or
    // "DirectVulkan". Chosen by the activity's `mobilegl_backend` launch extra and kept in
    // a file, because this service runs in its own process and reads it once, before the
    // library loads; the choice takes effect the next time the :mobilegl process starts.
    static final String BACKEND_FILE = "mobilegl-backend";
    static final String DEFAULT_BACKEND = "DirectGLES";

    static boolean isKnownBackend(String backend) {
        return "DirectGLES".equals(backend) || "DirectVulkan".equals(backend);
    }

    static void saveBackend(Context context, String backend) {
        if (!isKnownBackend(backend)) {
            Log.w(TAG, "Ignoring unknown MobileGL backend '" + backend + "'");
            return;
        }
        try {
            Files.write(new File(context.getFilesDir(), BACKEND_FILE).toPath(),
                    backend.getBytes(StandardCharsets.UTF_8));
        } catch (IOException error) {
            Log.e(TAG, "Could not save the MobileGL backend choice", error);
        }
    }

    static String loadBackend(Context context) {
        try {
            String backend = new String(Files.readAllBytes(
                    new File(context.getFilesDir(), BACKEND_FILE).toPath()), StandardCharsets.UTF_8).trim();
            if (isKnownBackend(backend)) return backend;
        } catch (IOException ignored) {
            // No choice saved yet.
        }
        return DEFAULT_BACKEND;
    }

    @Override public void onCreate() {
        super.onCreate();
        try {
            // Configure before dlopen: MobileGL chooses its process role at load.
            Os.setenv("MOBILEGL_IPC_DIAL", "no", true);
            Os.setenv("MOBILEGL_IPC_ROLE", "server", true);
            backend = loadBackend(this);
            Log.i(TAG, "MobileGL backend: " + backend);
            Os.setenv("MOBILEGL_BACKEND_TYPE", backend, true);
            Os.setenv("MOBILEGL_LOG_FILE_PATH", getFilesDir() + "/mobilegl-server.log", true);
            Os.unsetenv("MOBILEGL_TRANSPORT");
            Os.unsetenv("MOBILEGL_IPC_SERVER_PATH");
            Os.unsetenv("MOBILEGL_IPC_RING_MB");
            Os.unsetenv("MOBILEGL_IPC_STAGE_MB");
            nativeLoad(getApplicationInfo().nativeLibraryDir + "/libMobileGL.so");
            nativeInstallDisplay();
        } catch (Exception error) {
            startupError = new IllegalStateException("Embedded MobileGL could not start", error);
            Log.e(TAG, "Embedded compositor initialization failed", error);
        }
    }

    private final Binder binder = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == INTERFACE_TRANSACTION) {
                reply.writeString(INTERFACE);
                return true;
            }
            if (code != ATTACH && code != DETACH)
                return super.onTransact(code, data, reply, flags);
            data.enforceInterface(INTERFACE);
            if (startupError != null) {
                reply.writeException(startupError);
                return true;
            }
            if (code == ATTACH) {
                Surface surface = Surface.CREATOR.createFromParcel(data);
                int width = data.readInt();
                int height = data.readInt();
                long generation = data.readLong();
                long identity = data.readLong();
                IBinder owner = data.readStrongBinder();
                boolean retained = false;
                try {
                    synchronized (MobileGLWorker.this) {
                        // Parcel unmarshalling creates a new android::Surface
                        // wrapper even for the same buffer queue. Retain one
                        // wrapper per generation so resize updates its existing
                        // display lease instead of detaching a live GL session.
                        geometryCallback = owner;
                        if (attachedSurface != null && surfaceGeneration == generation &&
                                surfaceIdentity == identity &&
                                owner.equals(attachedOwner)) {
                            nativeAttachWindow(attachedSurface, width, height);
                        } else {
                            nativeAttachWindow(surface, width, height);
                            if (attachedSurface != null) attachedSurface.release();
                            attachedSurface = surface;
                            attachedOwner = owner;
                            surfaceGeneration = generation;
                            surfaceIdentity = identity;
                            retained = true;
                        }
                        // One server per process. The library multiplexes the
                        // sessions of every client that dials this endpoint.
                        if (serverThread == null || !serverThread.isAlive()) {
                            serverThread = new Thread(() -> {
                                int result = nativeServe(ENDPOINT);
                                Log.i(TAG, "Embedded server stopped, result=" + result);
                            }, "anland-mgl-server");
                            serverThread.start();
                        }
                    }
                    reply.writeNoException();
                } catch (RuntimeException error) {
                    reply.writeException(error);
                } finally {
                    if (!retained) surface.release();
                }
            } else {
                int result = detachSurface();
                Log.i(TAG, "Anland Surface detached, result=" + result);
                reply.writeNoException();
            }
            return true;
        }
    };

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopDesktop();
            return START_NOT_STICKY;
        }
        goForeground();
        desktopTarget = MobileGLDesktop.Target.from(intent, desktopTarget);
        if (!stopping && upQueued.compareAndSet(false, true)) {
            desktopThread.execute(() -> {
                upQueued.set(false);
                if (!stopping)
                    MobileGLDesktop.run(this, DesktopCommand.Verb.UP, desktopTarget, backend);
            });
        }
        // A process the system killed has lost its sessions with it; the next window starts over.
        return START_NOT_STICKY;
    }

    private void goForeground() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                    getString(R.string.mobilegl_desktop_channel), NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(channel);
        }
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, MobileGLWorker.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle(getString(R.string.mobilegl_desktop_running))
                .setContentText(getString(R.string.mobilegl_desktop_running_text, backend))
                .setContentIntent(open)
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null,
                        getString(R.string.mobilegl_desktop_stop), stop).build())
                .build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIFICATION_ID, notification);
    }

    // Ends the Plasma session and the container, closes the Anland windows, and lets this service
    // (and, in onDestroy, its process) go: the next window starts everything from scratch.
    private void stopDesktop() {
        if (stopping) return;
        stopping = true;
        Log.i(TAG, "Stop desktop requested");
        desktopThread.execute(() -> {
            MobileGLDesktop.run(this, DesktopCommand.Verb.DOWN, desktopTarget, backend);
            mainHandler.post(() -> {
                ActivityManager am = getSystemService(ActivityManager.class);
                if (am != null) {
                    for (ActivityManager.AppTask task : am.getAppTasks()) task.finishAndRemoveTask();
                }
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf();
            });
        });
    }

    @Override public IBinder onBind(Intent intent) { return binder; }

    private IBinder attachedOwner;

    private synchronized int detachSurface() {
        geometryCallback = null;
        int result = nativeDetachWindow();
        if (attachedSurface != null) attachedSurface.release();
        attachedSurface = null;
        attachedOwner = null;
        return result;
    }

    // Called on MobileGL's apply thread. The one-way callback posts geometry to
    // Anland's UI thread; SurfaceHolder callbacks report the resulting extent.
    private void requestGeometry(int width, int height) {
        IBinder callback = geometryCallback;
        if (callback == null) return;
        Parcel data = Parcel.obtain();
        try {
            data.writeInt(width); data.writeInt(height);
            callback.transact(IBinder.FIRST_CALL_TRANSACTION, data, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException error) {
            Log.w(TAG, "Anland window no longer accepts geometry", error);
        } finally {
            data.recycle();
        }
    }

    @Override public void onDestroy() {
        if (startupError == null) {
            nativeStop();
            // The serve loop returns asynchronously after nativeStop; uninstalling
            // the display while it is still up would pull the runtime from under
            // the apply thread. Join before teardown.
            Thread thread = serverThread;
            if (thread != null) {
                try {
                    thread.join(5000);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                }
                if (thread.isAlive()) Log.w(TAG, "Embedded server did not stop within 5s");
            }
            detachSurface();
            nativeUninstallDisplay();
        }
        desktopThread.shutdown();
        super.onDestroy();
        // After Stop desktop nothing of the renderer may linger: its process-global GL state goes
        // with the process, and the next start loads it fresh.
        if (stopping) Process.killProcess(Process.myPid());
    }

    private static native void nativeLoad(String path);
    private native void nativeInstallDisplay();
    private static native void nativeAttachWindow(Surface surface, int width, int height);
    private static native int nativeDetachWindow();
    private static native void nativeUninstallDisplay();
    private static native int nativeServe(String endpoint);
    private static native void nativeStop();
}
