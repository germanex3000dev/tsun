/* tsun.c: minimal tsundere text editor -- a C rebuild of tsun.lua
 *
 * build:   cc -std=c11 -O2 -Wall -Wextra -o tsun tsun.c    (or just: make)
 * usage:   ./tsun [file]
 *
 * type to insert, arrows/home/end/delete to move
 * ^S save   ^Q save and quit   ^C quit without saving
 *
 * needs a POSIX terminal (termios + ioctl TIOCGWINSZ)
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ personality */
/* tsundere engine: tables of strings, pick one at random.
 * mood: 0-100, lower = madder / more tsundere. your actions change it. */
static int mood = 40;

static const char *say_open_refuse[] = {
  "i-i'm not opening that file! who do you think you are?",
  "h-huh? open it yourself, baka!",
  "no way. i'm not reading that for you.",
  "tch. don't tell me what to open.",
  "i-i don't want to! leave me alone!",
};

static const char *say_open_ok[] = {
  "fine, i opened it. don't expect this to happen again.",
  "i opened it because i felt like it, not for you.",
  "...here. it's open. happy now, idiot?",
};

static const char *say_save[] = {
  "i-it's not like i saved it for you or anything...",
  "saved. don't read into it, baka.",
  "fine, it's saved. you're welcome, i guess.",
  "...there. saved. now stop asking.",
};

static const char *say_save_quit[] = {
  "i saved it first. don't get the wrong idea, idiot.",
  "saved and done. it's not like i'll miss you.",
};

static const char *say_discard_clean[] = {
  "...you didn't even change anything. whatever.",
  "leaving already? nothing to throw away, so. fine.",
};

static const char *say_discard_dirty[] = {
  "FINE. throw it all away. see if i care.",
  "all that work... gone. not that i was watching. BAKA.",
};

static const char *say_discard_refuse[] = {
  "i-i'm not letting you throw it all away!",
  "no! our work stays right here, baka!",
  "absolutely not. you're not quitting without saving.",
  "tch, you think i'd just let you leave? think again.",
};

static const char *say_discard_relent[] = {
  "FINE. go ahead and throw it away. see if i care.",
  "...do whatever you want. i'm not stopping you anymore.",
  "fine, leave. throw it all away. i-i wasn't watching anyway.",
};

static const char *say_save_fail[] = {
  "i-i couldn't save it. it's not my fault!",
  "i didn't open that file, so i'm not saving it either!",
  "the file wouldn't save. blame the computer, not me!",
};

static const char *say_type_taunt[] = {
  "type faster, idiot.",
  "why are you typing so much?",
  "...a lot to say, huh?",
};

#define PICK(t) (t)[(size_t)rand() % (sizeof(t) / sizeof((t)[0]))]

/* nudge the mood (0-100). lower = madder. */
static void mood_delta(int d) {
  mood += d;
  if (mood < 0) mood = 0;
  if (mood > 100) mood = 100;
}

/* ------------------------------------------------------------------ terminal */
static struct termios orig_termios;
static int raw_active = 0;

static void winsize(int *out_rows, int *out_cols) {
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1 && ws.ws_col > 0) {
    *out_rows = ws.ws_row;
    *out_cols = ws.ws_col;
  } else {
    *out_rows = 24;
    *out_cols = 80;
  }
}

static void leave_raw(void) {
  if (!raw_active) return;
  ssize_t w = write(1, "\033[?1049l", 8); (void)w;
  tcsetattr(0, TCSAFLUSH, &orig_termios);
  raw_active = 0;
}

