// SPDX-License-Identifier: Apache-2.0
package dev.mosaic.mvid;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.nio.ByteBuffer;

/** SurfaceView player. Not a game loop: it presents at the clip's fps. */
public final class MvidView extends SurfaceView implements SurfaceHolder.Callback, Runnable {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint paint = new Paint();
    private Mvid clip;
    private Bitmap frame;
    private ByteBuffer pixels;
    private volatile boolean running;

    public MvidView(Context ctx) {
        super(ctx);
        getHolder().addCallback(this);
        setKeepScreenOn(true);
        paint.setFilterBitmap(false);   // nearest: the art is pixel art
    }

    public void play(byte[] mvidBytes) {
        stop();
        clip = new Mvid(mvidBytes);
        frame = Bitmap.createBitmap(clip.width(), clip.height(), Bitmap.Config.RGB_565);
        pixels = ByteBuffer.allocateDirect(clip.width() * clip.height() * 2);
        running = true;
        handler.post(this);
    }

    public void stop() {
        running = false;
        handler.removeCallbacks(this);
        if (clip != null) { clip.close(); clip = null; }
        frame = null; pixels = null;
    }

    @Override public void run() {
        if (!running || clip == null) return;
        if (clip.draw(pixels)) {
            frame.copyPixelsFromBuffer(pixels);
            pixels.rewind();
            drawFrame();
        }
        if (!clip.next()) {
            if ((clip.frames() > 0)) clip.seek(0); else running = false;
        }
        handler.postDelayed(this, Math.max(8, 1000 / Math.max(1, clip.fps())));
    }

    private void drawFrame() {
        Canvas c = getHolder().lockCanvas();
        if (c == null) return;
        c.drawColor(0xFF000000);
        c.drawBitmap(frame, null, new Rect(0, 0, getWidth(), getHeight()), paint);
        getHolder().unlockCanvasAndPost(c);
    }

    @Override public void surfaceCreated(SurfaceHolder h) { if (clip != null && !running) { running = true; handler.post(this); } }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int ht) { }
    @Override public void surfaceDestroyed(SurfaceHolder h) { running = false; handler.removeCallbacks(this); }
}
