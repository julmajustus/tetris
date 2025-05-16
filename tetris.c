/* Micro Tetris, based on an obfuscated tetris, 1989 IOCCC Best Game
 *
 * Copyright (c) 1989  John Tromp <john.tromp@gmail.com>
 * Copyright (c) 2009-2021  Joachim Wiberg <troglobit@gmail.com>
 * Copyright (c) 2025  julmajustus <julmajustus@tutanota.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * See the following URLs for more information, first John Tromp's page about
 * the game http://homepages.cwi.nl/~tromp/tetris.html then there's the entry
 * page at IOCCC http://www.ioccc.org/1989/tromp.hint
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define clrscr()       puts ("\033[2J\033[1;1H")
#define gotoxy(x,y)    printf("\033[%d;%dH", y, x)
#define hidecursor()   puts ("\033[?25l")
#define showcursor()   puts ("\033[?25h")
#define bgcolor(c,s)   printf("\033[%dm" s, c ? c + 40 : 0)

#define SIGNAL(signo, cb)			\
	sigemptyset(&sa.sa_mask);		\
	sigaddset(&sa.sa_mask, signo);		\
	sa.sa_flags = 0;			\
	sa.sa_handler = cb;			\
	sigaction(signo, &sa, NULL)

/* the board */
#define      B_COLS 12
#define      B_ROWS 23
#define      B_SIZE (B_ROWS * B_COLS)

#define TL     -B_COLS-1	/* top left */
#define TC     -B_COLS		/* top center */
#define TR     -B_COLS+1	/* top right */
#define ML     -1		/* middle left */
#define MR     1		/* middle right */
#define BL     B_COLS-1		/* bottom left */
#define BC     B_COLS		/* bottom center */
#define BR     B_COLS+1		/* bottom right */

/* These can be overridden by the user. */
#define DEFAULT_KEYS "hjkl pqr"
#define KEY_LEFT    0
#define KEY_RROTATE 1
#define KEY_ROTATE  2
#define KEY_RIGHT   3
#define KEY_DROP    4
#define KEY_PAUSE   5
#define KEY_QUIT    6
#define KEY_RESTART 7

#define TEMP_SCORE_FILE "/tmp/tetris-tmp.scores"

static char state_dir[PATH_MAX];
static char high_score_file[PATH_MAX];

static volatile sig_atomic_t running = 1;

static struct termios savemodes;
static int havemodes = 0;

static char *keys = DEFAULT_KEYS;
static int level = 1;
static long points = 0;
static int lines_cleared = 0;
static int board[B_SIZE], shadow[B_SIZE];

static int *peek_shape;		/* peek preview of next shape */
static int  pcolor;
static int *shape;
static int  color;

static int shapes[] = {
	 7, TL, TC, MR, 2,	/* ""__   */
	 8, TR, TC, ML, 3,	/* __""   */
	 9, ML, MR, BC, 1,	/* "|"    */
	 3, TL, TC, ML, 4,	/* square */
	12, ML, BL, MR, 5,	/* |"""   */
	15, ML, BR, MR, 6,	/* """|   */
	18, ML, MR,  2, 7,	/* ---- sticks out */
	 0, TC, ML, BL, 2,	/* /    */
	 1, TC, MR, BR, 3,	/* \    */
	10, TC, MR, BC, 1,	/* |-   */
	11, TC, ML, MR, 1,	/* _|_  */
	 2, TC, ML, BC, 1,	/* -|   */
	13, TC, BC, BR, 5,	/* |_   */
	14, TR, ML, MR, 5,	/* ___| */
	 4, TL, TC, BC, 5,	/* "|   */
	16, TR, TC, BC, 6,	/* |"   */
	17, TL, MR, ML, 6,	/* |___ */
	 5, TC, BC, BL, 6,	/* _| */
	 6, TC, BC,  2 * B_COLS, 7, /* | sticks out */
};

