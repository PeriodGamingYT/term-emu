
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

#define TIGR_GAPI_GL
#include <tigr.h>

#define CHILD_PID 0
#define ERR_PID (pid_t)(-1)
#define TERM_BUF_MAX 512
#define INPUT_MAX 128
#define SHELL_PATH "/bin/sh"
#define FONT_SIZE_X 8
#define FONT_SIZE_Y 9
#define TERM_SIZE_X 80
#define TERM_SIZE_Y 24
#define TAB_SIZE 4
typedef struct {
	char term_image[TERM_SIZE_X * TERM_SIZE_Y];
	char buf[TERM_BUF_MAX];
	char input[INPUT_MAX];
	pid_t fork_pid;
	fd_set desc_in;
	int input_ready;
	int is_prog_end;
	int top_desc;
	int bottom_desc;
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
		close_if_open(term->top_desc) < 0 ||
		close_if_open(term->bottom_desc) < 0
	);
}

int init_term(term_t *term) {
	if(term == NULL) {
		return -1;
	}

	// opentty is far superior, but i use posix_openpt
	// and its friends for compatability
	term->top_desc = posix_openpt(O_RDWR | O_NOCTTY);
	if(
		term->top_desc < 0 ||
		grantpt(term->top_desc) < 0 ||
		unlockpt(term->top_desc) < 0
	) {
		deinit_term(term);
		return -1;
	}
 
	term->bottom_desc = open(
		ptsname(term->top_desc), 
		O_RDWR
	);

	if(term->bottom_desc < 0) {
		deinit_term(term);
		return -1;
	}

	term->fork_pid = fork();
	if(term->fork_pid == ERR_PID) {
		deinit_term(term);
		return -1;
	}

	if(term->fork_pid == CHILD_PID) {
		struct termios old_term = { 0 };
		struct termios new_term = { 0 };
		if(
			close(term->top_desc) < 0 ||
			tcgetattr(term->bottom_desc, &old_term) < 0
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
			tcsetattr(term->bottom_desc, TCSANOW, &new_term) < 0 ||
			close(STDIN_FILENO) < 0 ||
			close(STDOUT_FILENO) < 0 ||
			close(STDERR_FILENO) < 0 ||

			// done three seperate times for
			// stdin, stdout, and stderr in that order
			dup2(term->bottom_desc, STDIN_FILENO) < 0 ||
			dup2(term->bottom_desc, STDOUT_FILENO) < 0 ||
			dup2(term->bottom_desc, STDERR_FILENO) < 0 ||
			close(term->bottom_desc) < 0 ||
			setsid() == ERR_PID ||
			ioctl(0, TIOCSCTTY, 1) < 0
		) {
			deinit_term(term);
			return -1;
		}

		return 0;
 	}

 	if(close(term->bottom_desc) < 0) {
 		return -1;
 	}

 	return 0;
}

// something in here errors out, to do
int term_fetch(term_t *term) {
	if(
		term == NULL || 
		term->fork_pid == ERR_PID || 
		term->fork_pid == CHILD_PID
	) {
		deinit_term(term);
		return -1;
	}

	if(term->is_prog_end) {
		return 0;
	}

	FD_ZERO(&term->desc_in);
	FD_SET(0, &term->desc_in);
	FD_SET(term->top_desc, &term->desc_in);
	if(
		select(

			// dont know why the addition is there
			term->top_desc + 1,
			&term->desc_in,
			NULL,
			NULL,
			NULL
		) < 0
	) {
		deinit_term(term);
		return -1;
	}

	if(FD_ISSET(term->top_desc, &term->desc_in)) {
		if(
			read(
				term->top_desc, 
				term->buf, 
				sizeof(term->buf)
			) < 0
		) {
			deinit_term(term);
			return -1;
		}
	}

	if(term->input_ready) {
		term->input_ready = 0;
		if(
			write(
				term->top_desc,
				term->input,
				sizeof(term->input)
			) < 0
		) {
			deinit_term(term);
			return -1;
		}

		memset(term->input, 0, sizeof(term->input));
	}

	memset(term->buf, 0, sizeof(term->buf));
	int read_result = read(
		term->top_desc, 
		term->buf, 
		sizeof(term->buf)
	);
	
	if(read_result < 0) {
		deinit_term(term);
		return -1;
	}

	// if read_result is zero that means the program
	// is done sending input
	if(read_result == 0) {
		term->is_prog_end = 1;
		return 1;
	}

	return 0;
}

