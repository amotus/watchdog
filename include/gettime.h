#ifndef _GETTIME_H_
#define _GETTIME_H_

#include <time.h>

time_t gettime(void);

static const long NSEC = 1000000000L; /* nanoseconds per second. */

/*
 * Alternatives to the BSD macros for Linux to manipulate 'struct timespec'.
 *
 * Note these *assume* the structures are normalised - that is the nanosecond
 * part is 0...999,999,999 otherwise the checks on the result of add/subtract
 * are insufficient to guarantee the result in normalised.
 */

#ifndef timespecadd
static inline void timespecadd(const struct timespec *a, const struct timespec *b, struct timespec *res)
{
	res->tv_sec = a->tv_sec + b->tv_sec;
	res->tv_nsec = a->tv_nsec + b->tv_nsec;
	if (res->tv_nsec >= NSEC) {
		res->tv_nsec -= NSEC;
		res->tv_sec ++;
	}
}
#endif /* timespecadd */

#ifndef timespecsub
static inline void timespecsub(const struct timespec *a, const struct timespec *b, struct timespec *res)
{
	res->tv_sec = a->tv_sec - b->tv_sec;
	res->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (res->tv_nsec < 0) {
		res->tv_nsec += NSEC;
		res->tv_sec --;
	}
}
#endif /* timespecsub */

#endif /* _GETTIME_H_ */
