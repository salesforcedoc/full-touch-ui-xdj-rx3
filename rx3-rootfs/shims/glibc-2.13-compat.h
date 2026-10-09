/* glibc-2.13-compat.h -- hold the shims to the libc ABI they are loaded into.
 *
 * The shims are LD_PRELOADed into rbp, which links against the RX3 v1.20 rootfs's glibc 2.13.
 * The build host's headers describe a much newer glibc, so anything they declare that 2.13 does
 * not export becomes an unresolved import: the .so loads and then dies on the first call.
 * build-shims.sh checks every import against the four libs from the rootfs, so such a symbol is
 * caught at build time rather than on the hardware.
 *
 * Two host-side defaults need undoing:
 *
 *   - _FILE_OFFSET_BITS/_TIME_BITS default to 64 on Debian 13 (trixie) armhf, which renames
 *     libc entry points; undone with -U_FILE_OFFSET_BITS -U_TIME_BITS in build-shims.sh.
 *
 *   - glibc 2.38+ added the ISO C23 entry points and redirects the classic names to them.
 *     features.h sets _ISOC23_SOURCE (and _ISOC2Y_SOURCE) whenever _GNU_SOURCE is defined --
 *     which all five shims do -- so <stdio.h> does `#define sscanf __isoc23_sscanf` under
 *     __GLIBC_USE (ISOC23), and <stdlib.h> declares strtol as
 *     __REDIRECT_NTH(strtol, ..., __isoc23_strtol) under __GLIBC_USE (C23_STRTOL). glibc 2.13
 *     exports neither. That is this header's job.
 *
 * features.h only computes __GLIBC_USE_* inside its own include guard, so running it first and
 * then clearing the switches here sticks: no later header can put them back. bits/libc-header-start.h
 * is by design guard-less and re-derives its own __GLIBC_USE_IEC_60559_*_C23 switches on every
 * include, but it derives them from __GLIBC_USE (ISOC23), so they follow the override for free.
 * Everything guarded by __GLIBC_USE (C23_STRTOL) -- the strtol/strtoul family, which is what
 * touchshim and knobshim call -- needs the third switch cleared explicitly, because features.h
 * computes that one itself from __GLIBC_USE (ISOC23) and has already done so by the time this runs.
 *
 * _GNU_SOURCE is defined before features.h runs because build-shims.sh passes this file with
 * -include, i.e. ahead of the source's own `#define _GNU_SOURCE`; without it features.h would
 * never see the macro, and the shims would lose __USE_GNU (RTLD_NEXT, vasprintf, ...). The bare
 * form matches the sources' own `#define _GNU_SOURCE` exactly, so neither one warns.
 */
#ifndef RX3_GLIBC_2_13_COMPAT_H
#define RX3_GLIBC_2_13_COMPAT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <features.h>

#undef  __GLIBC_USE_ISOC23
#define __GLIBC_USE_ISOC23	0
#undef  __GLIBC_USE_ISOC2Y
#define __GLIBC_USE_ISOC2Y	0
#undef  __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL	0

#endif /* RX3_GLIBC_2_13_COMPAT_H */
