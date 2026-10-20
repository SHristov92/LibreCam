# FatFs R0.15a

ChaN's FatFs (http://elm-chan.org/fsw/ff/), 1-clause BSD-style license (`LICENSE.txt`), unmodified except `ffconf.h`:

- `FF_USE_LFN 1` (static buffer), `FF_MAX_LFN 128`, `FF_CODE_PAGE 437`, `FF_FS_EXFAT 1`, `FF_FS_TINY 1`
- every external symbol is renamed with an `a9ff_` prefix, because the Beken SDK links its own older FatFs with the same function names

It is not thread-safe as configured (`FF_FS_REENTRANT 0`). `hal/bk7252/storage_bk7252.c` serializes all access with one mutex.
