// Emacs configuration: -*- eval: (indent-tabs-mode 1) -*-
// Compile with: cc -o pong pong.c -lX11
// See LICENSE for license and copyright details.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define FPS 30
#define PLAYER_SIZE 35
#define BAR_OFFSET 50
#define MOVE_FACTOR 10

#define BETWEEN(thing, min, max) (((min) <= (thing)) && ((thing) <= (max)))
#define LEN(array) (sizeof (array) / sizeof *(array))

enum { CursorPointer, CursorNormal, CursorLAST };
enum { BarLeft=0, BarRight=1 };
typedef enum { GSTitle, GSPlaying, GSDead } Gamestate;

typedef struct {
	unsigned int width, height;
} Dimensions;
typedef struct {
	int x, y;
	unsigned char direction; /* 0b[y][x]: up/down+left/right, 0=down/left,1=up/right */
	int deadtime;
	unsigned int score;
} Player;
typedef struct {
	int x, y;
	int edge_x;
} Bar;
typedef struct {
	short *offs;
	int bs; /* block size */
} Wipe;

/* functions */
static void bounceplayer(void);
_X_NORETURN void die(const char *msg);
static void dowipe(void);
static void drawstage(void);
static void drawtitle(void);
static void init(void);
static void keypress(XEvent *ev);
static void keyrelease(XEvent *ev);
static void message(XEvent *ev);
static void matchfps(void);
static void nextstate(void);
static void play(void);
static void run(void);
static int touchingbar(void);
static void wait4expose(Window where);

/* variables */
static Display *dpy;
static Window win, titlewin;
static Dimensions winsz, twsz;
static Pixmap buffer, twbuffer; /* buffers to draw onto (double buffering) */
static GC gc;
static Atom WM_DELETE;
static Cursor cursors[CursorLAST];
static void (*handlers[])(XEvent *ev) = {
	[ClientMessage] = message,
	[KeyPress] = keypress,
	[KeyRelease] = keyrelease,
};
static Gamestate gamestate;
static Dimensions barsz;
static Bar bars[2];
static Bar *activebar;
static int barmove;
static Player player;
static Wipe wipe;
static int quit;

void bounceplayer(void) {
	int dy = (player.direction & 2) - 1;
	int lasty = player.y;

	dy *= MOVE_FACTOR;
	player.y += dy;

	if(!BETWEEN(player.y, 0, (int) winsz.height - PLAYER_SIZE)) {
		player.y = lasty-dy;
		player.direction ^= 2;
	}
}

void die(const char *msg) {
	if(msg && *msg) {
		fputs(msg, stderr);
		if(msg[strlen(msg) - 1] == ':') {
			fprintf(stderr, " %s", strerror(errno));
		}
		fputc('\n', stderr);
	}

	if(dpy) XCloseDisplay(dpy);

	exit(1);
}

void dowipe(void) {
	int done = 1;
	int blocks = (winsz.width + wipe.bs - 1) / wipe.bs;
	int i;

	/* randomize starting offsets */
	if(player.deadtime == 0) {
		srand(time(NULL));
		for(i = 0; i < blocks; i++) {
			wipe.offs[i] = -(rand() % 10);
		}
	}

	++player.deadtime;
	if(player.deadtime < 10) return; /* wait for 10 frames before wipe */

	for(i = 0; i < blocks; i++) {
		if(wipe.offs[i] < 0) {
			wipe.offs[i]++; done = 0;
		} else if(wipe.offs[i] < (int) winsz.height) {
			wipe.offs[i] += wipe.bs;
			done = 0;
			XSetForeground(dpy, gc, 0);
			XCopyArea(dpy, buffer, buffer, gc, i*wipe.bs, 0, wipe.bs, winsz.height, i*wipe.bs, wipe.bs);
			XFillRectangle(dpy, buffer, gc, i*wipe.bs, 0, wipe.bs, wipe.bs);
		}
	}

	if(done) {
		nextstate();
	}
}

