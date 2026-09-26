/*
** eaccess.c - accessibility: mme speaks for itself (a screen reader cannot
** read a window that draws its own text), and plays VS Code's
** accessibility signals.
**
** editor.accessibilitySupport, VS Code's: "on" speaks, "off" does not,
** "auto" speaks in mme-sdl when Windows says a screen reader runs (the
** terminal build leaves "auto" to the screen reader, which reads the
** console already). What is said, as VS Code with a screen reader: the line
** the caret moves to, the character or the word it moves over, the
** selection, the file of the tab in front, the item of a list, a dialog and
** its buttons, a notification. Accessible View (Alt+F2) shows the hover, a
** notification, Chat's answer or the terminal as text to read line by line.
**
** The voice: Windows' own (SAPI's SpVoice, through COM looked up at run
** time: Windows XP has it too), else spd-say (Linux's speech-dispatcher) or
** say (macOS). MME_SPEECH_LOG=file writes what is said there (the tests).
**
** The signals: accessibility.signals.<name>.sound "auto" (on while mme
** speaks), "on", "off", for lineHasError, lineHasWarning, lineHasBreakpoint,
** lineHasFoldedArea, taskCompleted, taskFailed, save, chatResponseReceived.
** Windows plays its system sounds; mme-sdl elsewhere a tone of its own; a
** terminal elsewhere its bell.
*/

#include "mme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


static int g_on = -1;	/* speaking: -1 not looked yet */
static char *g_log;	/* MME_SPEECH_LOG */


#ifdef _WIN32
/*
** SAPI through COM with nothing linked: ole32's two calls looked up, and
** SpVoice's methods by their place in its table (ISpVoice: IUnknown 3,
** ISpNotifySource 7, ISpEventSource 3, then SetOutput ... Speak is the 21st)
*/
typedef long (__stdcall *CoInitFn) (void *, unsigned long);
typedef long (__stdcall *CoCreateFn) (const GUID *, void *, unsigned long, const GUID *, void **);
typedef long (__stdcall *SpeakFn) (void *, const wchar_t *, unsigned long, unsigned long *);
typedef long (__stdcall *SetRateFn) (void *, long);
typedef long (__stdcall *SetVolumeFn) (void *, unsigned short);
typedef unsigned long (__stdcall *ReleaseFn) (void *);

static void *g_voice;
static int g_voice_tried;

static void *voice (void) {
  static const GUID clsid = {0x96749377, 0x3391, 0x11D2, {0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96}};
  static const GUID iid = {0x6C44DF74, 0x72B9, 0x4992, {0xA1, 0xEC, 0xEF, 0x99, 0x6E, 0x04, 0x22, 0xD4}};
  HMODULE ole;
  CoInitFn init;
  CoCreateFn create;
  if (g_voice_tried) return g_voice;
  g_voice_tried = 1;
  if ((ole = LoadLibraryW(L"ole32.dll")) == NULL) return NULL;
  init = (CoInitFn)(void (*)(void))GetProcAddress(ole, "CoInitializeEx");
  create = (CoCreateFn)(void (*)(void))GetProcAddress(ole, "CoCreateInstance");
  if (init == NULL || create == NULL) return NULL;
  init(NULL, 0x2);	/* COINIT_APARTMENTTHREADED; already so (SDL's): fine */
  if (create(&clsid, NULL, 0x1 | 0x4, &iid, &g_voice) != 0) g_voice = NULL;	/* CLSCTX_INPROC_SERVER | LOCAL_SERVER */
  if (g_voice) {	/* SetRate, SetVolume: the 29th and the 31st */
    int rate = (int)json_num(settings_get("mme\\.accessibility\\.speechRate"), 2);
    int vol = (int)json_num(settings_get("mme\\.accessibility\\.volume"), 100);
    ((SetRateFn)(*(void ***)g_voice)[28])(g_voice, rate < -10 ? -10 : rate > 10 ? 10 : rate);
    ((SetVolumeFn)(*(void ***)g_voice)[30])(g_voice, (unsigned short)(vol < 0 ? 0 : vol > 100 ? 100 : vol));
  }
  return g_voice;
}


static void speak (const char *text) {
  void *v = voice();
  int n;
  wchar_t *w;
  if (v == NULL) return;
  n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (n <= 0) return;
  w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, text, -1, w, n);
  ((SpeakFn)(*(void ***)v)[20])(v, w, 0x1 | 0x2 | 0x10, NULL);	/* SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML */
  free(w);
}


static int screen_reader (void) {	/* SPI_GETSCREENREADER: NVDA, Narrator, JAWS say so */
  BOOL on = FALSE;
  return SystemParametersInfoW(0x0046, 0, &on, 0) && on;
}


