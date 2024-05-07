
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
#include <sys/select.h>
#include <sys/ioctl.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <tigr.h>

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
	fd_set desc_in;
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
	term->tty.top_desc = posix_openpt(O_RDWR | O_NOCTTY);
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

// something in here errors out, to do
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

	FD_ZERO(&term->input.desc_in);
	FD_SET(term->tty.top_desc, &term->input.desc_in);
	if(
		select(

			// dont know why the addition is there
			term->tty.top_desc + 1,
			&term->input.desc_in,
			NULL,
			NULL,
			NULL
		) < 0
	) {
		deinit_term(term);
		return -1;
	}

	if(FD_ISSET(term->tty.top_desc, &term->input.desc_in)) {
		int read_result = read(
			term->tty.top_desc, 
			fetched_buf, 
			buf_size
		);
		
		if(read_result < 0) {

			// EIO happens when bottom file closes and is done writing
			// to the top file
			if(errno == EIO) {
				term->input.is_read_end = 1;
				return 1;
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
	}

	// for input, just get tigr keyboard input
	// and for encoded characters, send them (in a lookup table)
	// one by one, efficency does go down but its easier to make
	// a correct implementation this way
	return 0;
}

// may need to be reworked to have entire main loop inside here
// because of escape codes
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
	// to see reference for this implementation, some of the worst
	// code i have ever wrote
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
		// of the ansi parameters, and now its time to parse
		if(*index == old_index) {
			term->ansi.is_ansi_mode = 0;
			char first_param_repr = term->ansi.param_type_repr[0];
			int first_int_param = term->ansi.int_params[0];
			switch(first_param_repr) {
				case 'E':
				case 'A': {
					term->cursor_y += first_int_param == ANSI_INT_PARAM_SKIP 
						? 1 
						: first_int_param + 1;
					
					break;
				}

				case 'F':
				case 'B': {
					term->cursor_y -= first_int_param == ANSI_INT_PARAM_SKIP 
						? 1 
						: first_int_param + 1;
					
					break;
				}

				case 'C': {
					term->cursor_x += first_int_param == ANSI_INT_PARAM_SKIP 
						? 1 
						: first_int_param + 1;
					
					break;
				}

				case 'D': {
					term->cursor_x -= first_int_param == ANSI_INT_PARAM_SKIP 
						? 1 
						: first_int_param + 1;
					
					break;
				}

				case 'G': {
					term->cursor_x = first_int_param == ANSI_INT_PARAM_SKIP
						? 1
						: first_int_param + 1;

					break;
				}

				case 'K':
				case 'J': {
					term->ansi.current_color = term->ansi.default_color;
					int cursor_index = (
						(term->cursor_y * TERM_SIZE_X) + 
						term->cursor_x
					);

					int end_index = TERM_SIZE_X * TERM_SIZE_Y;
					int loop_start_index = 0;
					int loop_end_index = 0;
					switch(first_int_param) {
						case ANSI_INT_PARAM_SKIP:
						case 0: {
							loop_start_index = cursor_index;
							loop_end_index = end_index;
							break;
						}

						case 1: {
							loop_start_index = 0;
							loop_end_index = cursor_index;
							break;
						}

						case 2:

						// reduntant because we don't have scrolling
						case 3: 
						default: {
							loop_start_index = 0;
							loop_end_index = end_index;
							break;
						}
					}

					if(first_param_repr == 'K') {
						int line_start_index = term->cursor_y * TERM_SIZE_X;
						int line_end_index = (
							line_start_index + 
							(TERM_SIZE_X - 1)
						);
						
						switch(first_int_param) {
							case ANSI_INT_PARAM_SKIP:
							case 0: {
								loop_start_index = cursor_index;
								loop_end_index = line_end_index;
								break;
							}

							case 1: {
								loop_start_index = line_start_index;
								loop_end_index = cursor_index;
								break;
							}

							case 2:
							default: {
								loop_start_index = line_start_index;
								loop_end_index = line_end_index;
								break;
							}
						}
					}

					for(
						int i = loop_start_index;
						i < loop_end_index;
						i++
					) {
						term->ansi.color_image[i] = term->ansi.default_color;
						term->term_image[i] = ' ';
					}

					break;
				}

				case ';': {
					char second_param_repr = term->ansi.param_type_repr[1];
					int second_int_param = term->ansi.int_params[1];
					switch(second_param_repr) {
						case 'f':
						case 'H': {
							term->cursor_y = first_int_param == ANSI_INT_PARAM_SKIP
								? 1
								: first_int_param + 1;

							term->cursor_x = second_int_param == ANSI_INT_PARAM_SKIP
								? 1
								: second_int_param + 1;

							break;
						}

						case 'm': {

							// to be done
							break;	
						}
						
						case ';': {

							// this only happens with select graphics
							// rendition, to do	
							break;	
						}

						default: {
							break;
						}
					}
				}

				default: {
					break;
				}
			}
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

		case '\b': {
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