static int enter_raw(void) {
  if (tcgetattr(0, &orig_termios) != 0) {
    fprintf(stderr, "tsun: stdin has to be a terminal\n");
    return -1;
  }

  struct termios raw = orig_termios;
  raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
  raw.c_lflag &= (tcflag_t)~(ISIG | ICANON | ECHO | IEXTEN);
  raw.c_cc[VMIN] = 0;   /* read returns even with no input... */
  raw.c_cc[VTIME] = 1;  /* ...after 0.1s, so a lone ESC is detectable */

  tcsetattr(0, TCSAFLUSH, &raw);
  ssize_t w = write(1, "\033[?1049h", 8); (void)w; /* alternate screen */
  raw_active = 1;
  return 0;
}

/* ------------------------------------------------------------------ helpers */
static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p) { fprintf(stderr, "tsun: out of memory\n"); exit(1); }
  return p;
}

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) { fprintf(stderr, "tsun: out of memory\n"); exit(1); }
  return q;
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = xmalloc(n);
  memcpy(p, s, n);
  return p;
}

/* lua's s:sub(a, b): 1-based, inclusive, clamped */
static char *str_sub(const char *s, size_t a, size_t b) {
  size_t n = strlen(s);
  if (a == 0) a = 1;
  if (b > n) b = n;
  if (b < a) return xstrdup("");
  size_t len = b - a + 1;
  char *o = xmalloc(len + 1);
  memcpy(o, s + a - 1, len);
  o[len] = '\0';
  return o;
}

/* lua's s:sub(a) -- from a to the end */
#define str_from(s, a) str_sub((s), (a), (size_t)-1)

static char *join2(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *o = xmalloc(la + lb + 1);
  memcpy(o, a, la);
  memcpy(o + la, b, lb);
  o[la + lb] = '\0';
  return o;
}

static char *join3(const char *a, const char *b, const char *c) {
  char *ab = join2(a, b);
  char *o = join2(ab, c);
  free(ab);
  return o;
}

/* ------------------------------------------------------------------ buffer */
static const char *fname = "scratch.txt";
static char **lines;
static size_t nlines, lines_cap;
static size_t cx = 1, cy = 1;  /* cursor: column and row (1-based) */
static size_t rowoff, coloff;  /* scroll offsets */
static int dirty, loaded;      /* did she actually open the file? */
static int discard_refusals;   /* how many times she's blocked ^C */
static const char *msg = "";
static int rows = 24, cols = 80;

static void lines_grow(void) {
  lines_cap = lines_cap ? lines_cap * 2 : 16;
  lines = xrealloc(lines, lines_cap * sizeof *lines);
}

/* takes ownership of s */
static void lines_push(char *s) {
  if (nlines == lines_cap) lines_grow();
  lines[nlines++] = s;
}

/* 0-based index, takes ownership of s */
static void lines_insert(size_t idx, char *s) {
  if (nlines == lines_cap) lines_grow();
  memmove(lines + idx + 1, lines + idx, (nlines - idx) * sizeof *lines);
  lines[idx] = s;
  nlines++;
}

/* 0-based index, frees the removed line */
static void lines_remove(size_t idx) {
  free(lines[idx]);
  memmove(lines + idx, lines + idx + 1, (nlines - idx - 1) * sizeof *lines);
  nlines--;
}

static void load_file(void) {
  FILE *f = fopen(fname, "r");
  if (f) {
    char *l = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&l, &cap, f)) != -1) {
      if (len > 0 && l[len - 1] == '\n') l[--len] = '\0';
      if (len > 0 && l[len - 1] == '\r') l[--len] = '\0'; /* strip \r of \r\n */
      lines_push(xstrdup(l));
    }
    free(l);
    fclose(f);
  }
  if (nlines == 0) lines_push(xstrdup(""));
}

static int save_file(void) {
  if (!loaded) return 0;
  FILE *f = fopen(fname, "w");
  if (!f) return 0;
  for (size_t i = 0; i < nlines; i++) {
    fputs(lines[i], f);
    fputc('\n', f);
  }
  fclose(f);
  dirty = 0;
  mood_delta(2);
  return 1;
}