static void sound (int sig) {
  static const UINT beep[] = {MB_ICONHAND, MB_ICONEXCLAMATION, MB_ICONASTERISK, MB_OK, MB_ICONASTERISK, MB_ICONHAND,
                              MB_OK, MB_ICONASTERISK};
  MessageBeep(beep[sig]);
}

#else

static long g_say_pid;

static void speak (const char *text) {	/* spd-say, else say: one at a time, the one before cut short */
  static int which = -1;	/* 0 none, 1 spd-say, 2 say */
  char *argv[5], *exe = NULL;
  if (which < 0) {
    char *p;
    which = 0;
    if ((p = find_program("spd-say")) != NULL) which = 1;
    else if ((p = find_program("say")) != NULL) which = 2;
    free(p);
  }
  if (which == 0) return;
  if (g_say_pid > 0) os_kill(g_say_pid, 15);
  g_say_pid = 0;
  exe = find_program(which == 1 ? "spd-say" : "say");
  if (exe == NULL) return;
  argv[0] = exe;
  if (which == 1) {
    argv[1] = (char *)"-C";	/* what it says now stops */
    argv[2] = NULL;
    spawn_detached(argv);
    argv[1] = (char *)"--";
    argv[2] = (char *)text;
    argv[3] = NULL;
  }
  else {
    argv[1] = (char *)"--";
    argv[2] = (char *)text;
    argv[3] = NULL;
  }
  {
    OsProc proc;
    int io[3] = {-1, -1, -1};
    if (os_spawn(exe, argv, NULL, io, 3, &proc, &g_say_pid) == 0) os_detach(proc);
  }
  free(exe);
}


static int screen_reader (void) {
  return 0;
}


static void sound (int sig) {
  if (term_tone(sig) != 0 && (sig == SIG_ERROR || sig == SIG_TASK_FAILED)) term_write("\a", 1);	/* a terminal: its bell */
}

#endif


int acc_on (void) {
  if (g_on < 0) {
    const char *s = json_str(settings_get("editor\\.accessibilitySupport"), "auto");
    char *log = os_getenv("MME_SPEECH_LOG");
    if (log && *log) g_log = log;
    else free(log);
    g_on = strcmp(s, "on") == 0 || (strcmp(s, "auto") == 0 && term_can_raise() && screen_reader());
  }
  return g_on;
}


void acc_settings_changed (void) {	/* editor.accessibilitySupport again */
  g_on = -1;
}


/* text said, what was being said cut short; nothing while mme does not speak */
void acc_say (const char *text) {
  if (!acc_on() || text == NULL || *text == '\0') return;
  if (g_log) {
    int fd = os_open(g_log, OS_APPEND);
    if (fd >= 0) {
      os_write(fd, text, strlen(text));
      os_write(fd, "\n", 1);
      os_close(fd);
    }
    return;
  }
  speak(text);
}


void acc_sayf (const char *fmt, ...) {
  char b[1024];
  va_list ap;
  if (!acc_on()) return;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  acc_say(b);
}


/* a signal's sound, as its accessibility.signals.<name>.sound says */
void acc_signal (int sig) {
  static const char *const name[] = {"lineHasError", "lineHasWarning", "lineHasBreakpoint", "lineHasFoldedArea",
                                     "taskCompleted", "taskFailed", "save", "chatResponseReceived"};
  char key[96];
  const char *s;
  if (sig < 0 || sig >= SIG_N) return;
  snprintf(key, sizeof(key), "accessibility\\.signals\\.%s\\.sound", name[sig]);
  s = json_str(settings_get(key), "auto");
  if (strcmp(s, "off") == 0 || (strcmp(s, "auto") == 0 && !acc_on())) return;
  if (g_log) {
    acc_sayf("[sound %s]", name[sig]);
    return;
  }
  sound(sig);
}


/* a character said by its name when it has no sound of its own */
const char *acc_char_name (unsigned c) {
  static const struct {
    unsigned c;
    const char *name;
  } names[] = {
    {' ', "space"}, {'\t', "tab"}, {'.', "dot"}, {',', "comma"}, {';', "semicolon"}, {':', "colon"},
    {'(', "left paren"}, {')', "right paren"}, {'[', "left bracket"}, {']', "right bracket"},
    {'{', "left brace"}, {'}', "right brace"}, {'<', "less"}, {'>', "greater"}, {'=', "equals"},
    {'+', "plus"}, {'-', "dash"}, {'*', "star"}, {'/', "slash"}, {'\\', "backslash"}, {'"', "quote"},
    {'\'', "tick"}, {'`', "grave"}, {'!', "bang"}, {'?', "question"}, {'&', "and"}, {'|', "bar"},
    {'^', "caret"}, {'%', "percent"}, {'$', "dollar"}, {'#', "number"}, {'@', "at"}, {'~', "tilde"},
    {'_', "underline"}
  };
  size_t i;
  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (names[i].c == c) return names[i].name;
  return NULL;
}
