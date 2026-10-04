/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
XISO.C (Chiefrim)

Chiefrim's Halo build replaces the port's xiso.c, which copies maps/ out of
an Xbox disc image at the first start. That file follows extract-xiso's code
and carries its license: the original 4-clause BSD license, with the
advertising clause, which is incompatible with the GPL (docs/LICENSING.md).
A GPL-3.0 build can't include it.

Chiefrim never needs it: tools/launch_halo.sh and tools/run_phase0.sh point
the game at a maps/ folder that is already extracted (HALO_MAPS), for
example by the upstream halo-ce-universal release, which keeps this
feature. So this stub only says so.
*/

#include "xiso.h"

#include <stdio.h>

int xiso_extract_maps(const char *image_path, const char *destination, xiso_progress_proc progress, void *context,
	char *error, int error_size)
{
	(void)image_path;
	(void)destination;
	(void)progress;
	(void)context;

	if (error && error_size > 0)
	{
		snprintf(error, (size_t)error_size,
			"This build (Chiefrim) can't extract a disc image. Extract maps/ with the upstream "
			"halo-ce-universal release, then set HALO_MAPS to the folder that holds it.");
	}
	return 0;
}
