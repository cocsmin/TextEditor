
//includes

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

//defines

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}

//data
struct editorConfig{
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig Edit;

//terminal

void die(const char* s){
	write(STDOUT_FILENO, "\x1b[2j", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	perror(s);
	exit(1);
	
}

void disableRaw(){
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &Edit.orig_termios) == -1)
		die("tcsetattr");
}

void enableRaw(){
	if(tcgetattr(STDIN_FILENO, &Edit.orig_termios) == -1)
		die("tcgetattr");

	atexit(disableRaw);

	struct termios raw = Edit.orig_termios;

	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);

	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
	
}

char editorReadKey(){
	int nread;
	char c;
	while (1){
		nread = read(STDIN_FILENO, &c, 1);
		if (nread == -1 && errno != EAGAIN)
			die("read");
		else if (nread == 1)
			return c;
	}
}

int getCursorPosition(int *rows, int *cols){

	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;
	while (i < sizeof(buf) - 1){
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') 
		return -1;

	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;
	
	return 0;
}

int getWindowSize(int* rows, int* cols){
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	}	
	else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

//append buffer

struct abuf {
	char *b;
	int len;
};

void abAppend(struct abuf* ab, const char* s, int len){

	char* new = realloc(ab->b, ab->len + len);

	if (new == NULL)
		return;

	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf* ab){
	free(ab->b);
}


//output
void editorDrawRows(){
	int i;
	for (i = 0; i < Edit.screenrows; i++){
		write(STDOUT_FILENO, "~", 1);

		if (i < Edit.screenrows - 1){
			write(STDOUT_FILENO, "\r\n", 2);
		}
	}
}

void editorRefreshScreen(){
	write(STDOUT_FILENO, "\x1b[2j", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	editorDrawRows();

	write(STDOUT_FILENO, "\x1b[H", 3);
}



//input

void editorProcessKeypress(){
	char c = editorReadKey();

	if (c == 'q'){
		write(STDOUT_FILENO, "\x1b[2j", 4);
		write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);
	}

	//printf("%c\r\n", c);

}

//init

void initEditor(){
	if (getWindowSize(&Edit.screenrows, &Edit.screencols) == -1)
		die("getWindowSize");
}

int main(){
	enableRaw();
	initEditor();
	while (1){
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
