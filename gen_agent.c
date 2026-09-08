/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Enno Boland <g@s01.de>
 */

#include <assert.h>
#include <stdio.h>

int
main(void) {
	int squote = 0, dquote = 0;
	fputs("'\\\'',", stdout);
	for (int byte; (byte = getchar()) != EOF;) {
		squote |= byte == '\'';
		dquote |= byte == '"';
		printf("%d,", byte == '\'' ? '"' : byte);
	}
	assert(!squote || !dquote);
	fputs("'\\\'',0", stdout);
	return 0;
}
