# x0x-lite licensing

Everything under `apps/x0x/` (and its host test, `test/x0x/`) is free software under the GNU
General Public License, version 3 only (`GPL-3.0-only`, full text in `LICENSE` next to this
file). It is the one exception to the rest of this repository, which is MIT (the repository's
`LICENSE` and `NOTICE`). x0x-lite is a port of X0X, which is GPL-3.0-only, so
it keeps X0X's licence. If you distribute `x0x.e16app`, or anything built from this folder, you
must give its recipients the complete corresponding source under the same licence.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| 303 (`core/bass303.*`) | X0X's `dsp/bass303`, ported from [schwung-303](https://github.com/charlesvestal/schwung-303): Open303 by Robin Schmidt (MIT), Devilfish extensions after jc303 (midilab), RAT drive after dm-Rat (Dave Mollen) | GPL-3.0 (Open303 parts MIT) |
| 808 kit (`core/drum808.*`) | X0X's `dsp/drum808`, ported from [8W8](https://github.com/athousanddetails/schwung-8W8) (athousanddetails); circuit models after the TR-808 service notes and Werner / Abel / Smith | GPL-3.0 |
| TB-3PO (`core/tb3po.*`) | X0X's `seq/tb3po`, ported from [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po), itself a port of the Phazerville Hemisphere Suite `TB_3PO` applet (djphazer and contributors) | GPL-3.0 |
| Sequencer, patterns, maths (`core/seq.*`, `core/pattern.*`, `core/fastmath.h`, `core/x0x_param.h`) | X0X's `seq/sequencer`, `seq/pattern`, `dsp/fastmath.h`, `dsp/x0x_param.h` (X0X began as a fork of [Felucca](https://github.com/hugelton/Felucca), Leo Kuroshita, Hügelton Instruments, by way of [X0X for the M-VAVE FM-1](https://github.com/charlesvestal/fm1-x0x)) | GPL-3.0-only |
| The screen font (`gfx.c`) | the classic 5 x 7 LCD font, as X0X's `firmware/osdk/oled.c` carries it | public domain |
| The engine, the E16 app and its test (`core/x0x.*`, `core/x0x_save.*`, `app.c`, `gfx.*`, `test/x0x/x0x_test.c`) | x0x-lite, written for the E16 (`core/x0x_save.*` is `apps/drone/core/drone_save.*` renamed, by the same author) | GPL-3.0-only |

X0X itself: <https://github.com/charlesvestal/tbd-x0x> (its `LICENSING.md` has the full credits).

## How it runs, and the launcher

`x0x.e16app` is a separate program: the launcher loads it from the E16's disk at run time and it
talks to the hardware only through the launcher's API table (`src/apps/e16_api.h`), which it is
handed as `app_main`'s argument. It contains no launcher code but the app SDK's interface and
entry stub (`apps/sdk/e16_app.h`, `src/apps/e16_api.h`, `apps/sdk/crt0.c`, `apps/sdk/app.ld`),
which exist so that separately written apps can be built against the launcher. The launcher
firmware does not contain x0x-lite, and the GPL here does not extend to it.

No GPL code from this folder is used anywhere else in the repository.

## Trademarks

TR-808 and TB-303 are trademarks of Roland Corporation, used only to describe what the parts
emulate. x0x-lite is not affiliated with or endorsed by Roland or OXI Instruments.
