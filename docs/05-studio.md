# Mosaic Studio

Cross-platform SDL2 editor. Build for Linux (`-lSDL2`) or Windows (vcpkg SDL2).
It is deliberately dependency-light: no Electron, no browser, no Python at
runtime.

## Panels
* **Node palette** — click a node type to add it, then drag it on the canvas.
* **Graph** — drag nodes; drag from the right pin to a left pin to connect
  (exec pins amber, value pins blue). `Del` removes the selected node.
* **Inspector** — select a node; `-`/`+` change numeric properties.
* **Sprite matrix** — `<`/`>` pick one of the 16 shared slots; `-`/`+` edit
  `a b c d` in 0.05 steps and the wireframe preview shows the warp. Below it,
  `affine` and `linetab` choose the selected entity's object matrix and
  per-scanline table. Each edit writes the `.mpj` line and rebuilds, so the
  Scene tab shows the result immediately.
* **Scene** — live 320×240 preview of the compiled game; arrow keys/A move,
  Space pauses. This runs the real engine, not a mock.
* **Sequence** — the compiled exec chain, in order.
* **Assets** — files in `assets/`, with format hints.
* **Console** — build results and validator errors (the same text the CLI
  prints).

## Workflow
1. `Build` saves the `.mpj`, converts PNG/SMF assets, compiles graphs and
   mounts the resulting cart (validation errors appear in the console).
2. `Play` runs the cart in the preview.
3. `Export` writes `build/game.mcart` next to the project.
4. `mosaic rom` wraps the cart in a flashable image for the ESP32.

## Project file (`.mpj`)
Line-oriented and diff-friendly:
```
title=Starfall
mode=rich
level=assets/level.png 4
music=assets/theme.mid
sheet player assets/player.png 16 16 4
entity player play sheet=0 x=160 y=176 anim=0 priority=1
node 1 update 40 40
node 2 if 220 40
node 3 held 220 150 mask=4
node 4 addx 400 40 amount=-1.5
link 1.next 2.in
link 2.cond 3.out
link 2.then 4.in
```
`mat set=N a=.. b=.. c=.. d=..` appends one row to per-scanline table `N`;
rows accumulate in file order and tables must be contiguous from 0. They
compile to cart kind 12, and an entity selects them with
`affine=<slot> linetab=<table>` on its own line:
```
save=8192                      # declare an 8 KB console+cartridge save region
save_loc=both                  # console | cart | both
dev=cero-labs                  # developer key: namespaces the studio
dev_key=mosaic-demo-key        # keys the record MAC (defaults to dev=)
save_id=starfall               # names the save, not the title
save_reads=starfall            # read-only grant to another save id
mat set=0 a=1.00 b=0.22 c=0.00 d=1.00     # scanline 0 of table 0
entity player play sheet=0 x=160 y=176 affine=1 linetab=0
node 2 setmat 40 110 slot=1 a=1.00 b=0.35 c=0.00 d=1.00
```
`examples/warp/` is the reference for matrices: a 16-row sine table plus a
leaning object matrix, exercising `SETMAT`, `AFFINE` and `LINETAB` in one cart.
`examples/starfall2/` is the reference for save linkage: it declares
`save_reads=starfall`, so it opens the first game's save read-only while
keeping its own.

Node position is canvas coordinates, so graphs round-trip losslessly.

## Screenshot mode (CI)
`mosaic-studio project.mpj --screenshot out.png --view graph|scene|sequence`
renders headlessly through SDL's dummy driver — used to produce the images in
this repo without a display.
