/**
 * @file daemonize.c
 * @brief Daemonize the program
 */
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "defaults.h"
#include "log.h"

static void check_pidfile(const char *pidfile);
static char *getpwd(void);
static pid_t write_pidfile(const char *pidfile, pid_t pid);

extern char **environ;

/**
 * @brief Exit if the process referenced by @p pidfile is running
 * @param pidfile Path to PID file
 */
static void check_pidfile(const char *pidfile)
{
	pid_t pid;
	FILE* fd = fopen(pidfile, "r");		/* file isn't locked */
	if (fd != NULL) {
		notice("PID file '%s' already exists", pidfile);
		if (fscanf(fd, "%d", &pid) == 1 && kill(pid, 0) == 0) {
			notice("exiting, already daemonized (PID %d) ?", pid);
			fclose(fd);
			exit(EXIT_SUCCESS);
		}
		notice("existing PID file will be overwritten");
		fclose(fd);
	}
}

/**
 * @brief Get the current working directory
 * @return Absolute path to the current working directory if successful, or
 *         @p NULL if unsuccessful
 * @note Like @p get_current_dir_name(3)
 * @note If successful, caller is responsible for <tt>free(3)</tt>ing the result
 * @see @p get_current_dir_name(3)
 */
static char *getpwd(void)
{
	char *ret, *buf;

	if ((buf = malloc(PATH_MAX)) == NULL)
		ecritdie("cannot allocate %d (PATH_MAX) bytes: %s", PATH_MAX);

	if (getcwd(buf, PATH_MAX) == NULL) {
		eerr("cannot find current working directory: %s");
		return NULL;
	}

	if ((ret = strdup(buf)) == NULL)
		eerr("cannot duplicate current working directory: %s");

	free(buf);
	return ret;
}

/**
 * @brief Write a PID file
 *
 * Attempts to do so "atomically" as per @p daemon(7). Does not write PID file
 * if the file referenced by @p pidfile already exists, or the process ID
 * referenced by the file is in use by a running process.
 *
 * @param pidfile Path to PID file
 * @param pid A PID
 * @return The PID actually written to the PID file
 * @note Result should be equal to @pid
 * @note Exits if unsuccessful
 */
static pid_t write_pidfile(const char *pidfile, pid_t pid)
{
	char buf[16] = { "" };
	pid_t tmp = pid;	/* PID we want to write/verify */

	int ifd = open(pidfile, O_SYNC | O_CREAT | O_RDWR, 0644);

	if (flock(ifd, LOCK_EX | LOCK_NB) == -1)
		ecritdie("cannot lock PID file: %s");

	/* Check existing PID */
	for(int rv, i = 0; i < (int)sizeof(buf); ++i) {
		rv = read(ifd, &buf[i], 1);
		if (rv == -1) {
			ecritdie("cannot read PID file: %s");
		} else if (rv == 0) {
			buf[i] = '\0';
			break;
		}
	}
	buf[sizeof(buf) - 1] = '\0';
	pid = atoi(buf);

	if (pid != 0 && kill(pid, 0) == 0)
		ecritdie("found existing PID %d in PID file", pid);

	if (lseek(ifd, (off_t) 0, SEEK_SET) == -1)
		ecritdie("cannot rewind PID file: %s");

	pid = tmp;

	/* Write our PID */
	sprintf(buf, "%d\n", pid);

	if (write(ifd, buf, strlen(buf)) == -1)
		ecritdie("cannot write PID: %s");

	if (fsync(ifd) == -1)
		ecritdie("cannot sync PID file: %s");

	info("wrote PID %d to '%s'", pid, pidfile);

	if (lseek(ifd, (off_t) 0, SEEK_SET) == -1)
		ecritdie("cannot rewind PID file: %s");


	/* Verify written PID */
	for(int rv, i = 0; i < (int)sizeof(buf); ++i) {
		rv = read(ifd, &buf[i], 1);
		if (rv == -1) {
			ecritdie("cannot read PID file: %s");
		} else if (rv == 0) {
			buf[i] = '\0';
			break;
		}
	}
	buf[sizeof(buf) - 1] = '\0';
	pid = atoi(buf);

	if (close(ifd) == -1)
		ecritdie("cannot close PID file: %s");

	return pid;
}

/**
 * @brief Daemonize the program
 *
 * Attempts to do so in the manner described in @p daemon(7) - forks twice, with
 * the parent writing the PID of the second child to a PID file before exiting.
 * The daemon child also adds $PWD to its environment, in case any scripts it
 * executes require it.
 *
 * @param pidfile Path to PID file
 * @note Exits if unsuccessful
 */
void daemonize(const char *pidfile)
{
	check_pidfile(pidfile);

	/* Parent will write PID of (2nd) child and signal write completion. */
	int pipepc[2];			/* Parent writes, child reads */
	int pipecp[2];			/* Child writes, parent reads */
	if (pipe(pipepc) == -1 || pipe(pipecp) == -1)
		ecritdie("cannot create pipe: %s");

	pid_t pid;

	pid = fork();
	if (pid == -1) {
		ecritdie("cannot fork: %s");
	} else if (pid > 0) {
		/* Parent */
		close(pipepc[0]);
		close(pipecp[1]);
		if (read(pipecp[0], &pid, sizeof(pid)) != sizeof(pid))
			ecritdie("cannot read PID from child: %s");

		pid = write_pidfile(pidfile, pid);

		if (write(pipepc[1], &pid, sizeof(pid)) != sizeof(pid))
			ecritdie("cannot pipe PID back to child: %s");

		close(pipepc[1]);
		close(pipecp[0]);

		waitpid(pid, NULL, 0);
		debug("parent exiting");
		exit(EXIT_SUCCESS);
	}

	/* First child */
	close(pipepc[1]);
	close(pipecp[0]);

	if (setsid() == -1)
		ecritdie("setsid failed: %s");

	pid = fork();
	if (pid == -1) {
		ecritdie("cannot fork again: %s");
	} else if (pid > 0) {
		waitpid(-1, NULL, WNOHANG);	/* don't block waiting */
		debug("first child exiting");
		exit(EXIT_SUCCESS);
	}

	/* Second child (actual daemon process) */
	pid_t dpid = getpid();

	if (write(pipecp[1], &dpid, sizeof(dpid)) != sizeof(dpid))
		ecritdie("cannot pipe PID to parent: %s");

	if (read(pipepc[0], &pid, sizeof(pid)) != sizeof(pid))
		ecritdie("cannot read PID back from parent: %s");

	if (pid != dpid)
		ecritdie("got PID %d (not our own) from parent", pid);

	close(pipepc[0]);
	close(pipecp[1]);

	if (log_daemonize() == -1)
		exit(EXIT_FAILURE);

	umask(0);

	if (chdir(PEAPOD_ROOT_PATH) == -1)
		ecritdie("chdir to root directory '%s' failed: %s",
			 PEAPOD_ROOT_PATH);

	/* For script execution, but there's probably no real reason for this */
	char *pwd;
	if ((pwd = getpwd()) != NULL) {
		if (setenv("PWD", pwd, 1) == -1)
			eerr("cannot set environment variable PWD to '%s': %s",
			     pwd);
		free(pwd);
	}

	notice("successfully daemonized");
}