int term_buf_clear(term_t *term) {
	if(term == NULL) {
		return -1;
	}

	memset(term->buf, 0, sizeof(term->buf));
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

	term->cursor_x = 0;
	term->cursor_y = 0;
}

// may need to be reworked to have entire main loop inside here
// because of escape codes
int term_feed_char(term_t *term, char feed_char) {
printf("is term null on line %d?\n", __LINE__);
	if(term == NULL) {
		return -1;
	}

	int index = (term->cursor_y * TERM_SIZE_X) + term->cursor_x;
printf("index is %d and term size x * y is %d on line %d\n", index, TERM_SIZE_X * TERM_SIZE_Y, __LINE__);
	if(
		index < 0 || 
		index >= TERM_SIZE_X * TERM_SIZE_Y
	) {

		// can't fail because term is not null at this point
		term_make_room(term);
	}

	switch(feed_char) {
		case '\n':

			// trick because later on in this function
			// we increment cursor x by 1, thereby setting it
			// to zero
			term->cursor_x = -1;
			term->cursor_y++;
			break;

		case '\r':
			
			// trick because later on in this function
			// we increment cursor x by 1, thereby setting it
			// to zero
			term->cursor_x = -1;
			break;

		case '\t':
			term->cursor_x += TAB_SIZE;
			break;

		default:
			term->term_image[index] = isprint(feed_char)
				? feed_char
				: '?';

			break;
	}

	term->cursor_x++;
	if(term->cursor_x >= TERM_SIZE_X) {
		term->cursor_x = 0;
		term->cursor_y++;
	}

	if(term->cursor_y >= TERM_SIZE_Y) {

		// can't fail because term is not null at this point
		term_make_room(term);
	}
}

int main(int argc, char **argv, char **envp) {
	term_t term = { 0 };
	setvbuf(stdout, NULL, _IONBF, 0);
	if(init_term(&term) < 0) {
		return 1;
	}

	if(term.fork_pid == CHILD_PID) {
		char **child_argv = malloc(argc * sizeof(char *));
		if(child_argv == NULL) {
			deinit_term(&term);
			return 1;
		}
		
		for(int i = 1; i < argc; i++) {
			child_argv[i - 1] = strdup(argv[i]);
		}

		child_argv[argc - 1] = NULL;
		//if(execvp(SHELL_PATH, argv) < 0) {
		if(execvp(child_argv[0], child_argv) < 0) {
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
	
	while(!tigrClosed(tigr)) {
		tigrClear(tigr, tigrRGB(24, 24, 24));
		for(int y = 0; y < TERM_SIZE_Y; y++) {
			int line_index = y * TERM_SIZE_X;
			char *replace_char = &term.term_image[
				line_index + (TERM_SIZE_X - 1)
			];

			char temp_char = *replace_char;
			*replace_char = 0;
			tigrPrint(
				tigr,
				tfont,
				0,
				y * FONT_SIZE_Y,
				tigrRGB(240, 240, 240),
				&term.term_image[line_index]
			);

			*replace_char = temp_char;
		}

		int fetch_result = term_fetch(&term);
		if(fetch_result < 0) {
printf("fetch result is less than 0 on line %d\n", __LINE__);
			tigrFree(tigr);
			deinit_term(&term);
			return 1;
		}
printf("we didn't have fetch_result < 0 on line %d\n", __LINE__);

		char *buf = term.buf;
printf("buf is %s on line %d\n", buf, __LINE__);
		for(
			int i = 0; 
			buf[i] != 0 && 
			fetch_result > 0 &&
			i < TERM_BUF_MAX; 
			i++
		) {
			if(term_feed_char(&term, buf[i]) < 0) {
				tigrFree(tigr);
				deinit_term(&term);
printf("term feed char failed on line %d\n", __LINE__);
				return 1;
			}
		}

printf("tigr is updating on line %d\n", __LINE__);
		tigrUpdate(tigr);
printf("done updating on line %d\n", __LINE__);
	}

	tigrFree(tigr);
	deinit_term(&term);
	return 0;
}
