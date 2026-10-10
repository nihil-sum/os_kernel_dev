#include <unistd.h>
#include <stdio.h>

int main(void) {
	printf("%d,%d,%d",STDIN_FILENO,STDOUT_FILENO,STDERR_FILENO);
	//For UNIX operating systems the file descriptor 
	//0 is reserved for stdin
	//1 is reserved for stdout
	//2 is reserved for stderr
	return 0;
}
