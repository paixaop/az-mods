/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Beatport Remember: keeps the Beatport Streaming login on the deck.
 *
 * The XDJ-AZ asks for the Beatport username and password on its own login
 * screen, and forgets them when the deck is powered off. This mod remembers the
 * credentials once they have been typed in on that screen (nothing new to
 * enter), stores them on the deck, and the next time the login screen opens it
 * fills both fields and presses LOG IN for you.
 *
 * Everything happens inside EP147 through two hooks, so the mod is tied to one
 * EP147 build (firmware tag ep147-9e9454e357389ece, XDJ-AZ firmware 2.00):
 *
 *   0x1b8e3a8  BeatportLoginWidgetController(BeatportLoginWidget* widget)
 *              runs when the login screen is created; gives us both objects.
 *   0x1b8dfa8  the controller's Button::Listener callback (one of its vtable
 *              slots); it is where a LOG IN press becomes a Beatport session.
 *
 * The login widget keeps its two editors at +0xf0 (user name) and +0xf8
 * (password) and the LOG IN button at +0x118. The mod uses EP147's own
 * juce::TextEditor::setText/getText, so the fields hold real text. No firmware
 * file is modified; the hooks live only in RAM.
 */

#include "azmod-mod.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

#define MOD_ID "beatport-remember"

/* ---- EP147 2.00 addresses (tag ep147-9e9454e357389ece) ---------------------- */
#define FW_TAG              "ep147-9e9454e357389ece"
#define A_CTRL_CTOR         0x1b8e3a8u /* BeatportLoginWidgetController(Widget*) */
#define A_BTN_DISPATCH      0x1b8dfa8u /* controller Button::Listener callback   */
#define A_STRING_CTOR       0x24a1180u /* juce::String::String(const char*)      */
#define A_STRING_DTOR       0x2469aa0u /* juce::String::~String()                */
#define A_STRING_LEN        0x247f550u /* juce::String::length()                 */
#define A_EDITOR_SET_TEXT   0x2647670u /* TextEditor::setText(String&, bool)     */
#define A_EDITOR_GET_TEXT   0x263f4e0u /* TextEditor::getText() (sret in x8)     */

/* EP147's four widget-name juce::Strings, built by its static initialiser
 * (_INIT_1546 at 0x5716d0) in .bss:
 *
 *   0x3feee80 "userNameDelBtn"    0x3feee88 "passwordDelBtn"
 *   0x3feee90 "passwordMaskBtn"   0x3feee98 "loginBtn"
 *
 * The callback at A_BTN_DISPATCH does not take the pressed juce::Button: it
 * takes a pointer to a juce::String name and pointer-compares its first word
 * with the first word of one of those four globals:
 *
 *   ldr x0, [x1]                  // the name's StringHolder*
 *   ldr x1, [x2, #0xe98]          // EP147's own "loginBtn" holder
 *   cmp x0, x1 ; b.eq login       // -> 0x1b8e008
 *
 * Passing the address of EP147's own loginBtn String therefore selects the
 * LOG IN branch by construction: the callee reads that one word and neither
 * stores nor destroys the String. (Passing the Button* instead, as 0.1.4/0.1.5
 * did, leaves x0 = the button's vtable pointer, matches none of the four and
 * returns at once - which is why the automatic press did nothing.) */
#define A_LOGINBTN_NAME     0x3feee98u
/* EP147's login button is a toggle: 0 = LOG IN, 1 = LOG OUT / cancel. */
#define B_TOGGLE_STATE      0x1e0u

/* Members of BeatportLoginWidget. */
#define W_USER_EDITOR       0xf0u
#define W_PASS_EDITOR       0xf8u
#define W_LOGIN_BUTTON      0x118u
/* The controller keeps its widget at +0x38. */
#define C_WIDGET            0x38u

/* Vtable address points, used only to check that an object is still alive. */
#define V_WIDGET            0x2d8d5e8u
#define V_CONTROLLER        0x2d91bd0u

