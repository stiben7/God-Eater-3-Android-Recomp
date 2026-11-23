# Developer notes

What the port is made of, and what we learned getting Monster Hunter Rise to run on Android phones. The main
[README](../README.md) is for players (install, settings, every switch); these pages are for whoever works on the code.

- [How the port works](architecture.md): the loader, the SDK layer, NVN on Vulkan, memory, shaders, presenting.
- [Monster Hunter Rise, as the port sees it](mh-rise.md): how the game renders, the engine objects and addresses the port
  touches, and the game's own settings.
- [GPUs and drivers](gpus-and-drivers.md): what each family (Adreno with Qualcomm's driver, Turnip on A7xx and A8xx, Mali,
  PowerVR) needs, and why.
- [Fixes](fixes.md): bugs we hit, what caused them and what fixed them. Also what we tried that didn't help, so nobody
  tries it again.
- [Debugging](debugging.md): the log, the switches for debugging, frame captures, GPU breadcrumbs, probes and the
  other tools.

Code comments are mostly in Spanish and carry the evidence for each decision (trace names, addresses, measurements).
These pages are the summary; when they and a comment disagree, the comment next to the code is the one to trust.