static void init_high_score_file(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    const char *base;

    if (xdg && *xdg) {
        base = xdg;
    } else if (home && *home) {
        base = home;
    } else {
        fputs("ERROR: neither XDG_STATE_HOME nor HOME is set\n", stderr);
        exit(1);
    }

    if (xdg && *xdg) {
        if (snprintf(state_dir, sizeof state_dir, "%s/games", base) >= (int)sizeof state_dir) {
            fputs("ERROR: state_dir path too long\n", stderr);
            exit(1);
        }
    } else {
        if (snprintf(state_dir, sizeof state_dir,
                     "%s/.local/state/games", base) >= (int)sizeof state_dir) {
            fputs("ERROR: state_dir path too long\n", stderr);
            exit(1);
        }
    }

    if (mkdir(state_dir, 0755) != 0 && errno != EEXIST) {
        perror("mkdir(state_dir)");
        exit(1);
    }

    if (snprintf(high_score_file, sizeof high_score_file,
                 "%s/tetris.scores", state_dir) >= (int)sizeof high_score_file) {
        fputs("ERROR: high_score_file path too long\n", stderr);
        exit(1);
    }
}

static void draw(int x, int y, int c)
{
	gotoxy(x, y);
	bgcolor(c, "  ");
}

static int update(void)
{
	int x, y;

#ifdef ENABLE_PREVIEW
	static int shadow_preview[B_COLS * 10] = { 0 };
	int preview[B_COLS * 10] = { 0 };
	const int start = 5;

	preview[2 * B_COLS + 1] = pcolor;
	preview[2 * B_COLS + 1 + peek_shape[1]] = pcolor;
	preview[2 * B_COLS + 1 + peek_shape[2]] = pcolor;
	preview[2 * B_COLS + 1 + peek_shape[3]] = pcolor;

	for (y = 0; y < 4; y++) {
		for (x = 0; x < B_COLS; x++) {
			if (preview[y * B_COLS + x] - shadow_preview[y * B_COLS + x]) {
				int c = preview[y * B_COLS + x]; /* color */

				shadow_preview[y * B_COLS + x] = c;
				draw(x * 2 + 26 + 28, start + y, c);
			}
		}
	}
#endif

	/* Display board. */
	for (y = 1; y < B_ROWS - 1; y++) {
		for (x = 0; x < B_COLS; x++) {
			if (board[y * B_COLS + x] - shadow[y * B_COLS + x]) {
				int c = board[y * B_COLS + x]; /* color */

				shadow[y * B_COLS + x] = c;
				draw(x * 2 + 28, y, c);
			}
		}
	}

	/* Update points and level */
	while (lines_cleared >= 10) {
		lines_cleared -= 10;
		level++;
	}

#ifdef ENABLE_SCORE
	/* Display current level and points */
	gotoxy(26 + 28, 2);
	printf("\033[0mLevel  : %d", level);
	gotoxy(26 + 28, 3);
	printf("Points : %ld", points);
#endif
#ifdef ENABLE_PREVIEW
	gotoxy(26 + 28, 5);
	printf("Preview:");
#endif
	gotoxy(26 + 28, 10);
	printf("Keys:");
	fflush(stdout);

	return getchar();
}

/* Check if shape fits in the current position */
static int fits_in(int *s, int pos)
{
	if (board[pos] || board[pos + s[1]] || board[pos + s[2]] || board[pos + s[3]])
		return 0;

	return 1;
}

/* place shape at pos with color */
static void place(int *s, int pos, int c)
{
	board[pos] = c;
	board[pos + s[1]] = c;
	board[pos + s[2]] = c;
	board[pos + s[3]] = c;
}

static int *next_shape(void)
{
	int  pos  = rand() % 7 * 5;
	int *next = peek_shape;

	peek_shape = &shapes[pos];
	pcolor = peek_shape[4];
	if (!next)
		return next_shape();
	color = next[4];

	return next;
}

