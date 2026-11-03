# FatFs R0.15 (vendored)

ChaN's FatFs, generic FAT filesystem module, release R0.15.

- Source: http://elm-chan.org/fsw/ff/arc/ff15.zip
  (SHA-256 `e0d76654d877e6c74be5ea3c395808794d495169514e98cbf6046168b8f4f070`,
  downloaded 2026-10-31)
- License: `LICENSE.txt` (ChaN's 1-clause BSD-style license; the notice at the
  top of each source file is kept unmodified).

What's here, unmodified from the archive's `source/` directory: `ff.c`, `ff.h`,
`ffunicode.c`, `diskio.h`, `00readme.txt`, `00history.txt`.

Left out on purpose:

- `ffconf.h` -- the launcher's own configuration is `src/disk/ffconf.h`
  (it lists what differs from the stock file). Keeping the stock one here
  would shadow it, since `ff.h` includes `"ffconf.h"` relative to itself first.
- `diskio.c` (the sample glue) -- the launcher's is `src/disk/diskio.c`, over
  the block device (`src/disk/blockdev.h`).
- `ffsystem.c` -- only needed for `FF_USE_LFN 3` or `FF_FS_REENTRANT`, neither
  of which the launcher uses.
- The `documents/` tree.