/* Ticks at up to ~30 Hz before typing into a freshly created login screen. */
#define AUTOFILL_DELAY_TICKS 12

/* ---- stored credentials ----------------------------------------------------- */

#define CRED_DIR  "/home/root/settings/azmod-community"
#define CRED_FILE CRED_DIR "/beatport-remember.dat"

#define MAX_CRED 127

static const az_core_v1 *C;

static int auto_login = 1;   /* fill and press LOG IN */
static int remember   = 1;   /* capture what the DJ types */
/* The press is late on purpose. EP147 builds the Beatport login screen ~1.5 s
 * after start, but its Beatport media device is not registered yet: a press
 * that early runs usecase::beatport::BeatportSession::start() with a mediaId
 * whose device type is not kBeatport, which EP147 rejects with an ERR and
 * leaves the streaming client stuck on "connecting". The delay is tunable from
 * the settings file ("delay-sec", "attempts") so it can be found on a deck
 * without rebuilding the mod. */
#define DEFAULT_DELAY_SEC  45
#define DEFAULT_ATTEMPTS    1
#define RETRY_GAP_MS      8000
#define BOOT_WATCH_TICKS 900   /* ~30 s at the core's tick rate */

static int have_creds;
static char saved_user[MAX_CRED + 1];
static char saved_pass[MAX_CRED + 1];

static void *g_widget;
static void *g_ctrl;
static int pending_ticks;
static int attempted;
static int press_ticks;   /* ticks until the next automatic LOG IN press */
static int verify_ticks;  /* ticks until the outcome of the last press is reported */
static int presses;       /* automatic presses made on this login screen */
static int delay_sec = DEFAULT_DELAY_SEC;   /* wait this long after the screen appears */
static int max_attempts = DEFAULT_ATTEMPTS;
static long long press_at_ms;               /* 0 = nothing scheduled */
static int saw_login_screen;   /* the Beatport login screen was built at least once */
static int boot_watch_ticks;   /* ticks left before reporting that it never was */

static long long t0_ms;        /* CLOCK_MONOTONIC at init, for the boot timeline */

static long long since_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000);
}

static long long t_ms(void) { return since_ms() - t0_ms; }

typedef void (*fn_void_ptr)(void *, void *);
static fn_void_ptr orig_ctrl_ctor;
static fn_void_ptr orig_btn_dispatch;

/* juce::String is one pointer here (to its UTF-8 text holder). */
typedef struct { const char *text; } jstring;

static void js_ctor(jstring *s, const char *utf8)
{
    ((void (*)(jstring *, const char *))A_STRING_CTOR)(s, utf8);
}

static void js_dtor(jstring *s)
{
    ((void (*)(jstring *))A_STRING_DTOR)(s);
}

static int js_len(const jstring *s)
{
    return ((int (*)(const jstring *))A_STRING_LEN)(s);
}

/* TextEditor::getText() returns juce::String through the hidden x8 pointer.
 * GCC 7 does not implement the aarch64 naked attribute, so the trampoline is
 * a hand-written assembly symbol: no prologue, x8 <- out, x0 <- editor, then
 * a branch that loads the constant itself (the callee may clobber x16). */
#if defined(__aarch64__)
__asm__(
    ".pushsection .text\n"
    ".hidden bp_editor_get_text\n"
    ".type bp_editor_get_text, %function\n"
    "bp_editor_get_text:\n"
    ".cfi_startproc\n"
    "  stp x29, x30, [sp, #-16]!\n"
    ".cfi_def_cfa_offset 16\n"
    ".cfi_offset 29, -16\n"
    ".cfi_offset 30, -8\n"
    "  mov x8, x0\n"
    "  mov x0, x1\n"
    "  movz x16, #0xf4e0\n"
    "  movk x16, #0x263, lsl #16\n"
    "  blr x16\n"
    "  ldp x29, x30, [sp], #16\n"
    ".cfi_def_cfa_offset 0\n"
    ".cfi_restore 29\n"
    ".cfi_restore 30\n"
    "  ret\n"
    ".cfi_endproc\n"
    ".size bp_editor_get_text, .-bp_editor_get_text\n"
    ".popsection\n");