static void show_high_score(void)
{
#ifdef ENABLE_HIGH_SCORE
    FILE *tmpscore;

    if ((tmpscore = fopen(high_score_file, "a"))) {
        char *name = getenv("LOGNAME");

        if (!name)
            name = "anonymous";

        fprintf(tmpscore, "%7ld\t %5ld\t  %3d\t%s\n",
                (long)points * level,
                (long)points,
                level,
                name);
        fclose(tmpscore);

        char cmd[PATH_MAX * 2 + 128];

        snprintf(cmd, sizeof(cmd),
            "cat \"%s\" | sort -rn | head -10 > \"%s\" && "
            "cp \"%s\" \"%s\"",
            high_score_file, TEMP_SCORE_FILE,
            TEMP_SCORE_FILE, high_score_file);

        system(cmd);
        remove(TEMP_SCORE_FILE);
    }

    if (access(high_score_file, R_OK) == 0) {
        fprintf(stderr, "  Score\tPoints\tLevel\tName\n");

        char view_cmd[PATH_MAX + 32];
        snprintf(view_cmd, sizeof(view_cmd),
            "cat \"%s\"",
            high_score_file);
        system(view_cmd);
    }
#endif /* ENABLE_HIGH_SCORE */
}

static void show_online_help(void)
{
	const int start = 11;

	gotoxy(26 + 28, start);
	puts("\033[0mh     - left");
	gotoxy(26 + 28, start + 1);
	puts("j     - reverse rotate");
	gotoxy(26 + 28, start + 2);
	puts("k     - rotate");
	gotoxy(26 + 28, start + 3);
	puts("l     - right");
	gotoxy(26 + 28, start + 4);
	puts("space - drop");
	gotoxy(26 + 28, start + 5);
	puts("p     - pause");
	gotoxy(26 + 28, start + 6);
	puts("r     - restart");
	gotoxy(26 + 28, start + 7);
	puts("q     - quit");
}

/* Code stolen from http://c-faq.com/osdep/cbreak.html */
static int tty_init(void)
{
	struct termios modmodes;

	if (tcgetattr(fileno(stdin), &savemodes) < 0)
		return -1;

	havemodes = 1;
	hidecursor();

	/* "stty cbreak -echo" */
	modmodes = savemodes;
	modmodes.c_lflag &= ~ICANON;
	modmodes.c_lflag &= ~ECHO;
	modmodes.c_cc[VMIN] = 1;
	modmodes.c_cc[VTIME] = 0;

	return tcsetattr(fileno(stdin), TCSANOW, &modmodes);
}

static int tty_exit(void)
{
	if (!havemodes)
		return 0;

	showcursor();

	/* "stty sane" */
	return tcsetattr(fileno(stdin), TCSANOW, &savemodes);
}

