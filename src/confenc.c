#include <stdio.h>

int main (int argc, char *argv[]) {
  fwrite ("\0\0"
	  "\1\0"
	  "\2\0"
	  "\3\0"
	  "\4\0"
	  "\5\0", 1, 6 * 2, stdout);
  return 0;
}