extern void bp_editor_get_text(jstring *out, void *editor);
#define editor_get_text bp_editor_get_text
#else
static void editor_get_text(jstring *out, void *editor) { (void)out; (void)editor; }
#endif

static void editor_set_text(void *editor, const char *utf8)
{
    jstring s;
    js_ctor(&s, utf8);
    ((void (*)(void *, const jstring *, int))A_EDITOR_SET_TEXT)(editor, &s, 1);
    js_dtor(&s);
}

/* ---- small helpers ---------------------------------------------------------- */

static int str_is_empty(const jstring *s)
{
    const char *p = s->text;
    return !p || !*p;
}

static void copy_text(char *dst, int dst_len, const jstring *s)
{
    int n;
    if (!s->text) {
        dst[0] = 0;
        return;
    }
    n = js_len(s);
    if (n < 0) n = 0;
    if (n > dst_len - 1) n = dst_len - 1;
    memcpy(dst, s->text, (size_t)n);
    dst[n] = 0;
}

/* Hex on disk so a password with spaces, '=' or newlines round-trips. */
static void to_hex(const char *in, char *out, int out_len)
{
    static const char hex[] = "0123456789abcdef";
    int i, n = (int)strlen(in);
    if (n > (out_len - 1) / 2) n = (out_len - 1) / 2;
    for (i = 0; i < n; i++) {
        out[2 * i] = hex[((unsigned char)in[i]) >> 4];
        out[2 * i + 1] = hex[((unsigned char)in[i]) & 15];
    }
    out[2 * n] = 0;
}

static int from_hex(const char *in, char *out, int out_len)
{
    int i, n = (int)strlen(in);
    if (n % 2) return -1;
    if (n / 2 > out_len - 1) return -1;
    for (i = 0; i < n / 2; i++) {
        int hi, lo;
        char a = in[2 * i], b = in[2 * i + 1];
        hi = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10
             : a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
        lo = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10
             : b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (char)((hi << 4) | lo);
    }
    out[n / 2] = 0;
    return 0;
}

static int credible_credential(const char *s, int n);

static void save_credentials(void)
{
    char user_hex[2 * MAX_CRED + 2];
    char pass_hex[2 * MAX_CRED + 2];
    char buf[2 * (2 * MAX_CRED + 2) + 32];
    int fd, n;

    if (!have_creds) return;

    (void)mkdir(CRED_DIR, 0700);
    to_hex(saved_user, user_hex, (int)sizeof(user_hex));
    to_hex(saved_pass, pass_hex, (int)sizeof(pass_hex));
    n = snprintf(buf, sizeof(buf), "user=%s\npass=%s\n", user_hex, pass_hex);
    if (n <= 0) return;

    fd = open(CRED_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        C->log(MOD_ID, "could not save credentials: %s", strerror(errno));
        return;
    }
    if (write(fd, buf, (size_t)n) != n) {
        C->log(MOD_ID, "short write saving credentials: %s", strerror(errno));
        close(fd);
        return;
    }
    (void)fchmod(fd, 0600);
    close(fd);
    C->log(MOD_ID, "saved Beatport login (user=%d bytes pass=%d bytes)",
           (int)strlen(saved_user), (int)strlen(saved_pass));
}

static void load_credentials(void)
{
    char buf[sizeof(saved_user) * 2 + sizeof(saved_pass) * 2 + 32];
    char *line, *next;
    int fd, n;

    have_creds = 0;
    saved_user[0] = saved_pass[0] = 0;

    fd = open(CRED_FILE, O_RDONLY);
    if (fd < 0) return;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    for (line = buf; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "user=", 5))
            (void)from_hex(line + 5, saved_user, (int)sizeof(saved_user));
        else if (!strncmp(line, "pass=", 5))
            (void)from_hex(line + 5, saved_pass, (int)sizeof(saved_pass));
    }
    if (saved_user[0] && saved_pass[0] &&
        credible_credential(saved_user, (int)strlen(saved_user)) &&
        credible_credential(saved_pass, (int)strlen(saved_pass))) {
        have_creds = 1;
        C->log(MOD_ID, "loaded saved Beatport login (user=%d bytes pass=%d bytes)",
               (int)strlen(saved_user), (int)strlen(saved_pass));
    } else {
        have_creds = 0;
        saved_user[0] = saved_pass[0] = 0;
        if (n > 0) C->log(MOD_ID, "ignored malformed saved login");
    }
}