static void freeze(int enable)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);

	sigprocmask(enable ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

static void alarm_handler(int signo)
{
	static long h[4];

	(void)signo;

	/* On init from main() */
	if (!signo)
		h[3] = 500000;

	h[3] -= h[3] / (3000 - 10 * level);
	setitimer(0, (struct itimerval *)h, 0);
}

static void exit_handler(int signo)
{
	(void)signo;
	running = 0;
}

static void sig_init(void)
{
	struct sigaction sa;

	SIGNAL(SIGINT, exit_handler);
	SIGNAL(SIGTERM, exit_handler);
	SIGNAL(SIGALRM, alarm_handler);

	/* Start update timer. */
	alarm_handler(0);
}
static void init(int *c, int *pos ) 
{
	int i, *ptr;

	level = 1;
	points = 0;
	lines_cleared = 0;
	*c = 0;
	*pos = 17;
	ptr = board;
	
	/* Initialize board, grey border, used to be white(7) */
	for (i = B_SIZE; i; i--)
		*ptr++ = i < 25 || i % B_COLS < 2 ? 60 : 0;
	for (i = B_SIZE; i--; shadow[i] = 0)
		;
	clrscr();
	/* Set up signals */
	alarm_handler(0);
	show_online_help();
	shape = next_shape();
}

int main(void)
{
	int i, c = 0, pos;
	int *backup;

	srand((unsigned int)time(NULL));
	if (tty_init() == -1)
		return 1;
	
	init_high_score_file();

	/* Set up signals */
	sig_init();
	
	init(&c, &pos);

	while (running) {
		if (c < 0) {
			if (fits_in(shape, pos + B_COLS)) {
				pos += B_COLS;
			} else {
				place(shape, pos, color);
				int clears = 0;
				for (i = 1; i < B_ROWS-2; ++i) {
					int full = 1;
					for (int x = 1; x < B_COLS-1; ++x) {
						if (!board[i * B_COLS + x]) {
							full = 0;
							break;
						}
					}
					if (full) {
						++clears;
						/* clear row i */
						for (int x = 1; x < B_COLS-1; ++x)
							board[i * B_COLS + x] = 0;
						c = update();

						/* shift everything above row i down one */
						for (int y = i; y > 0; --y) {
							for (int x = 1; x < B_COLS-1; ++x)
								board[y * B_COLS + x] = board[(y-1) * B_COLS + x];
						}
						c = update();
						/* re‐check this same row index next iteration */
						--i;
					}
				}

				if (clears > 0) {
					static const int score_table[5] = {
						0,
						40,
						100,
						300,
						1200
					};
					double ofcheck = points + score_table[clears] * level;
					if (ofcheck > LONG_MAX) {
						clrscr();
						gotoxy(0, 0);
						printf("\n\nYOU HAVE WON\n\n");
						printf("\033[0mYour score: %ld points x level %d = %ld\n\n", points, level, points * level);
						show_high_score();
						sleep(5);
						break;
					}
					points += score_table[clears] * level;
					lines_cleared += clears;
				}
				c = update();
				shape = next_shape();
				if (!fits_in(shape, pos = 17)) {
					clrscr();
					gotoxy(0, 0);
					printf("");
					printf("\n\nYOU HAVE FAILED!\n\n\033[0mYour score: %ld points x level %d = %ld\n\n", points, level, points * level);
					show_high_score();
					freeze(1);
					printf("\n\nPress 'r' for replay or 'q' for quit!\n");
					while ((c = getchar())) {
						if (c == keys[KEY_QUIT] || c == keys[KEY_RESTART])
							break;
					}
					if (c == keys[KEY_QUIT])
						break;
					else
						freeze(0);

					init(&c, &pos);
					continue;
				}
			}
		}

		if (c == keys[KEY_LEFT]) {
			if (!fits_in(shape, --pos))
				++pos;
		}

		if (c == keys[KEY_ROTATE]) {
			backup = shape;
			int curr_idx = (shape - shapes) / 5;
			int prev_idx = curr_idx;
			for (int i = 0; i < 19; i++) {
				if (shapes[5*i + 0] == curr_idx) {
					prev_idx = i;
					break;
				}
			}
			shape = &shapes[5 * prev_idx];
			if (!fits_in(shape, pos))
				shape = backup;
		}
		
		if (c == keys[KEY_RROTATE]) {
			backup = shape;
			shape = &shapes[5 * *shape];	/* Rotate */
			/* Check if it fits, if not restore shape from backup */
			if (!fits_in(shape, pos))
				shape = backup;
		}

		if (c == keys[KEY_RIGHT]) {
			if (!fits_in(shape, ++pos))
				--pos;
		}

		if (c == keys[KEY_DROP]) {
			for (; fits_in(shape, pos + B_COLS); ++points)
				pos += B_COLS;
		}

		if (c == keys[KEY_RESTART]) {
			init(&c, &pos);
			continue;
		}

		if (c == keys[KEY_PAUSE] || c == keys[KEY_QUIT]) {
			freeze(1);

			if (c == keys[KEY_QUIT]) {
				clrscr();
				gotoxy(0, 0);

				printf("\033[0mYour score: %ld points x level %d = %ld\n\n", points, level, points * level);
				show_high_score();
				sleep(5);
				break;
			}

			for (i = B_SIZE; i--; shadow[i] = 0)
			   ;

			while (getchar() - keys[KEY_PAUSE])
			   ;

			freeze(0);
		}

		place(shape, pos, color);
		c = update();
		place(shape, pos, 0);
	}

	clrscr();
	if (tty_exit() == -1)
		return 1;

	return 0;
}
