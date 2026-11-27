# Freedom

Mosaic is free software and a free platform: Apache-2.0, no fee, no
certification, no permission to ask. Anyone may build a console, cut a
cartridge, write a game, give it away, sell it, fork it, or copy a cartridge
they own. That is the point of the platform, not a tolerated side effect.

So: **there is no DRM in Mosaic, and there will be none.** Not a weak one, not
an optional one, not one that is "off by default". A patch that adds one is a
bug report, not a feature.

## What that means, concretely

- **Nothing authenticates a cartridge.** cart16 v2 has no signature field, no
  key slot, no publisher id, no nonce, no counter. `ms_cart_mount` checks that
  the file is structurally a cartridge — magic, version, TOC checksum, payload
  checksums, payload validators — and nothing about who made it or whether it
  was made at all. A cart built by hand in hex is as legitimate as one built
  by Mosaic Studio.
- **No lockout, no region, no revocation, no expiry, no phone-home.** There is
  no code path that can refuse a cartridge for any reason other than "these
  bytes are not a cartridge". There is no server to consult, and the console
  works with the network switched off forever.
- **Copying is a first-class command, not an exploit.** `mosaic dump` reads a
  cartridge out byte for byte, with or without its SRAM, into a single file
  that the console can then run from its own storage with the cartridge in a
  drawer. The dumper works on an unseekable source and a sink smaller than the
  game, because a handheld with a 4 KB buffer should still be able to copy a
  cartridge.
- **A dump is not a derivative work.** The bytes that come out are the bytes
  that went in; `memcmp` is the test, and it is in `core/tests/test_dump.c`.
- **A game cannot tell how it was loaded.** There is no "source" field in the
  cartridge, the manifest, or the API. Running from a cartridge, from a dumped
  file, or from a friend's copy is the same execution, so no game can behave
  differently on a copy — and no game can be written to.
- **Saves belong to the player.** A save image can be listed, dumped and
  exported without the developer key. Import can be told to ignore the source
  build's identity entirely (`--any`), and the records are re-signed with the
  destination's key on the way in. Losing a save because a build changed is a
  bug in the save system, never a right of the save system.

## Every check in the platform, and its exit

| Check | Why it exists | How you get past it |
|---|---|---|
| TOC / payload crc32 | a truncated cart would otherwise crash the console | `mosaic dump` still copies it, and says which payload failed |
| `MS_CART_F_EXT` flag on kinds 8–13 | so a *strict* cart16 v2 reader rejects rather than misparses a Mosaic cart | set the flag; the packer does it for you |
| Save `dev_id` / `app_id` binding | so a game cannot silently overwrite a stranger's progress | `ms_save_open_raw`, `save list`, `save dump`, `save import --any` |
| Save record MAC (SipHash-2-4) | a hand-edited body that recomputed the cheap crc would otherwise load | ignored by raw open; not checked on import; re-signed on write |
| `save_reads` grant | a sequel should not get write access to the predecessor's save | share one `save_id=` instead, or export/import |

None of these can stop a copy, and none of them are meant to. They exist to
turn silent corruption into a named error, which is the only thing a save
system is allowed to protect: the player's data from the player's own crash,
never the publisher's file from the player.

## What we will not add

Cartridge authentication. Online activation. Per-cartridge play limits.
Region locks. A "signed" cartridge that the runtime prefers or requires.
Telemetry that identifies a cartridge or a player. A save format whose MAC
cannot be skipped. A dumper that is "for backup only" — there is no such
distinction here and we will not pretend to enforce one.

## Honest limits

The MAC is an integrity guard keyed by a key that ships inside the cartridge;
the spec says so in those words. It stops accidents, not people. It cannot
prevent copying and does not try to, and the copy path does not consult it.

This document is about Mosaic's own behaviour. It makes no claim about what
anyone else's platform permits, and nothing here is legal advice about the
cartridges *you* own in *your* jurisdiction — Mosaic's position is simply that
the software will never be the thing standing in your way.

## The server asks for nothing

`mosaic serve` has no account, no login, no session token, no player id and no
telemetry: a peer says what it can draw, receives registers, and may leave. The
session's `cart_crc` is an integrity check so two different builds do not try
to render each other's frames — it is not a permission bit, and a display node
that fails it is told so rather than silently authenticated. A save lives in the
image file the operator passes with `--saves`; it belongs to whoever holds that
file, which is the same rule the handheld follows.