static void forget_credentials(void)
{
    have_creds = 0;
    saved_user[0] = saved_pass[0] = 0;
    (void)unlink(CRED_FILE);
    C->log(MOD_ID, "saved Beatport login forgotten");
}

/* ---- stale DRM shared memory ------------------------------------------------
 *
 * EP147's cipher_createSharedMemory() (0xed0cc0) does
 *   ftok("/etc", 0x14); shmget(key, 0x340, IPC_CREAT|IPC_EXCL|0600)
 * and treats EEXIST as a hard error: it never falls back to the existing
 * segment. A normal EP147 exit leaves the segment behind with nattch 0, so the
 * next start (AZ-MODS Reload, systemctl restart, a crashed session) cannot
 * initialize the cipher and streaming tracks never finish "downloading".
 * Removing an unattached stale segment before EP147 uses the cipher fixes it.
 * Only a 0x340-byte, mode-0600 segment with no attached process is removed. */
static void clear_stale_cipher_shm(void)
{
    key_t key = ftok("/etc", 0x14);
    struct shmid_ds ds;
    int id;

    if (key == (key_t)-1) return;
    id = shmget(key, 0, 0);
    if (id < 0) return;
    if (shmctl(id, IPC_STAT, &ds) != 0) return;
    if (ds.shm_nattch != 0 || ds.shm_segsz != 0x340 || ds.shm_perm.mode != 0600) return;
    if (shmctl(id, IPC_RMID, NULL) == 0 && C)
        C->log(MOD_ID, "removed stale cipher shared memory");
}

/* Runs at dlopen, before EP147's main thread starts the cipher. */
__attribute__((constructor))
static void beatport_remember_ctor(void)
{
    clear_stale_cipher_shm();
}

/* ---- reading the two editors ------------------------------------------------ */

static void *widget_member(void *widget, unsigned off)
{
    return *(void **)((char *)widget + off);
}

static int widget_still_valid(void)
{
    if (!g_widget) return 0;
    return *(unsigned long *)g_widget == (unsigned long)V_WIDGET;
}

static int controller_still_valid(void)
{
    if (!g_ctrl) return 0;
    return *(unsigned long *)g_ctrl == (unsigned long)V_CONTROLLER;
}

static int credible_credential(const char *s, int n);

/* process_vm_readv on our own process: returns an error instead of raising
 * SIGSEGV for an unmapped address, so a wrong guess cannot crash the deck. */
static int safe_copy(void *dst, const void *src, size_t len)
{
#if defined(__aarch64__) && defined(SYS_process_vm_readv)
    struct iovec local = { dst, len };
    struct iovec remote = { (void *)src, len };
    long n = syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL);
    return n == (long)len;
#else
    memcpy(dst, src, len);
    return 1;
#endif
}

/* Read one member with the same protection as the text reads: an unmapped or
 * already released address is an error, never a SIGSEGV on the deck. */
static int read_u32(const void *addr, unsigned int *out)
{
    return safe_copy(out, addr, sizeof(*out));
}

/* Reading the editors goes through EP147's own juce::TextEditor::getText:
 * the text lives in the editor's internal document model (see 0x263f4e0), and
 * guessing one member offset reads unrelated data. The returned juce::String
 * is one pointer to its UTF-8 text; bytes are copied with process_vm_readv
 * (safe for a bad pointer) and the String is destroyed by EP147's dtor. */
