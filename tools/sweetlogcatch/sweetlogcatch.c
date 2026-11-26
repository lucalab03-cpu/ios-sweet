/*
 * sweetlogcatch.dylib: a program's own os_log, caught inside it (2026-11-26).
 *
 * Inserted with DYLD_INSERT_LIBRARIES by launchd (`sweetjob env LABEL DYLD_INSERT_LIBRARIES=...`, kernel patch 0153
 * lets dyld take it in platform binaries). Its constructor sets OS_ACTIVITY_DT_MODE=YES before libtrace has read it
 * (libtrace then writes every message to standard error too), points standard error at a pipe and a thread copies
 * the lines to /var/tmp/sweetlog-PROGRAM.txt: every line for PosterBoard and the posters, only the lines about
 * posters and wallpapers for SpringBoard (it says a great deal), 6000 lines at most a process. The same as the
 * MT6768 port of github.com/nicolascuaces/hakios-xnu-kernel does (inject_sb.c, poster_log=1).
 *
 * Why: PosterBoard never asks for a poster extension (no wallpaper; round ap) and says why nowhere we can read:
 * the same variable given by launchd with stderr to a file left that file empty.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int out_fd = -1;
static int filtered;

static void
put(const char *text, size_t length)
{
	if (out_fd >= 0) {
		write(out_fd, text, length);
	}
}

static int
interesting(const char *line)
{
	static const char *const words[] = { "oster", "allpaper", "PBF", "PRS", "PBUI", "xtension", "ssert", "rror", "fail" };

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strstr(line, words[i]) != NULL) {
			return 1;
		}
	}
	return 0;
}

static void *
reader(void *argument)
{
	FILE *in = fdopen((int)(intptr_t)argument, "r");
	char line[2048];
	unsigned kept = 0;

	while (in != NULL && fgets(line, sizeof(line), in) != NULL) {
		if (kept >= 6000 || (filtered && !interesting(line))) {
			continue;
		}
		put(line, strlen(line));
		kept++;
	}
	return NULL;
}

__attribute__((constructor)) static void
sweet_log_catch(void)
{
	const char *name = getprogname();
	char path[256], head[256];
	int fds[2];
	pthread_t thread;
	time_t now = time(NULL);

	snprintf(path, sizeof(path), "/var/tmp/sweetlog-%s.txt", name);
	out_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	filtered = strcmp(name, "SpringBoard") == 0;
	snprintf(head, sizeof(head), "===== sweetlogcatch: %s[%d] started %s", name, getpid(), ctime(&now));
	put(head, strlen(head));
	if (pipe(fds) != 0) {
		put("sweetlogcatch: no pipe\n", 23);
		return;
	}
	setenv("OS_ACTIVITY_DT_MODE", "YES", 1);
	dup2(fds[1], 2);
	close(fds[1]);
	pthread_create(&thread, NULL, reader, (void *)(intptr_t)fds[0]);
	pthread_detach(thread);
}
