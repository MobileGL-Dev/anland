package com.anland.consumer;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;

/** Keeps the packaged renderer service alive for an Anland window. */
final class MobileGLConnection {
    private final Context context;
    private IBinder worker;
    private Surface surface;
    private int width, height;
    private boolean bound;
    private Runnable ready;
    private long surfaceGeneration = 1;
    interface GeometryListener { void request(int width, int height); }
    private final GeometryListener geometryListener;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    MobileGLConnection(Context context, GeometryListener listener) {
        this.context = context;
        this.geometryListener = listener;
    }

    private final Binder geometryCallback = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
            int requestedWidth = data.readInt(), requestedHeight = data.readInt();
            mainHandler.post(() -> {
                if (surface != null) geometryListener.request(requestedWidth, requestedHeight);
            });
            return true;
        }
    };

    private final ServiceConnection workerConnection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            worker = service;
            if (surface != null && surface.isValid() && sendAttach() && ready != null) ready.run();
        }
        @Override public void onServiceDisconnected(ComponentName name) { worker = null; }
    };

    void attach(Surface surface, int width, int height, Runnable ready) {
        this.surface = surface;
        this.width = width;
        this.height = height;
        this.ready = ready;
        if (!bound)
            bound = context.bindService(new Intent(context, MobileGLWorker.class),
                    workerConnection, Context.BIND_AUTO_CREATE);
        if (worker != null && sendAttach()) ready.run();
    }

    void reportGeometry(Surface surface, int width, int height) {
        this.surface = surface;
        this.width = width;
        this.height = height;
        if (worker != null) sendAttach();
    }

    private boolean sendAttach() {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(MobileGLWorker.INTERFACE);
            surface.writeToParcel(data, 0);
            data.writeInt(width);
            data.writeInt(height);
            data.writeLong(surfaceGeneration);
            data.writeLong(Native.surfaceIdentity(surface));
            data.writeStrongBinder(geometryCallback);
            if (!worker.transact(MobileGLWorker.ATTACH, data, reply, 0))
                throw new IllegalStateException("MobileGL worker rejected Surface attachment");
            reply.readException();
            return true;
        } catch (RemoteException | RuntimeException error) {
            Log.e("AnlandMobileGL", "Could not give Anland Surface to embedded renderer", error);
            android.widget.Toast.makeText(context, "Embedded MobileGL could not start; see Anland logs",
                    android.widget.Toast.LENGTH_LONG).show();
            return false;
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    void detach() {
        surfaceGeneration++;
        surface = null;
        ready = null;
        if (worker == null) return;
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(MobileGLWorker.INTERFACE);
            if (worker.transact(MobileGLWorker.DETACH, data, reply, 0)) reply.readException();
        } catch (RemoteException | RuntimeException error) {
            Log.w("AnlandMobileGL", "Embedded renderer already detached", error);
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    void close() {
        detach();
        if (bound) context.unbindService(workerConnection);
        bound = false;
        worker = null;
    }
}