static int credible_credential(const char *s, int n)
{
    int i;
    if (n <= 0 || n > MAX_CRED) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0; /* no control characters */
    }
    return 1;
}

static int editor_text(void *editor, char *out, int out_len)
{
    jstring s;
    const char *p;
    int n = 0;

    out[0] = 0;
    if (!editor || out_len < 2) return 0;
    memset(&s, 0, sizeof(s));
    editor_get_text(&s, editor);
    p = s.text;
    if (p) {
        while (n < out_len - 1) {
            char c;
            if (!safe_copy(&c, p + n, 1)) { n = 0; break; }
            if (!c) break;
            out[n++] = c;
        }
        out[n] = 0;
    }
    js_dtor(&s);
    if (!credible_credential(out, n)) {
        out[0] = 0;
        return 0;
    }
    return n;
}

static void read_login_fields(char *user, int user_len, char *pass, int pass_len,
                              int *user_empty, int *pass_empty)
{
    user[0] = pass[0] = 0;
    *user_empty = *pass_empty = 1;
    if (!widget_still_valid()) return;
    if (editor_text(widget_member(g_widget, W_USER_EDITOR), user, user_len) > 0)
        *user_empty = 0;
    if (editor_text(widget_member(g_widget, W_PASS_EDITOR), pass, pass_len) > 0)
        *pass_empty = 0;
}

static void *login_button(void)
{
    if (!widget_still_valid()) return NULL;
    return widget_member(g_widget, W_LOGIN_BUTTON);
}

/* ---- capture ---------------------------------------------------------------- */

static void capture_from_login_screen(void)
{
    char user[MAX_CRED + 1], pass[MAX_CRED + 1];
    int user_empty, pass_empty;

    if (!remember) return;
    read_login_fields(user, sizeof(user), pass, sizeof(pass), &user_empty, &pass_empty);
    if (user_empty || pass_empty) return; /* empty fields are not a login */
    if (have_creds && !strcmp(user, saved_user) && !strcmp(pass, saved_pass)) return;

    if (!credible_credential(user, (int)strlen(user)) ||
        !credible_credential(pass, (int)strlen(pass))) return;
    snprintf(saved_user, sizeof(saved_user), "%s", user);
    snprintf(saved_pass, sizeof(saved_pass), "%s", pass);
    have_creds = 1;
    C->log(MOD_ID, "captured Beatport login (user=%d bytes pass=%d bytes)",
           (int)strlen(user), (int)strlen(pass));
    save_credentials();
}

/* ---- automatic sign-in ------------------------------------------------------ */

static void try_auto_login(void)
{
    void *button;

    if (!auto_login || !have_creds || attempted) return;
    if (!widget_still_valid() || !controller_still_valid()) return;
    button = login_button();
    if (!button) return;

    {
        char cur_user[MAX_CRED + 1], cur_pass[MAX_CRED + 1];
        int ue, pe;
        read_login_fields(cur_user, sizeof(cur_user), cur_pass, sizeof(cur_pass), &ue, &pe);
        if (!ue || !pe) return; /* something is already in the fields: leave it */
    }
    attempted = 1;
    editor_set_text(widget_member(g_widget, W_USER_EDITOR), saved_user);
    editor_set_text(widget_member(g_widget, W_PASS_EDITOR), saved_pass);
    C->log(MOD_ID, "filled the Beatport login at +%lld ms; press scheduled at +%lld ms",
           t_ms(), t_ms() + (long long)delay_sec * 1000LL);
    press_at_ms = t_ms() + (long long)delay_sec * 1000LL;
}

/* Press LOG IN the way a finger does: the screen's callback is entered with
 * EP147's own "loginBtn" name, and it reads the two editors itself.
 *
 * The press happens only when:
 *   - both fields still hold exactly the values we filled (typed text is never
 *     submitted),
 *   - the button is in its LOG IN state (state 1 is LOG OUT; pressing it would
 *     sign the DJ out),
 *   - EP147's loginBtn name String is already built (otherwise the name would
 *     match no branch and the press would be a silent no-op). */
