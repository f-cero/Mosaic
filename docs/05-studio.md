# Mosaic Studio

Cross-platform SDL2 editor. Build for Linux (`-lSDL2`) or Windows (vcpkg SDL2).
It is deliberately dependency-light: no Electron, no browser, no Python at
runtime.

## Panels
* **Node palette** — click a node type to add it, then drag it on the canvas.
* **Graph** — drag nodes; drag from the right pin to a left pin to connect
  (exec pins amber, value pins blue). `Del` removes the selected node.
* **Inspector** — select a node; `-`/`+` change numeric properties.
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
Node position is canvas coordinates, so graphs round-trip losslessly.

## Screenshot mode (CI)
`mosaic-studio project.mpj --screenshot out.png --view graph|scene|sequence`
renders headlessly through SDL's dummy driver — used to produce the images in
this repo without a display.
