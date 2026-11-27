/* SPDX-License-Identifier: Apache-2.0
 * JNI bridge: the same C decoder the engine runs, compiled for Android.
 * The Java side owns the Surface; this side owns the pixels.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "mvid.h"

typedef struct {
    uint8_t  *blob;
    size_t    size;
    mvid_t    v;
    uint8_t  *sheet;
    uint16_t *map;
    uint16_t *fb;      /* display size RGB565 */
    int       w, h;
} jmvid_t;

JNIEXPORT jlong JNICALL Java_dev_mosaic_mvid_Mvid_nativeOpen(JNIEnv *env, jclass c, jbyteArray bytes)
{
    (void)c;
    const jsize n = (*env)->GetArrayLength(env, bytes);
    jmvid_t *m = (jmvid_t *)calloc(1, sizeof(*m));
    if (!m) return 0;
    m->blob = (uint8_t *)malloc((size_t)n);
    m->sheet = (uint8_t *)malloc(MVID_MAX_TILES * 64);
    m->map = (uint16_t *)malloc(128 * 128 * 2);
    if (!m->blob || !m->sheet || !m->map) { free(m->blob); free(m->sheet); free(m->map); free(m); return 0; }
    (*env)->GetByteArrayRegion(env, bytes, 0, n, (jbyte *)m->blob);
    m->size = (size_t)n;
    if (mvid_open(&m->v, m->blob, m->size, m->sheet, m->map) != MVID_E_OK) {
        free(m->blob); free(m->sheet); free(m->map); free(m); return 0;
    }
    m->w = m->v.info.width; m->h = m->v.info.height;
    m->fb = (uint16_t *)malloc((size_t)m->w * m->h * 2u);
    if (!m->fb) { free(m->blob); free(m->sheet); free(m->map); free(m); return 0; }
    return (jlong)(intptr_t)m;
}

#define M(h) ((jmvid_t *)(intptr_t)(h))

JNIEXPORT jint JNICALL Java_dev_mosaic_mvid_Mvid_nativeWidth(JNIEnv *env, jclass c, jlong h)
{ (void)env; (void)c; return h ? M(h)->w : 0; }
JNIEXPORT jint JNICALL Java_dev_mosaic_mvid_Mvid_nativeHeight(JNIEnv *env, jclass c, jlong h)
{ (void)env; (void)c; return h ? M(h)->h : 0; }
JNIEXPORT jint JNICALL Java_dev_mosaic_mvid_Mvid_nativeFps(JNIEnv *env, jclass c, jlong h)
{ (void)env; (void)c; return h ? M(h)->v.info.fps : 12; }
JNIEXPORT jint JNICALL Java_dev_mosaic_mvid_Mvid_nativeFrames(JNIEnv *env, jclass c, jlong h)
{ (void)env; (void)c; return h ? M(h)->v.info.frames : 0; }
JNIEXPORT jint JNICALL Java_dev_mosaic_mvid_Mvid_nativeFrame(JNIEnv *env, jclass c, jlong h)
{ (void)env; (void)c; return h ? M(h)->v.cur : 0; }
JNIEXPORT jboolean JNICALL Java_dev_mosaic_mvid_Mvid_nativeSeek(JNIEnv *env, jclass c, jlong h, jint f)
{ (void)env; (void)c; return (h && mvid_seek(&M(h)->v, (uint16_t)f) == MVID_E_OK) ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jboolean JNICALL Java_dev_mosaic_mvid_Mvid_nativeNext(JNIEnv *env, jclass c, jlong h)
{ (void)env; (void)c; return (h && mvid_next(&M(h)->v)) ? JNI_TRUE : JNI_FALSE; }

/* Fill a direct ByteBuffer (w*h*2, RGB565 little-endian) with the frame. */
JNIEXPORT jboolean JNICALL Java_dev_mosaic_mvid_Mvid_nativeDraw(JNIEnv *env, jclass c, jlong h, jobject buf)
{
    (void)c;
    if (!h) return JNI_FALSE;
    jmvid_t *m = M(h);
    void *dst = (*env)->GetDirectBufferAddress(env, buf);
    if (!dst) return JNI_FALSE;
    mvid_draw(&m->v, m->fb, (unsigned)m->w, (unsigned)m->h);
    memcpy(dst, m->fb, (size_t)m->w * m->h * 2u);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_dev_mosaic_mvid_Mvid_nativeClose(JNIEnv *env, jclass c, jlong h)
{
    (void)env; (void)c;
    if (!h) return;
    jmvid_t *m = M(h);
    mvid_close(&m->v);
    free(m->fb); free(m->map); free(m->sheet); free(m->blob); free(m);
}