static void try_press_login(void)
{
    void *button;
    unsigned int state = 0, name_holder = 0;
    char u[MAX_CRED + 1], p[MAX_CRED + 1];
    int ue, pe;

    if (!auto_login || !have_creds || !attempted) return;
    if (!widget_still_valid() || !controller_still_valid()) return;
    button = login_button();
    if (!button) return;

    if (!read_u32((const char *)button + B_TOGGLE_STATE, &state)) {
        C->log(MOD_ID, "login button state unreadable, not pressing");
        return;
    }
    if (state != 0) {
        C->log(MOD_ID, "login button is in state %u (signed in), not pressing", state);
        presses = max_attempts;  /* stop retrying */
        return;
    }
    if (!read_u32((const void *)A_LOGINBTN_NAME, &name_holder) || name_holder == 0) {
        C->log(MOD_ID, "EP147 loginBtn name is not initialised, not pressing");
        return;
    }

    read_login_fields(u, sizeof(u), p, sizeof(p), &ue, &pe);
    if (ue || pe) return;
    if (strcmp(u, saved_user) || strcmp(p, saved_pass)) return;

    presses++;
    C->log(MOD_ID, "pressing LOG IN automatically, attempt %d/%d at +%lld ms (name %x)",
           presses, max_attempts, t_ms(), name_holder);
    orig_btn_dispatch(g_ctrl, (void *)A_LOGINBTN_NAME);
    C->log(MOD_ID, "LOG IN dispatch returned (+%lld ms)", t_ms());
    if (presses < max_attempts)
        press_at_ms = t_ms() + RETRY_GAP_MS;
    else
        verify_ticks = 45;               /* ~1.5 s later: report what happened */
}

/* What the login screen did with the automatic press. A state of 1 means the
 * button turned into LOG OUT, i.e. EP147 accepted the session; a stale object
 * means the screen closed. Either way this line is the deck-side verdict. */
static void report_login_outcome(void)
{
    void *button;
    unsigned int state = 0;

    if (!widget_still_valid()) {
        C->log(MOD_ID, "after auto login: login screen is gone (+%lld ms)", t_ms());
        return;
    }
    button = login_button();
    if (button && read_u32((const char *)button + B_TOGGLE_STATE, &state))
        C->log(MOD_ID, "after auto login: screen still open, button state %u (+%lld ms)",
               state, t_ms());
    else
        C->log(MOD_ID, "after auto login: screen still open, state unreadable (+%lld ms)",
               t_ms());
}

/* If EP147 never builds the Beatport login screen, the deck is not waiting for
 * a streaming login at start-up at all; the automatic sign-in can then only
 * happen once the DJ opens Beatport. This line says which one it was. */
static void report_boot_state(void)
{
    if (saw_login_screen) return;
    C->log(MOD_ID, "no Beatport login screen in the first ~30 s after start");
}

/* ---- hooks ------------------------------------------------------------------ */

static void hook_ctrl_ctor(void *self, void *widget)
{
    orig_ctrl_ctor(self, widget);
    g_ctrl = self;
    g_widget = widget;
    attempted = 0;
    presses = 0;
    press_ticks = 0;
    press_at_ms = 0;
    verify_ticks = 0;
    saw_login_screen = 1;
    pending_ticks = AUTOFILL_DELAY_TICKS;
    {
        char b[128];
        snprintf(b, sizeof(b), "login screen built at +%lld ms (self=%llx widget=%llx)",
                 t_ms(), (unsigned long long)(unsigned long)self,
                 (unsigned long long)(unsigned long)widget);
        C->log(MOD_ID, "%s", b);
    }
}

static void hook_btn_dispatch(void *self, void *button)
{
    /* Any button event on the login screen is a chance to remember what the DJ
     * has typed. capture_from_login_screen() only saves when both fields hold
     * text, so clear/back/mask buttons just pass through. */
    if (remember && widget_still_valid())
        capture_from_login_screen();
    orig_btn_dispatch(self, button);
}

