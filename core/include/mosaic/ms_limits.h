/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_limits.h: the normative platform limits (spec revision 1).
 * Every value here is mirrored by docs/01-platform-spec.md and by the
 * editor's validation pass. If they ever disagree the build is wrong.
 * Derived from the ESP-16 platform contract (PPU-16 + midicore). */
#ifndef MOSAIC_MS_LIMITS_H
#define MOSAIC_MS_LIMITS_H

#define MS_SCREEN_W        320u
#define MS_SCREEN_H        240u
#define MS_QFA_W           160u
#define MS_QFA_H           120u
#define MS_FPS             60u

#define MS_MAX_BG          2u
#define MS_MAX_AFFINE      2u
#define MS_MAX_SPRITES     128u
#define MS_MAX_MATRICES    16u
#define MS_TILE            8u
#define MS_SPRITE_MIN      8u
#define MS_SPRITE_MAX      64u
#define MS_MAX_TILES       1024u
#define MS_PAL_ENTRIES     256u
#define MS_PAL_SLOT_4BPP   16u
#define MS_MAP_TILES_MAX   65536u

#define MS_AUDIO_HZ        32000
#define MS_AUDIO_HZ_MIN    8000
#define MS_AUDIO_HZ_MAX    48000
#define MS_VOICES          16
#define MS_VOICES_MAX      64
#define MS_MIDI_CH         16

#define MS_CART_BYTES_MAX  (4u * 1024u * 1024u)
#define MS_BANK_BYTES_MAX  (128u * 1024u)
#define MS_PROGRAM_MAX     (256u * 1024u)

#define MS_VM_GLOBALS      256u
#define MS_VM_REGS         16u
#define MS_VM_STACK        32u
#define MS_MAX_ENTITIES    64u
#define MS_MAX_SHEETS      8u

#define MS_LINK_MAX_NODES  8u
#define MS_LINK_MTU        4096u
#define MS_LINK_SPI_HZ     (40u * 1000u * 1000u)

#define MS_SPR_MAGIC       0x32525053u  /* "SPR2" LE (canonical) */
#define MS_PROG_MAGIC      0x314D564Du  /* "MVM1" LE */
#define MS_BANK_MAGIC      0x314E4253u  /* "SBN1" LE */
#define MS_SPR1_MAGIC      0x31525053u  /* "SPR1" LE (deprecated, read-only) */

#endif
