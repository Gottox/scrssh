/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Enno Boland <g@s01.de>
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void) {
	char *line = NULL, *p;
	size_t n = 0;
	puts("'\\'',");
	while (getline(&line, &n, stdin) > 0) {
		assert(strchr(line, '\'') == NULL);
		if ((p = strchr(line, '#'))) {
			*p = 0;
		}
		for (p = &line[strlen(line) - 1]; strchr(" \t\n", *p); p--) {
			*p = 0;
		}
		for (p = line; *p && *p != '\n' && *p != '#'; p++) {
			printf("%d,", *p);
		}
		if (p != line) {
			puts("'\\n',");
		}
	}
	puts("'\\'',0\n");
	free(line);
	return 0;
}