void drawstage(void) {
	static char scorebuf[16];
	size_t n;
	int baryoff;

	/* draw player */
	XSetForeground(dpy, gc, 0xEEEEEE);
	XFillRectangle(dpy, buffer, gc, player.x, player.y, PLAYER_SIZE, PLAYER_SIZE);
	XSetForeground(dpy, gc, 0x303030);
	XFillRectangle(dpy, buffer, gc, player.x + PLAYER_SIZE/7, player.y + PLAYER_SIZE/7,
	               PLAYER_SIZE*5/7, PLAYER_SIZE*5/7);

	/* draw bars */
	baryoff = (winsz.height - barsz.height) / 2;
	XSetForeground(dpy, gc, 0xEEEEEE);
	XFillRectangle(dpy, buffer, gc, bars[BarLeft].x,  baryoff + -bars[BarLeft].y,
	               barsz.width, barsz.height); /* left */
	XFillRectangle(dpy, buffer, gc, bars[BarRight].x, baryoff + -bars[BarRight].y,
	               barsz.width, barsz.height); /* right */

	/* draw score */
	n = snprintf(scorebuf, sizeof scorebuf, "Score: %u", player.score);
	XSetForeground(dpy, gc, 0xFFFFFF);
	XSetFunction(dpy, gc, GXxor); /* the bar may get on top of the score string, this is for visibility */
		XDrawString(dpy, buffer, gc, 20, 20, scorebuf, n);
	XSetFunction(dpy, gc, GXcopy);
}

void drawtitle(void) {
	XSetForeground(dpy, gc, 0x181818);
	XFillRectangle(dpy, twbuffer, gc, 0, 0, twsz.width, twsz.height);

	XSetForeground(dpy, gc, 0xEEEEEE);
	XDrawRectangle(dpy, twbuffer, gc, 5, 5, twsz.width - 10, twsz.height - 10);
	XFillRectangle(dpy, twbuffer, gc, 10, 10, twsz.width - 20, twsz.height - 20);
	XSetForeground(dpy, gc, 0x181818);
	XDrawString(dpy, twbuffer, gc, 25, 30, "Press Enter to start!", strlen("Press Enter to start!"));
	XCopyArea(dpy, twbuffer, titlewin, gc, 0, 0, twsz.width, twsz.height, 0, 0);
}

void init(void) {
	XSetWindowAttributes swa;
	XClassHint class;

	cursors[CursorPointer] = XCreateFontCursor(dpy, XC_arrow);
	cursors[CursorNormal] = XCreateFontCursor(dpy, XC_heart);

	winsz.width = 1280;
	winsz.height = 720;

	twsz.width = winsz.width/2+50;
	twsz.height = winsz.height / 2;

	barsz.width = PLAYER_SIZE;
	barsz.height = winsz.height / 2;

	wipe.bs = 16;
	wipe.offs = calloc((winsz.width + wipe.bs-1) / wipe.bs, sizeof *wipe.offs);

	player.x = (winsz.width - PLAYER_SIZE) / 2;
	player.y = (winsz.height - PLAYER_SIZE) / 2;
	player.direction = 0b00;

	bars[BarLeft].x = BAR_OFFSET;
	bars[BarLeft].edge_x = BAR_OFFSET + barsz.width;
	bars[BarRight].x = bars[BarRight].edge_x = winsz.width - BAR_OFFSET - barsz.width;
	activebar = &bars[player.direction&1];

	/* create primary window */
	swa.background_pixmap = None;
	swa.event_mask = ExposureMask|KeyPressMask|KeyReleaseMask;
	swa.cursor = cursors[CursorNormal];

	win = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)), 100, 100, 800, 600,
	                    0, DefaultDepth(dpy, DefaultScreen(dpy)), InputOutput,
	                    CopyFromParent, CWBackPixmap|CWEventMask|CWCursor, &swa);

	/* create title screen [overlay] window */
	swa.cursor = cursors[CursorPointer];

	titlewin = XCreateSimpleWindow(dpy, win, (winsz.width-twsz.width)/2, (winsz.height-twsz.height)/2,
	                               twsz.width, twsz.height, 0, 0, 0);
	XSelectInput(dpy, titlewin, ExposureMask|KeyPressMask);
	XChangeWindowAttributes(dpy, titlewin, CWCursor, &swa);

	XStoreName(dpy, win, "XPong");
	XStoreName(dpy, titlewin, "XPong <title>");
	class.res_name = class.res_class = "XPong";
	XSetClassHint(dpy, win, &class);

	WM_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", True);
	XSetWMProtocols(dpy, win, &WM_DELETE, 1);

	gc = XCreateGC(dpy, win, 0, NULL);
	buffer = XCreatePixmap(dpy, win, winsz.width, winsz.height, DefaultDepth(dpy, DefaultScreen(dpy)));
	twbuffer = XCreatePixmap(dpy, titlewin, twsz.width, twsz.height, DefaultDepth(dpy, DefaultScreen(dpy)));
}

void keypress(XEvent *ev) {
	int ks = XKeycodeToKeysym(dpy, ev->xkey.keycode, 0);

	if(gamestate == GSTitle && ks == XK_Return) {
		nextstate();
	}

	if(ks == XK_q || ks == XK_Escape) quit = 1;

	if(gamestate == GSPlaying) {
		if(ks == XK_Up)
			barmove = 1;
		if(ks == XK_Down)
			barmove = -1;
	}
}

