#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _XOPEN_SOURCE 500	/* for getsid(2) */
#define _BSD_SOURCE		/* for acct(2) */
#define _DEFAULT_SOURCE	/* To stop complaints with gcc >= 2.19 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <netdb.h>
#include <paths.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <utmp.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "watch_err.h"
#include "extern.h"
#include "ext2_mnt.h"

#if defined __GLIBC__
#include <sys/quota.h>
#include <sys/swap.h>
#include <sys/reboot.h>
#else				/* __GLIBC__ */
#include <linux/quota.h>
#endif				/* __GLIBC__ */

#include <unistd.h>

#ifndef NSIG
#define NSIG _NSIG
#endif

#ifndef __GLIBC__
#ifndef RB_AUTOBOOT
#define RB_AUTOBOOT	0xfee1dead,672274793,0x01234567 /* Perform a hard reset now.  */
#define RB_ENABLE_CAD	0xfee1dead,672274793,0x89abcdef /* Enable reboot using Ctrl-Alt-Delete keystroke.  */
#define RB_HALT_SYSTEM	0xfee1dead,672274793,0xcdef0123 /* Halt the system.  */
#define RB_POWER_OFF	0xfee1dead,672274793,0x4321fedc /* Stop system and switch power off if possible.  */
#endif /*RB_AUTOBOOT*/
#endif /* !__GLIBC__ */

extern void umount_all(void *);
extern int ifdown(void);
#if 0
extern int mount_one(char *, char *, char *, char *, int, int);
static struct mntent rootfs;
#endif

/* close the device and check for error */
static void close_all(void)
{
	close_watchdog();
	close_loadcheck();
	close_memcheck();
	close_tempcheck();
	close_heartbeat();
	free_process();		/* What check_bin() was waiting to report. */
}

/* on exit we close the device and log that we stop */
void terminate(int ecode)
{
	log_message(LOG_NOTICE, "stopping daemon (%d.%d)", MAJOR_VERSION, MINOR_VERSION);
	unlock_our_memory();
	close_all();
	remove_pid_file();
	close_logging();
	usleep(100000);		/* 0.1s to make sure log is written */
	exit(ecode);
}

/* panic: we're still alive but shouldn't */
static void panic(void)
{
	/*
	 * Okay we should never reach this point,
	 * but if we do we will cause the hard reset
	 */
	open_logging(NULL, MSG_TO_STDERR | MSG_TO_SYSLOG);
	log_message(LOG_ALERT, "WATCHDOG PANIC: failed to reboot, trying hard-reset");
	sleep(dev_timeout * 4);

	/* if we are still alive, we just exit */
	log_message(LOG_ALERT, "WATCHDOG PANIC: still alive after sleeping %d seconds", 4 * dev_timeout);
	close_all();
	close_logging();
	exit(1);
}

static void mnt_off(void)
{
	FILE *fp;
	struct mntent *mnt;

	fp = setmntent(_PATH_MNTTAB, "r");
	/* in some rare cases fp might be NULL so be careful */
	while (fp != NULL && ((mnt = getmntent(fp)) != (struct mntent *)0)) {
		/* First check if swap */
		if (!strcmp(mnt->mnt_type, MNTTYPE_SWAP))
			if (swapoff(mnt->mnt_fsname) < 0)
				perror(mnt->mnt_fsname);

		/* quota only if mounted at boot time && filesytem=ext2 */
		if (hasmntopt(mnt, MNTOPT_NOAUTO) || strcmp(mnt->mnt_type, MNTTYPE_EXT2))
			continue;

		/* group quota? */
		if (hasmntopt(mnt, MNTOPT_GRPQUOTA))
			if (quotactl(QCMD(Q_QUOTAOFF, GRPQUOTA), mnt->mnt_fsname, 0, (caddr_t) 0) < 0)
				perror(mnt->mnt_fsname);

		/* user quota */
		if (hasmntopt(mnt, MNTOPT_USRQUOTA))
			if (quotactl(QCMD(Q_QUOTAOFF, USRQUOTA), mnt->mnt_fsname, 0, (caddr_t) 0) < 0)
				perror(mnt->mnt_fsname);

#if 0
		/* not needed anymore */
		/* while we're at it we add the remount option */
		if (strcmp(mnt->mnt_dir, "/") == 0) {
			/* save entry if root partition */
			rootfs.mnt_freq = mnt->mnt_freq;
			rootfs.mnt_passno = mnt->mnt_passno;

			rootfs.mnt_fsname = strdup(mnt->mnt_fsname);
			rootfs.mnt_dir = strdup(mnt->mnt_dir);
			rootfs.mnt_type = strdup(mnt->mnt_type);

			/* did we get enough memory? */
			if (rootfs.mnt_fsname == NULL || rootfs.mnt_dir == NULL || rootfs.mnt_type == NULL) {
				log_message(LOG_ERR, "out of memory");
			}

			if ((rootfs.mnt_opts = malloc(strlen(mnt->mnt_opts) + strlen("remount,ro") + 2)) == NULL) {
				log_message(LOG_ERR, "out of memory");
			} else
				sprintf(rootfs.mnt_opts, "%s,remount,ro", mnt->mnt_opts);
		}
#endif
	}
	endmntent(fp);
}

/*
 * Kill everything, but depending on 'aflag' spare kernel/privileged
 * processes. Do this twice in case we have out-of-memory problems.
 *
 * The value of 'stime' is the delay from 2nd SIGTERM to SIGKILL but
 * the SIGKILL is only used when 'aflag' is true as things really bad then!
 */

