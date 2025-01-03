
//includes

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

//defines

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUF_INIT {NULL, 0}
#define EDITOR_VERSION "0.0.1"
#define TAB_STOP 8
#define QUIT_TIMES 3

enum editorKey{
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY, // fn + backspace
	HOME_KEY, // fn + arrow_left 
	END_KEY, // fn + arrow_right
	PAGE_UP,
	PAGE_DOWN
};

//data

typedef struct erow {
	int size;
	int rsize;
	char* chars;
	char* render;
} erow;

struct editorConfig{
	int cx;
	int cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow* row;
	int differ;
	char* filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char* prompt);

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

int editorReadKey(){
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}
	if (c == '\x1b'){
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
			
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';

		if (seq[0] == '['){
			if (seq[1] >= '0' && seq[1] <= '9'){
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~'){
					switch (seq[1]){
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}
			else{
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O'){
			switch (seq[1]){
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		
		return '\x1b';
	}
	else {
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



// operations with rows

int editorRowCxToRx(erow* row, int cx){
	int rx = 0;
	for (int j = 0; j < cx; j++){
		if (row->chars[j] == '\t')
			rx += (TAB_STOP - 1) - (rx % TAB_STOP);
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow *row){
	int tabs = 0;
	for (int j = 0; j < row->size; j++)
		if (row->chars[j] == '\t')
			tabs++;
	free(row->render); 
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

	int idx = 0;
	for (int j = 0; j < row->size; j++){
		if (row->chars[j] == '\t'){
			row->render[idx++] = ' ';
			while(idx % TAB_STOP != 0)
				row->render[idx++] = ' ';
		}
		else 
			row->render[idx++] = row->chars[j];
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorInsertRow(int at, char* s, size_t len){
	Edit.row = realloc(Edit.row, sizeof(erow) * (Edit.numrows + 1));

	Edit.row[at].size = len;
	Edit.row[at].chars = malloc(len + 1);
	memcpy(Edit.row[at].chars, s, len);
	Edit.row[at].chars[len] = '\0';

	Edit.row[at].rsize = 0;
	Edit.row[at].render = NULL;
	editorUpdateRow(&Edit.row[at]);
	
	Edit.numrows++;
	Edit.differ++;
}

void editorRowInsertChar(erow* row, int at, int c){
	if (at < 0 || at > row->size)
		at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	Edit.differ++;
}

void editorRowAppendString(erow* row, char* s, size_t len){
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editorUpdateRow(row);
	Edit.differ++;
}

void editorRowDelChar(erow* row, int at){
	if (at < 0 || at >= row->size)
		return;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	Edit.differ++;
}

void editorFreeRow(erow* row){
	free(row->render);
	free(row->chars);
}

void editorDelRow(int at){
	if (at < 0 || at >= Edit.numrows)
		return;
	editorFreeRow(&Edit.row[at]);
	memmove(&Edit.row[at], &Edit.row[at + 1], sizeof(erow) * (Edit.numrows - at - 1));
	Edit.numrows--;
	Edit.differ++;
}

//editor operations

void editorInsertChar(int c){
	if (Edit.cy == Edit.numrows)
		editorInsertRow(Edit.numrows, "", 0);

	editorRowInsertChar(&Edit.row[Edit.cy], Edit.cx, c);
	Edit.cx++;
}

void editorDelChar(){
	if (Edit.cy == Edit.numrows)
		return;

	if (Edit.cx == 0 && Edit.cy == 0)
		return;

	erow* row = &Edit.row[Edit.cy];
	if (Edit.cx > 0){
		editorRowDelChar(row, Edit.cx - 1);
		Edit.cx--;
	}
	else {
		Edit.cx = Edit.row[Edit.cy - 1].size;
		editorRowAppendString(&Edit.row[Edit.cy - 1], row->chars, row->size);
		editorDelRow(Edit.cy);
		Edit.cy--;
	}
}

void editorInsertNewline(){
	if (Edit.cx == 0)
		editorInsertRow(Edit.cy, "", 0);
	else{
		erow* row = &Edit.row[Edit.cy];
		editorInsertRow(Edit.cy + 1, &row->chars[Edit.cx], row->size - Edit.cx);
		row = &Edit.row[Edit.cy];
		row->size = Edit.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	Edit.cy++;
	Edit.cx = 0;

}

// file I/O

void editorOpen(char* filename){

	free(Edit.filename);
	Edit.filename = strdup(filename);

	FILE* fd = fopen(filename, "r");
	if (!fd)
		die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while((linelen = getline(&line, &linecap, fd)) != -1){
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		editorInsertRow(Edit.numrows, line, linelen);
	
	}
	free(line);
	fclose(fd);
	Edit.differ = 0;
	
}

char* editorRowsToString(int* buflen){
	int totlen = 0;
	for (int j = 0; j < Edit.numrows; j++)
		totlen += Edit.row[j].size + 1;
	*buflen = totlen;

	char* buf = malloc(totlen);
	char* p = buf;
	for (int j = 0; j < Edit.numrows; j++){
		memcpy(p, Edit.row[j].chars, Edit.row[j].size);
		p += Edit.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}


void editorSave(){
	if (Edit.filename == NULL){
		Edit.filename = editorPrompt("Save as: %s (ESC to cancel)");
		if (Edit.filename == NULL){
			editorSetStatusMessage("Save canceled");
			return;
		}
	}
	int len;
	char* buf = editorRowsToString(&len);

	int fd = open(Edit.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1){
		if (ftruncate(fd, len) != -1){
			if (write(fd, buf, len) == len){
				close(fd);
				free(buf);
				Edit.differ = 0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't save!! I/O error: %s", strerror(errno));
}


//find

void editorFind(){
	char* query = editorPrompt("Search: %s (ESC to cancel)");
	if (query == NULL)
		return;

	//TODO
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

void editorDrawMessageBar(struct abuf* ab){
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(Edit.statusmsg);
	if (msglen > Edit.screencols)
		msglen = Edit.screencols;
	if (msglen && time(NULL) - Edit.statusmsg_time < 5)
		abAppend(ab, Edit.statusmsg, msglen);
}

void editorSetStatusMessage(const char* fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(Edit.statusmsg, sizeof(Edit.statusmsg), fmt , ap);
	va_end(ap);
	Edit.statusmsg_time = time(NULL);
}

void editorDrawStatusBar(struct abuf* ab){
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
	Edit.filename ? Edit.filename : "[No Name]", Edit.numrows,
	Edit.differ ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", Edit.cy + 1, Edit.numrows);
	if (len > Edit.screencols) 
		len = Edit.screencols;
	abAppend(ab, status, len);	

	while (len < Edit.screencols){
		if (Edit.screencols - len == rlen){
			abAppend(ab, rstatus, rlen);
			break;
		}
		else {
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorScroll(){

	Edit.rx = 0;
	if (Edit.cy < Edit.numrows)
		Edit.rx = editorRowCxToRx(&Edit.row[Edit.cy], Edit.cx);

	if (Edit.cy < Edit.rowoff)
		Edit.rowoff = Edit.cy;

	if (Edit.cy >= Edit.rowoff + Edit.screenrows)
		Edit.rowoff = Edit.cy - Edit.screenrows + 1;

	if (Edit.rx < Edit.coloff)
		Edit.coloff = Edit.rx;

	if (Edit.rx >= Edit.coloff + Edit.screencols)
		Edit.coloff = Edit.rx - Edit.screencols + 1;
}

void editorDrawRows(struct abuf* ab){
	int i;
	for (i = 0; i < Edit.screenrows; i++){
		int filerow = i + Edit.rowoff;
		if (filerow >= Edit.numrows){
			if (Edit.numrows == 0 && i == Edit.screenrows / 3){
				char welcome[100];
				int welcomelen = snprintf(welcome, sizeof(welcome),
				"Cocsmin editor -- version %s", EDITOR_VERSION);
				if (welcomelen > Edit.screencols)
					welcomelen = Edit.screencols;

				int padding = (Edit.screencols - welcomelen) / 2;
				if (padding){
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--)
					abAppend(ab, " ", 1);
					
				abAppend(ab, welcome, welcomelen);
			}
			else{	
				abAppend(ab, "~", 1);
			}
		}
		else{
			int len = Edit.row[filerow].rsize - Edit.coloff;
			if (len < 0)
				len = 0;
			if (len > Edit.screencols)
				len = Edit.screencols;
			abAppend(ab, &Edit.row[filerow].render[Edit.coloff], len);
		}

		
		abAppend(ab, "\x1b[K", 3);

		abAppend(ab, "\r\n", 2);
	}
}

void editorRefreshScreen(){

	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (Edit.cy - Edit.rowoff) + 1, (Edit.rx - Edit.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}



//input

char* editorPrompt(char* prompt){
	size_t bufsize = 128;
	char* buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1){
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
			if (buflen != 0)
				buf[--buflen] = '\0';
		}
		else if (c == '\x1b'){
			editorSetStatusMessage("");
			free(buf);
			return NULL;
		}
		else if (c == '\r'){
			if (buflen != 0){
				editorSetStatusMessage("");
				return buf;
			}
		}
		else if (!iscntrl(c) && c < 128){
			if (buflen == bufsize - 1){
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
	}
}

void editorMoveCursor(int key){

	erow *row = (Edit.cy >= Edit.numrows) ? NULL : &Edit.row[Edit.cy];

	switch (key){
		case ARROW_LEFT:
			if (Edit.cx != 0)
				Edit.cx--;
			else if (Edit.cy > 0){
				Edit.cy--;
				Edit.cx = Edit.row[Edit.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && Edit.cx < row->size)
				Edit.cx++;
			else if (row && Edit.cx == row->size){
				Edit.cy++;
				Edit.cx = 0;
			}
			break;
		case ARROW_UP:
			if (Edit.cy != 0)
				Edit.cy--;
			break;
		case ARROW_DOWN:
			if (Edit.cy < Edit.numrows)
				Edit.cy++;
			break;
	}

	row = (Edit.cy >= Edit.numrows) ? NULL : &Edit.row[Edit.cy];
	int rowlen = row ? row->size : 0;
	if (Edit.cx > rowlen)
		Edit.cx = rowlen;
}

void editorProcessKeypress(){
	int c = editorReadKey();

	static int quit_times = QUIT_TIMES;

	switch(c){

		case '\r':
			editorInsertNewline();
			break;
	
		case CTRL_KEY('q'):
			if (Edit.differ && quit_times > 0){
				editorSetStatusMessage("WARNING!!! File has unsaved changes. Press Ctrl+Q %d more times to quit without saving.", quit_times);
				quit_times--;
				return;	
			}
			write(STDOUT_FILENO, "\x1b[2j", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case CTRL_KEY('s'):
			editorSave();
			break;

		case HOME_KEY:
			Edit.cx = 0;
			break;

		case END_KEY:
			if (Edit.cy < Edit.numrows)
				Edit.cx = Edit.row[Edit.cy].size;
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY)
				editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP)
					Edit.cy = Edit.rowoff;
				else if (c == PAGE_DOWN){
					Edit.cy = Edit.rowoff + Edit.screenrows - 1;	
					if (Edit.cy > Edit.numrows)
						Edit.cy = Edit.numrows;
				}

				int times = Edit.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);	
			}
			break;
			
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editorInsertChar(c);
			break;
	}
	quit_times = QUIT_TIMES;
}

//init

void initEditor(){
	Edit.cx = 0;
	Edit.cy = 0;
	Edit.rx = 0;
	Edit.rowoff = 0;
	Edit.coloff = 0;
	Edit.numrows = 0;
	Edit.row = NULL;
	Edit.differ = 0;
	Edit.filename = NULL;
	Edit.statusmsg[0] = '\0';
	Edit.statusmsg_time = 0;
	
	if (getWindowSize(&Edit.screenrows, &Edit.screencols) == -1)
		die("getWindowSize");

	Edit.screenrows -= 2;
}

int main(int argc, char* argv[]){
	enableRaw();
	initEditor();
	if (argc >= 2)
		editorOpen(argv[1]);

	editorSetStatusMessage("HELP: Ctrl+Q = quit | Ctrl+S = save");
	
	while (1){
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