void keyrelease(XEvent *ev) {
	int ks = XKeycodeToKeysym(dpy, ev->xkey.keycode, 0);

	if(gamestate == GSPlaying) {
		if(ks == XK_Up || ks == XK_Down) barmove = 0;
	}
}

void message(XEvent *ev) {
	XClientMessageEvent *e = &ev->xclient;

	if(WM_DELETE == (Atom)e->data.l[0])
		quit = 1;
}

void matchfps() {
	static const float spf = 1 / (float) FPS;
	struct timespec ts;

	ts.tv_sec = (time_t) spf;
	ts.tv_nsec = (long long) (1e9 * spf) % (long long) 1e9;
	nanosleep(&ts, NULL);
}

void nextstate(void) {
	++gamestate;
	if(gamestate > GSDead) gamestate = GSTitle;

	player.deadtime = -1;

	switch(gamestate) {
		case GSTitle:
			player.score = 0;
			bars[BarLeft].y = bars[BarRight].y = 0;
			player.x = (winsz.width - PLAYER_SIZE) / 2;
			player.y = (winsz.height - PLAYER_SIZE) / 2;
			XMapWindow(dpy, titlewin);
			break;

		case GSPlaying:
			XUnmapWindow(dpy, titlewin);
			break;

		default: break;
	}
}

void play(void) {
	int dx = ((player.direction & 1) << 1) - 1 /* turn boolean into plus/minus 1 */
	, dy = (player.direction & 2) - 1;

	int lastx = player.x
	, lasty = player.y
	, lastbary = activebar->y;

	/* move bar */
	activebar->y += barmove * MOVE_FACTOR;

	if(!BETWEEN(activebar->y, (int) -(winsz.height-barsz.height)/2, (int) (winsz.height-barsz.height)/2)) {
		activebar->y = lastbary;
	}

	/* move player */
	dx *= MOVE_FACTOR;
	dy *= MOVE_FACTOR;

	player.x += dx;
	player.y += dy;

	/* collision checks */
	if(!BETWEEN(player.x, 0, (int) winsz.width - PLAYER_SIZE)) {
		nextstate();
		return;
	}

	if(!BETWEEN(player.y, 0, (int) winsz.height - PLAYER_SIZE)) {
		player.direction ^= 2; /* change Y direction */
		player.y = lasty-dy;
	}

	if(touchingbar()) {
		player.score += 1;
		player.direction ^= 1; /* change X direction */
		player.x = lastx-dx;
		activebar = &bars[player.direction&1];
		barmove = 0;
	}
}

void run(void) {
	XEvent ev;

	XMapWindow(dpy, win);
	XMapWindow(dpy, titlewin);
	wait4expose(win);
	wait4expose(titlewin);

	while(!quit) {
		while(XPending(dpy) > 0) {
			XNextEvent(dpy, &ev);
			if(handlers[ev.type]) handlers[ev.type](&ev);
		}

		if(gamestate != GSDead) {
			XSetForeground(dpy, gc, 0);
			XFillRectangle(dpy, buffer, gc, 0, 0, winsz.width, winsz.height);
			drawstage();
		}

		switch(gamestate) {
			case GSTitle:
				/* draw box with text like "Press Space to start" */
				bounceplayer();
				drawtitle();
				break;
			case GSPlaying:
				play();
				break;
			case GSDead:
				dowipe();
				break;
		}

		/* swap buffers */
		XCopyArea(dpy, buffer, win, gc, 0, 0, winsz.width, winsz.height, 0, 0);
		matchfps();
	}
}

int touchingbar(void) {
	int barymin = (winsz.height - barsz.height)/2 + -activebar->y;
	int barymax = (winsz.height + barsz.height)/2 + -activebar->y;

	return BETWEEN(activebar->edge_x, player.x, player.x + PLAYER_SIZE) && BETWEEN(player.y, barymin, barymax);
}

void wait4expose(Window where) {
	XEvent ev;
	/* According to X manual, you should not draw anything onto
	 * a window until you receive at least one Expose event. */

	for(;;) {
		XWindowEvent(dpy, where, ExposureMask, &ev);
		if(ev.xexpose.count == 0) break;
	}
	XFlush(dpy);
}

int main(void) {
	dpy = XOpenDisplay(NULL);
	if(!dpy) die("couldn't open display");

	init();
	run();

	XFreeGC(dpy, gc);
	XCloseDisplay(dpy);
	return 0;
}