/* ------------------------------------------------------------------ editing */
static void insert_char(char ch) {
  char *l = lines[cy - 1];
  char one[2] = { ch, '\0' };
  char *nl = join3(str_sub(l, 1, cx - 1), one, str_from(l, cx));
  free(l);
  lines[cy - 1] = nl;
  cx++;
  dirty = 1;
  if (rand() % 40 == 0) {
    msg = PICK(say_type_taunt);
    mood_delta(-1);
  }
}

static void newline(void) {
  char *l = lines[cy - 1];
  lines[cy - 1] = str_sub(l, 1, cx - 1);
  lines_insert(cy, str_from(l, cx)); /* insert after the current line */
  free(l);
  cy++;
  cx = 1;
  dirty = 1;
}

static void backspace(void) {
  if (cx > 1) {
    char *l = lines[cy - 1];
    char *nl = join2(str_sub(l, 1, cx - 2), str_from(l, cx));
    free(l);
    lines[cy - 1] = nl;
    cx--;
    dirty = 1;
  } else if (cy > 1) {
    char *prev = lines[cy - 2];
    char *cur = lines[cy - 1];
    size_t new_cx = strlen(prev) + 1;
    char *nl = join2(prev, cur);
    lines_remove(cy - 1); /* frees cur; prev still in place */
    free(prev);
    lines[cy - 2] = nl;
    cx = new_cx;
    cy--;
    dirty = 1;
  }
}

static void delete_char(void) {
  char *l = lines[cy - 1];
  size_t len = strlen(l);
  if (cx <= len) {
    char *nl = join2(str_sub(l, 1, cx - 1), str_from(l, cx + 1));
    free(l);
    lines[cy - 1] = nl;
    dirty = 1;
  } else if (cy < nlines) {
    char *nl = join2(l, lines[cy]);
    lines_remove(cy); /* frees the next line; l still in place */
    free(l);
    lines[cy - 1] = nl;
    dirty = 1;
  }
}

/* ------------------------------------------------------------------ movement */
enum { MV_LEFT, MV_RIGHT, MV_UP, MV_DOWN, MV_HOME, MV_END };

static void clamp_cx(void) {
  size_t len = strlen(lines[cy - 1]);
  if (cx > len + 1) cx = len + 1;
}

static void move_cursor(int dir) {
  switch (dir) {
  case MV_LEFT:
    if (cx > 1) {
      cx--;
    } else if (cy > 1) {
      cy--;
      cx = strlen(lines[cy - 1]) + 1;
    }
    break;
  case MV_RIGHT:
    if (cx <= strlen(lines[cy - 1])) {
      cx++;
    } else if (cy < nlines) {
      cy++;
      cx = 1;
    }
    break;
  case MV_UP:
    if (cy > 1) {
      cy--;
      clamp_cx();
    }
    break;
  case MV_DOWN:
    if (cy < nlines) {
      cy++;
      clamp_cx();
    }
    break;
  case MV_HOME:
    cx = 1;
    break;
  case MV_END:
    cx = strlen(lines[cy - 1]) + 1;
    break;
  }
}