/* ---- Mod Menu page ---------------------------------------------------------- */

static const char *status_line(void)
{
    static char line[MAX_CRED + 48];
    if (!have_creds) return "No saved login yet. Log in once on the Beatport screen.";
    snprintf(line, sizeof(line), "Saved login: %s", saved_user);
    return line;
}

static az_rect g_auto_row   = { 0, 0, 0, 0 };
static az_rect g_rem_row    = { 0, 0, 0, 0 };
static az_rect g_forget_btn = { 0, 0, 0, 0 };

static int rect_hit(const az_rect *r, const az_event *ev)
{
    return r->w > 0 && r->h > 0 &&
           ev->x >= r->x && ev->x < r->x + r->w &&
           ev->y >= r->y && ev->y < r->y + r->h;
}

static void draw_toggle(az_draw *d, az_rect area, int y, int row, int on, const char *label,
                        az_rect *out)
{
    int px = row / 2;
    int cb = row / 2;
    if (px < 8) px = 8;
    if (px > 96) px = 96;
    if (cb < 12) cb = 12;

    *out = (az_rect){ 0, 0, 0, 0 };
    if (y + row > area.y + area.h) return;

    C->draw_checkbox(d, area.x + row / 2, y + (row - cb) / 2, cb, on, 0);
    C->draw_text(d, area.x + row / 2 + cb + row / 2,
                 y + (row - px * 3 / 2) / 2, label, px, C->color(AZ_COL_TEXT));
    *out = (az_rect){ area.x, y, area.w, row };
}

static void draw_page(az_draw *d, az_rect area, void *user)
{
    const int row = C->row_height(d);
    const int px = row / 2 < 8 ? 8 : (row / 2 > 96 ? 96 : row / 2);
    int y;
    (void)user;

    C->draw_header(d, "Beatport Remember");
    g_auto_row = g_rem_row = g_forget_btn = (az_rect){ 0, 0, 0, 0 };

    y = area.y + row + row / 4;
    if (y + px * 3 / 2 <= area.y + area.h)
        C->draw_text(d, area.x + row / 2, y, status_line(), px, C->color(AZ_COL_TEXT_DIM));
    y += row + row / 4;

    draw_toggle(d, area, y, row, auto_login, "Auto login (sign in at start-up)", &g_auto_row);
    y += row;
    draw_toggle(d, area, y, row, remember, "Remember a login typed on the deck", &g_rem_row);
    y += row + row / 2;

    if (have_creds) {
        int tw = C->text_width("Forget saved login", px);
        int bw = tw + row;
        int bh = row * 3 / 4;
        int bx = area.x + row / 2;
        if (tw > 0 && y + bh <= area.y + area.h) {
            C->fill_rect(d, bx, y, bw, bh, C->color(AZ_COL_HI));
            C->draw_text(d, bx + row / 2, y + (bh - px * 3 / 2) / 2,
                         "Forget saved login", px, C->color(AZ_COL_HI_TEXT));
            g_forget_btn = (az_rect){ bx, y, bw, bh };
        }
    } else    if (y + px * 3 / 2 <= area.y + area.h) {
        C->draw_text(d, area.x + row / 2, y, "Nothing saved to forget.",
                     px, C->color(AZ_COL_TEXT_DIM));
    }
}

static int page_input(const az_event *ev, void *user)
{
    (void)user;
    if (!ev || ev->kind != AZ_EV_TAP) return 0;

    if (rect_hit(&g_auto_row, ev)) {
        auto_login = !auto_login;
        C->cfg_set(MOD_ID, "auto-login", auto_login);
        C->log(MOD_ID, "auto login %s", auto_login ? "on" : "off");
        if (AZ_HAS(C, request_redraw) && C->request_redraw) C->request_redraw();
        return 1;
    }
    if (rect_hit(&g_rem_row, ev)) {
        remember = !remember;
        C->cfg_set(MOD_ID, "remember", remember);
        C->log(MOD_ID, "remember login %s", remember ? "on" : "off");
        if (AZ_HAS(C, request_redraw) && C->request_redraw) C->request_redraw();
        return 1;
    }
    if (have_creds && rect_hit(&g_forget_btn, ev)) {
        forget_credentials();
        C->log(MOD_ID, "saved Beatport login forgotten from the page button");
        if (AZ_HAS(C, request_redraw) && C->request_redraw) C->request_redraw();
        return 1;
    }
    return 0; /* BACK returns to the Mod Menu */
}