static void kill_everything_else(int aflag, int stime)
{
	int ii;

	/* Ignore all signals (except children, so run_func_as_child() works as expected). */
	for (ii = 1; ii < NSIG; ii++) {
		if (ii != SIGCHLD) {
			signal(ii, SIG_IGN);
		}
	}

	/* Stop init; it is insensitive to the signals sent by the kernel. */
	kill(1, SIGTSTP);

	/* Try to terminate processes the 'nice' way. */
	killall5(SIGTERM, aflag);
	safe_sleep(1);
	/* Do this twice in case we have out-of-memory problems. */
	killall5(SIGTERM, aflag);

	/* Now wait for most processes to exit as intended. */
	safe_sleep(stime);

	if (aflag) {
		/* In case that fails, send them the non-ignorable kill signal. */
		killall5(SIGKILL, aflag);
		keep_alive();
		/* Out-of-memory safeguard again. */
		killall5(SIGKILL, aflag);
		keep_alive();
	}
}

/* part that tries to shut down the system cleanly */
static void try_clean_shutdown(int errorcode)
{
	int i = 0, fd;
	char *seedbck = RANDOM_SEED;

	/* soft-boot the system */
	/* do not close open files here, they will be closed later anyway */
	/* close_all(); */

	/* if we will halt the system we should try to tell a sysadmin */
	send_email(errorcode, NULL);

	close_logging();

	safe_sleep(1);		/* make sure log is written */

	/* We cannot start shutdown, since init might not be able to fork. */
	/* That would stop the reboot process. So we try rebooting the system */
	/* ourselves. Note, that it is very likely we cannot start any rc */
	/* script either, so we do it all here. */

	/* Close all files except the watchdog device. */
	for (i = 0; i < 3; i++)
		if (!isatty(i))
			close(i);
	for (i = 3; i < 20; i++)
		if (i != get_watchdog_fd())
			close(i);
	close(255);

	kill_everything_else(TRUE, sigterm_delay-1);

	/* Remove our PID file, as nothing should be capable of starting a 2nd daemon now. */
	remove_pid_file();

	/* Record the fact that we're going down */
	if ((fd = open(_PATH_WTMP, O_WRONLY | O_APPEND)) >= 0) {
		time_t t;
		struct utmp wtmp;
		memset(&wtmp, 0, sizeof(wtmp));

		time(&t);
		strcpy(wtmp.ut_user, "shutdown");
		strcpy(wtmp.ut_line, "~");
		strcpy(wtmp.ut_id, "~~");
		wtmp.ut_pid = 0;
		wtmp.ut_type = RUN_LVL;
		wtmp.ut_time = t;
		if (write(fd, (char *)&wtmp, sizeof(wtmp)) < 0)
			log_message(LOG_ERR, "failed writing wtmp (%s)", strerror(errno));
		close(fd);
	}

	/* save the random seed if a save location exists */
	/* don't worry about error messages, we react here anyway */
	if (strlen(seedbck) != 0) {
		int fd_seed;

		if ((fd_seed = open("/dev/urandom", O_RDONLY)) >= 0) {
			int fd_bck;

			if ((fd_bck = creat(seedbck, S_IRUSR | S_IWUSR)) >= 0) {
				char buf[512];

				if (read(fd_seed, buf, 512) == 512) {
					if (write(fd_bck, buf, 512) < 0)
						log_message(LOG_ERR, "failed writing urandom (%s)", strerror(errno));
				}
				close(fd_bck);
			}
			close(fd_seed);
		}
	}

	/* Turn off accounting */
	if (acct(NULL) < 0)
		log_message(LOG_ERR, "failed stopping acct() (%s)", strerror(errno));

	keep_alive();

	/* Turn off quota and swap */
	mnt_off();

	/* umount all partitions */
	umount_all(NULL);

#if 0
	/* with the more recent version of mount code, this is not needed anymore */
	/* remount / read-only */
	//if (setjmp(ret2dog) == 0)
		mount_one(rootfs.mnt_fsname, rootfs.mnt_dir, rootfs.mnt_type,
			  rootfs.mnt_opts, rootfs.mnt_freq, rootfs.mnt_passno);
#endif

	/* shut down interfaces (also taken from sysvinit source */
	ifdown();
}

/* shut down the system */
void do_shutdown(int errorcode)
{
	/* tell syslog what's happening */
	log_message(LOG_ALERT, "shutting down the system because of error %d = '%s'", errorcode, wd_strerror(errorcode));

	if(errorcode != ERESET)	{
		try_clean_shutdown(errorcode);
	} else {
		/* We have been asked to hard-reset, make basic attempt at clean filesystem
		 * but don't try stopping anything, etc, then used device (below) to do reset
		 * action.
		 */
		sync();
		sleep(1);
	}

	/* finally reboot */
	if (errorcode != ETOOHOT) {
		if (get_watchdog_fd() != -1) {
			/* We have a hardware timer, try using that for a quick reboot first. */
			set_watchdog_timeout(1);
			sleep(dev_timeout * 4);
		}
		/* That failed, or was not possible, ask kernel to do it for us. */
		reboot(RB_AUTOBOOT);
	} else {
		if (temp_poweroff) {
			/* Tell system to power off if possible. */
			reboot(RB_POWER_OFF);
		} else {
			/* Turn on hard reboot, CTRL-ALT-DEL will reboot now. */
			reboot(RB_ENABLE_CAD);
			/* And perform the `halt' system call. */
			reboot(RB_HALT_SYSTEM);
		}
	}

	/* unbelievable: we're still alive */
	panic();
}