/* ------------------------------------------------------------------ drawing */
typedef struct { char *s; size_t len, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bprintf(Buf *b, const char *fmt, ...) {
  char tmp[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) bput(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void bput_trunc(Buf *b, const char *s, size_t max) {
  size_t n = strlen(s);
  if (n > max) n = max;
  bput(b, s, n);
}

static void scroll(void) {
  int textrows = rows - 2;
  if (textrows < 1) textrows = 1;
  if (cy <= rowoff) rowoff = cy - 1;
  if (cy > rowoff + (size_t)textrows) rowoff = cy - (size_t)textrows;
  if (cx <= coloff) coloff = cx - 1;
  if (cx > coloff + (size_t)cols) coloff = cx - (size_t)cols;
}

/* chip/speck style status bar: filename, keybinds, byte count */
static char *status_bar(void) {
  long long bytes = 0;
  for (size_t i = 0; i < nlines; i++) bytes += (long long)strlen(lines[i]);
  bytes += (long long)nlines; /* one newline per line (internal + trailing) */
  size_t cap = strlen(fname) + 128;
  char *s = xmalloc(cap);
  snprintf(s, cap, "%s%s  ^S save  ^Q quit  ^C discard  %lld bytes",
           fname, dirty ? " [+]" : "", bytes);
  return s;
}

static void draw(void) {
  scroll();
  Buf b = { NULL, 0, 0 };
  bputs(&b, "\033[?25l");
  bputs(&b, "\033[H");
  for (int r = 1; r <= rows - 2; r++) {
    size_t idx = rowoff + (size_t)(r - 1);
    const char *l = idx < nlines ? lines[idx] : "";
    /* byte slice: l:sub(coloff + 1, coloff + cols) */
    size_t len = strlen(l);
    if (coloff < len) {
      size_t n = len - coloff;
      if (n > (size_t)cols) n = (size_t)cols;
      bput(&b, l + coloff, n);
    }
    bputs(&b, "\033[K\r\n");
  }
  /* penultimate row: dedicated tsundere message status bar */
  bput_trunc(&b, msg, (size_t)cols);
  bputs(&b, "\033[K\r\n");
  /* last row: chip/speck status bar (always visible, separate line) */
  char *bar = status_bar();
  bput_trunc(&b, bar, (size_t)cols);
  free(bar);
  bputs(&b, "\033[K");
  bprintf(&b, "\033[%zu;%zuH", cy - rowoff, cx - coloff);
  bputs(&b, "\033[?25h");
  ssize_t w = write(1, b.s, b.len); (void)w;
  free(b.s);
}

/* ------------------------------------------------------------------ input */
enum { ACT_NONE = 0, ACT_SAVE, ACT_QUIT, ACT_DISCARD };

/* returns ACT_* and sets *pos to the byte after what was consumed */
static int handle(const unsigned char *s, size_t start, size_t n, size_t *pos) {
  size_t i = start;
  while (i <= n) {
    unsigned char b = s[i - 1];
    if (b == 27) {
      if (i + 1 <= n && s[i] == '[') {
        char c = i + 2 <= n ? (char)s[i + 1] : '\0';
        int mv = -1;
        switch (c) {
        case 'A': mv = MV_UP; break;
        case 'B': mv = MV_DOWN; break;
        case 'C': mv = MV_RIGHT; break;
        case 'D': mv = MV_LEFT; break;
        case 'H': mv = MV_HOME; break;
        case 'F': mv = MV_END; break;
        }
        if (mv >= 0) {
          move_cursor(mv);
          i += 3;
        } else if (isdigit((unsigned char)c) && i + 3 <= n && s[i + 2] == '~') {
          if (c == '3') {
            delete_char();
          } else if (c == '1' || c == '7') {
            move_cursor(MV_HOME);
          } else if (c == '4' || c == '8') {
            move_cursor(MV_END);
          }
          i += 4;
        } else {
          /* unknown CSI sequence: skip parameters and final byte so
           * the leftover chars aren't inserted as literal text */
          size_t j = i + 2;
          while (j <= n && (isdigit(s[j - 1]) || s[j - 1] == ';')) j++;
          i = j + 1;
        }
      } else if (i + 1 <= n && s[i] == 'O') {
        char c = i + 2 <= n ? (char)s[i + 1] : '\0';
        if (c == 'H') move_cursor(MV_HOME);
        else if (c == 'F') move_cursor(MV_END);
        i += 3;
      } else {
        i += 1; /* lone ESC, ignore */
      }
    } else {
      i += 1;
      if (b == 19) {
        *pos = i;
        return ACT_SAVE;
      } else if (b == 17) {
        *pos = i;
        return ACT_QUIT;
      } else if (b == 3) {
        *pos = i;
        return ACT_DISCARD;
      } else if (b == 13 || b == 10) {
        newline();
      } else if (b == 127 || b == 8) {
        backspace();
      } else if (b >= 32 && b <= 126) {
        insert_char((char)b);
      }
    }
  }
  *pos = i;
  return ACT_NONE;
}

/* ------------------------------------------------------------------ main */
static const char *final_msg;
static char errbuf[256];

/* returns 0 on a clean exit (final_msg may be set), -1 on error (errbuf set) */
static int run(void) {
  winsize(&rows, &cols);

  /* sometimes she just refuses to open your file */
  int open_chance = 35 - mood / 2;
  if (rand() % 100 + 1 <= open_chance) {
    msg = PICK(say_open_refuse);
    mood_delta(-3);
    loaded = 0;
    /* start with an empty buffer; she's not showing you the file */
    lines_push(xstrdup(""));
  } else {
    msg = PICK(say_open_ok);
    load_file();
    loaded = 1;
  }
  draw();

  for (;;) {
    unsigned char buf[64];
    ssize_t r = read(0, buf, sizeof buf);
    if (r < 0) {
      if (errno == EINTR) continue;
      snprintf(errbuf, sizeof errbuf, "read failed: %s", strerror(errno));
      return -1;
    }
    if (r == 0) continue; /* timeout: nothing typed (VMIN=0, VTIME=1) */

    size_t pos = 1, len = (size_t)r;
    while (pos <= len) {
      size_t next;
      int action = handle(buf, pos, len, &next);
      pos = next;
      if (action == ACT_SAVE) {
        if (save_file()) msg = PICK(say_save);
        else msg = PICK(say_save_fail);
      } else if (action == ACT_QUIT) {
        if (dirty && !loaded) {
          /* she won't save a file she refused to open */
          msg = PICK(say_save_fail);
          pos = len + 1; /* stop processing this buffer */
        } else if (save_file()) {
          final_msg = PICK(say_save_quit);
          return 0;
        } else {
          msg = PICK(say_save_fail);
          pos = len + 1; /* stop processing this buffer */
        }
      } else if (action == ACT_DISCARD) {
        if (dirty) {
          discard_refusals++;
          if (discard_refusals < 3) {
            msg = PICK(say_discard_refuse);
            mood_delta(-2);
            /* refuse to quit: stay in the editor */
          } else {
            final_msg = PICK(say_discard_relent);
            mood_delta(-5);
            return 0;
          }
        } else {
          /* the lua original picks `dirty and discard_dirty or discard_clean`
           * here, but dirty is always false on this branch, so it always
           * lands on discard_clean (discard_dirty is unreachable in lua too) */
          final_msg = PICK(say_discard_clean);
          (void)say_discard_dirty; /* keep the table for fidelity with tsun.lua */
          return 0;
        }
      } else {
        if (msg[0] != '\0') msg = ""; /* message fades on next keypress */
      }
    }
    draw();
  }
}

static void fatal_signal(int sig) {
  leave_raw(); /* restore the terminal before dying */
  signal(sig, SIG_DFL);
  raise(sig);
}

int main(int argc, char **argv) {
  static const int fatal_sigs[] = {
    SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTERM, SIGHUP, SIGQUIT,
  };

  fname = argc > 1 ? argv[1] : "scratch.txt";
  srand((unsigned)time(NULL));

  if (enter_raw() != 0) return 1;
  if (atexit(leave_raw) != 0) { leave_raw(); return 1; }
  for (size_t i = 0; i < sizeof fatal_sigs / sizeof fatal_sigs[0]; i++)
    signal(fatal_sigs[i], fatal_signal);

  int rc = run();
  leave_raw();

  if (rc != 0) {
    fprintf(stderr, "tsun crashed: %s\nni-it wasn't MY fault... probably.\n",
            errbuf);
    return 1;
  }
  if (final_msg) {
    printf("%s\n", final_msg);
    fflush(stdout);
  }
  return 0;
}
