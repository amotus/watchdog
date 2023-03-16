#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>

#include "gettime.h"

/*
 * Return current MONOTONIC time.
 * (like time(NULL), but monotonic)
 */
time_t gettime(void)
{
	struct timespec now = {};

#if defined( CLOCK_BOOTTIME )
	/* If available this avoids CLOCK_MONOTONIC being wrong if mostly suspended. */
	int rc = clock_gettime(CLOCK_BOOTTIME, &now);
#else
	int rc = clock_gettime(CLOCK_MONOTONIC, &now);
#endif /*CLOCK_BOOTTIME*/

	if (rc < 0) {
		return -1;
	}
	return now.tv_sec;
}
