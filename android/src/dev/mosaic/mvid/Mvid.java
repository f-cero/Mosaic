// SPDX-License-Identifier: Apache-2.0
package dev.mosaic.mvid;

import java.nio.ByteBuffer;

/** MVID decoder — the engine's C decoder, through JNI. */
public final class Mvid implements AutoCloseable {
    static { System.loadLibrary("mvid"); }

    private long handle;

    private static native long nativeOpen(byte[] bytes);
    private static native int nativeWidth(long h);
    private static native int nativeHeight(long h);
    private static native int nativeFps(long h);
    private static native int nativeFrames(long h);
    private static native int nativeFrame(long h);
    private static native boolean nativeSeek(long h, int frame);
    private static native boolean nativeNext(long h);
    private static native boolean nativeDraw(long h, ByteBuffer dst);
    private static native void nativeClose(long h);

    public Mvid(byte[] clip) { handle = nativeOpen(clip); if (handle == 0) throw new IllegalArgumentException("not an MVID clip"); }

    public int width() { return nativeWidth(handle); }
    public int height() { return nativeHeight(handle); }
    public int fps() { return nativeFps(handle); }
    public int frames() { return nativeFrames(handle); }
    public int position() { return nativeFrame(handle); }
    public boolean seek(int f) { return nativeSeek(handle, f); }
    public boolean next() { return nativeNext(handle); }
    /** dst must be a direct buffer of width*height*2 bytes (RGB565). */
    public boolean draw(ByteBuffer dst) { return nativeDraw(handle, dst); }

    @Override public void close() { if (handle != 0) { nativeClose(handle); handle = 0; } }
}
