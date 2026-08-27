#include <assert.h>
#include <stdio.h>

int
main(void) {
	fputs("'\\\'',", stdout);
	for (int byte; (byte = getchar()) != EOF;) {
		assert(byte != '\'');
		printf("%d,", byte);
	}
	fputs("'\\\'',0", stdout);
	return 0;
}
