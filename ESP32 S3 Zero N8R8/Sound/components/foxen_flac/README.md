# libfoxenflac

Upstream: https://github.com/astoeckel/libfoxenflac
Revision: e491d2ec76709975f09d1820367a92a1772ccfb1
License: LGPL-2.1-or-later (see LICENSE).

The two upstream source files are unmodified. CRC checks remain enabled.
FLAC.ELF is independently rebuildable with tools/build_modules.ps1 flac;
replace /WC/ESP/FLAC.ELF to use a modified version of this library.
The adapter accepts native FLAC, 4–24 bits, 1–8 channels, and allocates its
decoder from the STREAMINFO maximum block size and channel count.
