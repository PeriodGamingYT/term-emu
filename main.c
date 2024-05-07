
// terminals on posix is pure insanity.
// i dont know why these version(?) macros are
// here, maybe for backwards compatability?
// probably monkey patching all the weird stuff nobody
// saw coming
#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <tigr.h>

#define ARRAY_SIZE(_array) \
	((int)(sizeof(_array) / sizeof((_array)[0])))

#define CHILD_PID 0
#define ERR_PID (pid_t)(-1)
#define SHELL_PATH "/bin/sh"

// i use the same buffer size as musl libc, i use
// that library a lot for reference on this project
#define TERM_BUF_SIZE 1024
#define FONT_SIZE_X 8
#define FONT_SIZE_Y 9
#define TERM_SIZE_X 80
#define TERM_SIZE_Y 24
#define TAB_SIZE 4
#define ANSI_INT_PARAM_MAX 16
#define ANSI_INT_PARAM_SKIP -1
typedef struct {
	TPixel color_image[TERM_SIZE_X * TERM_SIZE_Y];
	int int_params[ANSI_INT_PARAM_MAX];
	char param_type_repr[ANSI_INT_PARAM_MAX];
	Tigr *tigr;
	TPixel default_color;
	TPixel current_color;
	int is_ansi_mode;
	int int_param_index;
} ansi_t;

typedef struct {
	pid_t fork_pid;
	int top_desc;
	int bottom_desc;
} tty_t;

typedef struct {
	int input_ready;
	int is_read_end;
} input_t;

typedef struct {
	char term_image[TERM_SIZE_X * TERM_SIZE_Y];
	ansi_t ansi;
	tty_t tty;
	input_t input;
	int cursor_x;
	int cursor_y;
} term_t;

int bool_to_err(int bool) {
	return !!bool * -1;
}

int close_if_open(int desc) {
	if(desc <= 0) {
		return 0;
	}

	return close(desc);
}

int deinit_term(term_t *term) {
	return bool_to_err(
		term == NULL || 
		close_if_open(term->tty.top_desc) < 0 ||
		close_if_open(term->tty.bottom_desc) < 0
	);
}

int term_make_room(term_t *term) {
	if(term == NULL) {
		return -1;
	}

	// can be replaced with scrolling
	memset(
		term->term_image, 
		' ', 
		sizeof(term->term_image)
	);

	for(
		int i = 0; 
		i < TERM_SIZE_X * TERM_SIZE_Y; 
		i++
	) {
		term->ansi.color_image[i] = term->ansi.default_color;
	}

	term->cursor_x = 0;
	term->cursor_y = 0;
}

int init_term(term_t *term) {
	if(term == NULL) {
		return -1;
	}

	// opentty is far superior, but i use posix_openpt
	// and its friends for compatability
	term->tty.top_desc = posix_openpt(
		O_RDWR | 
		O_NOCTTY | 
		O_NONBLOCK
	);
	
	if(
		term->tty.top_desc < 0 ||
		grantpt(term->tty.top_desc) < 0 ||
		unlockpt(term->tty.top_desc) < 0
	) {
		deinit_term(term);
		return -1;
	}

	// in seperate variable to not have open happen
	// before ptsname, not sure if this actually happens
	// but its possible
 	char *bottom_name = ptsname(term->tty.top_desc);
 	if(bottom_name == NULL) {
 		deinit_term(term);
 		return -1;	
 	}
 	
	term->tty.bottom_desc = open(
		bottom_name, 
		O_RDWR
	);

	if(term->tty.bottom_desc < 0) {
		deinit_term(term);
		return -1;
	}

	term->tty.fork_pid = fork();
	if(term->tty.fork_pid == ERR_PID) {
		deinit_term(term);
		return -1;
	}

	if(term->tty.fork_pid == CHILD_PID) {
		struct termios old_term = { 0 };
		struct termios new_term = { 0 };
		if(
			close(term->tty.top_desc) < 0 ||
			tcgetattr(term->tty.bottom_desc, &old_term) < 0
		) {
			deinit_term(term);
			return -1;
		}

		new_term = old_term;
		new_term.c_iflag &= ~(
			IGNBRK |
			BRKINT |
			PARMRK |
			ISTRIP |
			INLCR |
			IGNCR |
			ICRNL |
			IXON
		);
		
		new_term.c_oflag &= ~OPOST;
		new_term.c_lflag &= ~(
			ECHO |
			ECHONL |
			ICANON |
			ISIG |
			IEXTEN
		);
		
		new_term.c_cflag &= ~(
			CSIZE |
			PARENB
		);
		
		new_term.c_cflag |= CS8;
		new_term.c_cc[VMIN] = 1;
		new_term.c_cc[VTIME] = 0;
		if(
			tcsetattr(term->tty.bottom_desc, TCSANOW, &new_term) < 0 ||
			close(STDIN_FILENO) < 0 ||
			close(STDOUT_FILENO) < 0 ||
			close(STDERR_FILENO) < 0 ||

			// done three seperate times for
			// stdin, stdout, and stderr in that order
			dup2(term->tty.bottom_desc, STDIN_FILENO) < 0 ||
			dup2(term->tty.bottom_desc, STDOUT_FILENO) < 0 ||
			dup2(term->tty.bottom_desc, STDERR_FILENO) < 0 ||
			close(term->tty.bottom_desc) < 0 ||
			setsid() == ERR_PID ||
			ioctl(0, TIOCSCTTY, 1) < 0
		) {
			deinit_term(term);
			return -1;
		}

		return 0;
 	}

 	if(close(term->tty.bottom_desc) < 0) {
 		return -1;
 	}

	term->ansi.default_color = tigrRGB(240, 240, 240);
	term->ansi.current_color = term->ansi.default_color;
 	return 0;
}

