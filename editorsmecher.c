#include <unistd.h>
#include <unistd.h>

void enableRaw(){
	struct termios raw;

	tcgetattr(STDIN_FILENO, &raw);

	raw.c_lflag &= ~(ECHO);

	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	
}

int main(){
	enableRaw();

	char c;
	while (read(STDIN_FILENO, &c, 1) == 1);
	return 0;
}
