# SAASound for VDAC4

Source: https://github.com/stripwax/SAASound/tree/3d92322030d1e5627cbf8295bed1efc872bad6eb
License: BSD-3-Clause (LICENSE).

Only the five chip components are built. The desktop configuration, DLL and
file-output wrapper are omitted. SAADevice.cpp includes SAADevice.h directly.
defns.h selects the SAM Coupe 8 MHz clock, fixed frequency table, 44.1 kHz PCM
and 2x oversampling; it does not enable configuration-file I/O.