int term_fetch(
	term_t *term, 
	char *fetched_buf,
	int buf_size
) {
	if(
		term == NULL || 
		fetched_buf == NULL ||
		buf_size < 0 ||
		term->tty.fork_pid == ERR_PID || 
		term->tty.fork_pid == CHILD_PID
	) {
		deinit_term(term);
		return -1;
	}

	if(term->input.is_read_end || buf_size == 0) {
		return 0;
	}
	
	int read_result = read(
		term->tty.top_desc, 
		fetched_buf, 
		buf_size
	);
	
	if(read_result < 0) {

		// eio happens when bottom file closes and is done writing
		// to the top file
		if(errno == EIO) {
			term->input.is_read_end = 1;
			return 1;
		}

		if(errno == EWOULDBLOCK) {
			return 0;
		}
		
		deinit_term(term);
		return -1;
	}

	// if read_result is zero that means the program
	// is done sending input
	if(read_result == 0) {
		term->input.is_read_end = 1;
		return 1;
	}

	// for input, just get tigr keyboard input
	// and for encoded characters, send them (in a lookup table)
	// one by one, efficency does go down but its easier to make
	// a correct implementation this way
	return 0;
}

int term_feed_char(term_t *term, char feed_char) {
	if(term == NULL) {
		return -1;
	}

	int index = (term->cursor_y * TERM_SIZE_X) + term->cursor_x;
	if(
		index < 0 || 
		index >= TERM_SIZE_X * TERM_SIZE_Y
	) {

		// cant fail because term is not null at this point
		term_make_room(term);
	}

	// see https://en.wikipedia.org/wiki/ANSI_escape_code
	// to see reference for this implementation
	if(term->ansi.is_ansi_mode) {

		// this is a hack that takes advantage of how ifs in c work.
		// 2 is true, so we use it here to make sure we have the '['
		// in the right place
		if(feed_char == '[' && term->ansi.is_ansi_mode == 2) {
			term->ansi.is_ansi_mode = 1;
			return 0;
		}
		
		int *index = &term->ansi.int_param_index;
		int old_index = *index;
		int *current_int = &term->ansi.int_params[*index];
		if(isdigit(feed_char)) {
			*current_int *= 10;
			*current_int += feed_char - '0';
			return 0;
		} else {
			term->ansi.param_type_repr[*index] = feed_char;

			// if a number is met by a semicolon, it means
			// that there are more numbers to come. a question mark
			// is also going to continue the line because in ansi
			// a question will always by follows by a number, but for 
			// a question mark, its not preceded by a number
			if(feed_char == ';' || feed_char == '?') {
				(*index)++;
			}
		}

		// if the index didn't change, that means we hit the end
		// of the ansi parameters, and go back to normal, ansi
		// escape codes are a horrible nightmare and i don't want them
		// in my code
		if(*index == old_index) {
			term->ansi.is_ansi_mode = 0;
		}

		return 0;
	}

	switch(feed_char) {

		// null character, we reached the end of the buffer,
		// so don't read further
		case 0: {
			return 0;
		}

		// copy behavior of st, one my reference
		// implementations
		case '\f':
		case '\v':
		case '\n': {
			term->cursor_x = 0;
			term->cursor_y++;
			break;
		}

		case 127:
		case '\b': {

			// get rid of some characters for the sake of backspace working
			// as it should, since input is also put into here
			term->ansi.color_image[index] = term->ansi.default_color;
			term->term_image[index] = ' ';
			if(term->cursor_x > 0) {
				term->cursor_x--;
			}

			break;
		}

		case '\r': {
			term->cursor_x = 0;
			break;
		}

		case '\t': {
			term->cursor_x += TAB_SIZE;
			break;
		}

		case '\x1b': {

			// this two is intentional, it's a trick
			// to make sure '[' is consumed
			term->ansi.is_ansi_mode = 2;
			break;
		}

		default: {
			term->ansi.color_image[index] = term->ansi.current_color;
			term->term_image[index] = isprint(feed_char)
				? feed_char
				: '?';

			term->cursor_x++;
			break;
		}
	}

	if(term->cursor_x >= TERM_SIZE_X) {
		term->cursor_x = 0;
		term->cursor_y++;
	}

	if(term->cursor_y >= TERM_SIZE_Y) {

		// cant fail because term is not null at this point
		term_make_room(term);
	}

	return 0;
}

