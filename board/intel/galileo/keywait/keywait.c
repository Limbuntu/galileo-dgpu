/* keywait: exit when any key on any /dev/input/event* is pressed.
 * Independent of tty mode/job control: reads raw evdev events. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <linux/input.h>

int main(void) {
	glob_t g;
	if (glob("/dev/input/event*", 0, NULL, &g) != 0) return 2;
	int fd[64], n = 0;
	struct pollfd pfd[64];
	for (size_t i = 0; i < g.gl_pathc && n < 64; i++) {
		int f = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK);
		if (f < 0) continue;
		fd[n] = f; pfd[n].fd = f; pfd[n].events = POLLIN; n++;
	}
	globfree(&g);
	if (!n) return 3;
	for (;;) {
		int r = poll(pfd, n, 1000);
		if (r <= 0) continue;
		for (int i = 0; i < n; i++) {
			if (!(pfd[i].revents & POLLIN)) continue;
			struct input_event ev[16];
			int rd = read(fd[i], ev, sizeof(ev));
			for (int k = 0; k < rd / (int)sizeof(struct input_event); k++)
				if (ev[k].type == EV_KEY && ev[k].value == 1) return 0;
		}
	}
}