static const az_screen_v1 PAGE = {
    sizeof(az_screen_v1), draw_page, page_input, NULL, NULL
};

/* ---- Mod Menu rows ---------------------------------------------------------- */

/* ---- init / tick ------------------------------------------------------------ */

static int init(const az_core_v1 *core)
{
    static const az_row_v1 rows[] = {
        { .size = sizeof(az_row_v1), .key = "menu", .label = "Beatport Remember",
          .kind = AZ_ROW_SCREEN,
          .help = "Saved Beatport login, auto login and forget",
          .screen = &PAGE },
    };

    if (!core || core->abi != AZ_MOD_ABI) return 1;
    C = core;
    t0_ms = since_ms();
    boot_watch_ticks = BOOT_WATCH_TICKS;
    clear_stale_cipher_shm();

    if (!AZ_HAS(C, claim_hook) || !AZ_HAS(C, fw_tag)) return 2;
    if (!C->fw_tag || strcmp(C->fw_tag(), FW_TAG)) {
        C->log(MOD_ID, "not for this firmware (%s)", C->fw_tag ? C->fw_tag() : "?");
        return 3;
    }

    auto_login = C->cfg_get(MOD_ID, "auto-login", 1) ? 1 : 0;
    remember = C->cfg_get(MOD_ID, "remember", 1) ? 1 : 0;
    delay_sec = C->cfg_get(MOD_ID, "delay-sec", DEFAULT_DELAY_SEC);
    max_attempts = C->cfg_get(MOD_ID, "attempts", DEFAULT_ATTEMPTS);
    if (delay_sec < 0) delay_sec = 0;
    if (delay_sec > 600) delay_sec = 600;
    if (max_attempts < 1) max_attempts = 1;
    if (max_attempts > 4) max_attempts = 4;
    load_credentials();

    if (AZ_HAS(C, register_row)) {
        int i;
        for (i = 0; i < 1; i++) C->register_row(MOD_ID, &rows[i]);
    }

    /* Claim the hooks last: nothing below this can fail. */
    if (C->claim_hook(MOD_ID, A_CTRL_CTOR, (void *)hook_ctrl_ctor,
                      (void **)&orig_ctrl_ctor) != 0) {
        C->log(MOD_ID, "login controller hook refused");
        return 4;
    }
    if (C->claim_hook(MOD_ID, A_BTN_DISPATCH, (void *)hook_btn_dispatch,
                      (void **)&orig_btn_dispatch) != 0) {
        C->log(MOD_ID, "login button hook refused");
        return 5;
    }

    C->log(MOD_ID, "loaded on %s (auto=%d remember=%d creds=%d)",
           C->fw_tag(), auto_login, remember, have_creds);
    C->log(MOD_ID, "auto login delay %d s, up to %d attempt(s)", delay_sec, max_attempts);
    return 0;
}

static void tick(void)
{
    if (pending_ticks > 0 && --pending_ticks == 0) try_auto_login();
    if (press_at_ms > 0 && t_ms() >= press_at_ms) {
        press_at_ms = 0;
        try_press_login();
    }
    if (verify_ticks > 0 && --verify_ticks == 0) report_login_outcome();
    if (boot_watch_ticks > 0 && --boot_watch_ticks == 0) report_boot_state();
}

static const char *const FW_TAGS[] = { FW_TAG, NULL };

static const az_mod_v1 MOD = {
    AZ_MOD_ABI, sizeof(az_mod_v1), MOD_ID, FW_TAGS, init, tick, NULL
};

const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