int term_write_input(term_t *term, char *buf, int size) {
	if(
		term == NULL ||
		buf == NULL ||
		size < 0
	) {
		return -1;
	}

	for(int i = 0; buf[i] != 0 && i < size; i++) {
		if(
			write(
				term->tty.top_desc, 
				&buf[i], 
				sizeof(char)
			) < 0
		) {
			return -1;
		}
		
		if(term_feed_char(term, buf[i]) < 0) {
			return -1;
		}
	}

	return 0;
}

int main(int argc, char **argv, char **envp) {
	term_t term = { 0 };
	setvbuf(stdout, NULL, _IONBF, 0);
	if(init_term(&term) < 0) {
		return 1;
	}

	if(term.tty.fork_pid == CHILD_PID) {
		char **child_argv = malloc(argc * sizeof(char *));
		if(child_argv == NULL) {
			deinit_term(&term);
			return 1;
		}
		
		for(int i = 1; i < argc; i++) {
			child_argv[i - 1] = strdup(argv[i]);
		}

		child_argv[argc - 1] = NULL;
		if(execvp(SHELL_PATH, argv) < 0) {
			for(int i = 0; i < argc - 1; i++) {
				free(child_argv[i]);
			}
			
			free(child_argv);
			deinit_term(&term);
			return 1;
		}

		for(int i = 0; i < argc - 1; i++) {
			free(child_argv[i]);
		}
		
		free(child_argv);
		deinit_term(&term);
		return 0;
	}
	
	Tigr *tigr = tigrWindow(
		TERM_SIZE_X * FONT_SIZE_X, 
		TERM_SIZE_Y * FONT_SIZE_Y, 
		"term-emu", 
		0
	);

	term.ansi.tigr = tigr;
	while(!tigrClosed(tigr)) {
		int last_char = tigrReadChar(tigr);
printf("last char is '%c' on line %d\n", last_char, __LINE__);
		if(last_char != 0) {
			if(

				// don't check uppercase to allow implementation
				// of ctrl+shift+c and ctrl+shift+v or copying/pasting
				last_char >= 'a' && 
				last_char <= 'z' &&
				tigrKeyHeld(tigr, TK_CONTROL)
			) {

				// trick to get control-key versions of ascii
				// characters
				last_char &= 0x1f;
			}



			if(
				last_char >= ' ' &&
				last_char <= '~' &&
				term_write_input(
					&term, 
					(char *)(&last_char), 
					sizeof(char)
				) < 0
			) {
				tigrFree(tigr);
				deinit_term(&term);
				return 1;
			}
		}

		// this is just awful, the pure, plain essence of awfulness.
		// my hope is that this code is so bad im never allowed to write
		// a terminal emulator ever again
		char key_to_term[][5] = {
			{ '0', 0, 0, 0, 0 },
			{ '1', 0, 0, 0, 0 },
			{ '2', 0, 0, 0, 0 },
			{ '3', 0, 0, 0, 0 },
			{ '4', 0, 0, 0, 0 },
			{ '5', 0, 0, 0, 0 },
			{ '6', 0, 0, 0, 0 },
			{ '7', 0, 0, 0, 0 },
			{ '8', 0, 0, 0, 0 },
			{ '9', 0, 0, 0, 0 },
			{ '*', 0, 0, 0, 0 },
			{ '+', 0, 0, 0, 0 },
			{ '\n', 0, 0, 0, 0 },
			{ '-', 0, 0, 0, 0 },
			{ '.', 0, 0, 0, 0 }, 
			{ '/', 0, 0, 0, 0 },
			{ 27, 79, 80, 0, 0 },
			{ 27, 79, 81, 0, 0 },
			{ 27, 79, 82, 0, 0 },
			{ 27, 79, 83, 0, 0 },
			{ 27, 91, 49, 53, 126 },
			{ 27, 91, 49, 55, 126 },
			{ 27, 91, 49, 56, 126 },
			{ 27, 91, 49, 57, 126 },
			{ 27, 91, 50, 48, 126 },
			{ 27, 91, 50, 49, 126 },
			{ 27, 91, 50, 51, 126 },
			{ 27, 91, 50, 52, 126 },
			{ 127, 0, 0, 0, 0 }, 
			{ 9, 0, 0, 0, 0 }, 
			{ 13, 0, 0, 0, 0 }, 
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 27, 0, 0, 0, 0 }, 
			{ ' ', 0, 0, 0, 0 },
			{ 27, 91, 53, 126, 0 }, 
			{ 27, 91, 54, 126, 0 }, 
			{ 27, 91, 52, 126, 0 },
			{ 27, 91, 72, 0, 0 }, 
			{ 27, 91, 68, 0, 0 }, 
			{ 27, 91, 65, 0, 0 }, 
			{ 27, 91, 67, 0, 0 },
			{ 27, 91, 66, 0, 0 }, 
			{ 27, 91, 52, 104, 0 }, 
			{ 27, 91, 80, 0, 0 }, 
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 },
			{ 0, 0, 0, 0, 0 }, 
			{ ';', 0, 0, 0, 0 },
			{ '=', 0, 0, 0, 0 },
			{ ',', 0, 0, 0, 0 },
			{ '-', 0, 0, 0, 0 },
			{ '.', 0, 0, 0, 0 },
			{ '/', 0, 0, 0, 0 },
			{ '`', 0, 0, 0, 0 },
			{ '[', 0, 0, 0, 0 },
			{ '\\', 0, 0, 0, 0 },
			{ ']', 0, 0, 0, 0 },
			{ '\'', 0, 0, 0, 0 }
		};

		int index = last_char - 128;
		for(int i = 0; i < ARRAY_SIZE(key_to_term); i++) {
if(tigrKeyDown(tigr, i + 128)){printf("key was down on line %d!\n",__LINE__);}
			if(
				tigrKeyDown(tigr, i + 128) &&
				term_write_input(
					&term,
					key_to_term[i],
					sizeof(int[5])
				) < 0
			) {
				tigrFree(tigr);
				deinit_term(&term);
				return 1;
			}
		}
		
		char fetched_buf[TERM_BUF_SIZE] = { 0 };
		int fetch_result = term_fetch(
			&term, 
			fetched_buf,
			TERM_BUF_SIZE
		);
		
		if(fetch_result < 0) {
			tigrFree(tigr);
			deinit_term(&term);
			return 1;
		}
		
		for(
			int i = 0; 
			i < TERM_BUF_SIZE &&
			fetched_buf[i] != 0;
			i++
		) {
			switch(fetched_buf[i]) {
				case '\a': {
					tigrClear(tigr, tigrRGB(255, 255, 255));
					tigrUpdate(tigr);
					break;
				}
				
				default: {
					if(term_feed_char(&term, fetched_buf[i]) < 0) {
						tigrFree(tigr);
						deinit_term(&term);
						return 1;
					}

					break;
				}
			}
		}

		tigrClear(tigr, tigrRGB(24, 24, 24));
		for(int y = 0; y < TERM_SIZE_Y; y++) {
			for(int x = 0; x < TERM_SIZE_X; x++) {
				int index = (y * TERM_SIZE_X) + x;

				// wrapped in this print_str to prevent string overflow
				char print_str[2] = { term.term_image[index], 0 };
				tigrPrint(
					tigr,
					tfont,
					x * FONT_SIZE_X,
					y * FONT_SIZE_Y,
					term.ansi.color_image[index],
					print_str
				);
			}
		}

		tigrUpdate(tigr);
	}

	tigrFree(tigr);
	deinit_term(&term);
	return 0;
}
