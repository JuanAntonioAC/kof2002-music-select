#include "bgm.h"
#include "library.h"
#include "menu.h"
#include "str.h"
#include "winapi.h"

typedef HANDLE (WINAPI *CreateFileW_fn)(const wch*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
typedef DWORD (WINAPI *XInputGetState_fn)(DWORD, void*);

typedef struct XPad {
    u16 buttons;
    u8 lt, rt;
    short lx, ly, rx, ry;
} XPad;

typedef struct XState {
    DWORD packet;
    XPad pad;
} XState;

/* Same layout as AppMusicPad in KofVsSelector.dll. */
typedef struct VsPad {
    int id;
    int side;
    XPad pad;
} VsPad;

typedef int (WINAPI *VsPads_fn)(VsPad*, int);
typedef void (WINAPI *VsMenu_fn)(int);

#define ROW_H 28
#define ROW_N 14
#define LIST_TOP 124
#define GAME_NAME "KOF 2002"
#define GAME_ALBUM "KOF 2002 UM - Soundtrack"
#define GAME_RATE 48000

#define PLAY_RVA 0x7DF00u
#define CURRENT_RVA 0x7C63F4u
#define BGM_ENTRY_RVA 0x7C6400u
#define BGM_STRIDE 0x14Cu
#define BGM_PATH_OFF 0x40u
#define BGM_LOOP_OFF 0x140u
#define BGM_SLOTS 90
#define PREVIEW_CARRIER "BGM15"
#define WM_PLAY (WM_APP + 1)

static HMODULE g_self;
static CreateFileW_fn g_create;
static XInputGetState_fn g_xinput;
static Library g_libs[2];
static Library* volatile g_lib = &g_libs[0];
#define g_cat (g_lib->cat)
static wchar_t g_bgm_dir[MAX_PATH];
static wchar_t g_music_dir[2][MAX_PATH];
static wchar_t g_loop0[MAX_PATH];
static int g_lib_built;
static LibItem g_items[LIB_ITEMS];
static int g_item_count;
static int g_folder;
static MenuInput g_menu;
static u32 g_key_prev;
static VsPads_fn g_vs_pads;
static VsMenu_fn g_vs_menu;
static u64 g_vs_probe;
static CRIT_ g_lock;
static HWND g_hwnd;
static wchar_t g_dll_dir[MAX_PATH];
static wchar_t g_log_path[MAX_PATH];
static char g_preview_extra[BGM_STEM];
static char g_redir_from[BGM_STEM];
static char g_redir_to[BGM_STEM];
static int g_prev_pending;
static u64 g_prev_at;
static char g_choice[BGM_STEM];
static int g_random;
static int g_index;
static int g_scroll;
static volatile int g_visible;
static volatile int g_block_input;
static volatile long g_request_show;
static volatile long g_ask;
static int g_phase;
static u64 g_last_show;
static u64 g_ps_tick;
static int g_after_fight;
static int g_await_order;
static char g_prev_stem[BGM_STEM];
static u64 g_prev_open_at;
static volatile long g_request_hide;
static int g_preview_logged;
static unsigned g_rng;
#define SEL_MAX 32
static wchar_t g_sel[SEL_MAX][MAX_PATH];
static int g_sel_count;
static int g_sel_pick = -1;
static int g_sel_last = -1;
static int g_sel_bag[SEL_MAX];
static int g_sel_bag_n;
static u64 g_sel_at;
static wchar_t g_sel_path[MAX_PATH];
static u32 g_loop_orig[BGM_SLOTS][3];
static int g_slots;
static int g_trace;

static void log_line(const char* text) {
    HANDLE f;
    DWORD w = 0;
    if (!g_log_path[0] || !text) return;
    f = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE) return;
    WriteFile(f, text, (DWORD)s_len(text), &w, 0);
    WriteFile(f, "\r\n", 2, &w, 0);
    CloseHandle(f);
}

static void log_num(const char* prefix, int n) {
    char buf[80];
    char digits[16];
    int i = 0;
    int v = n;
    s_copy(buf, 80, prefix);
    if (v == 0) digits[i++] = '0';
    while (v > 0 && i < 15) {
        digits[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        char one[2];
        one[0] = digits[--i];
        one[1] = 0;
        s_cat(buf, 80, one);
    }
    log_line(buf);
}

static void trace_open(const wch* name) {
    char line[MAX_PATH * 3 + 32];
    char digits[24];
    u64 t = GetTickCount64();
    int i = 0, n;
    if (!name) return;
    do { digits[i++] = (char)('0' + (int)(t % 10)); t /= 10; } while (t && i < 20);
    n = 0;
    while (i > 0) line[n++] = digits[--i];
    line[n++] = ' ';
    if (WideCharToMultiByte(CP_UTF8, 0, name, -1, line + n, (int)sizeof(line) - n, 0, 0) <= 0) line[n] = 0;
    log_line(line);
}

static int patch_iat(HMODULE module, const char* fn, void* detour, void** original) {
    u8* base = (u8*)module;
    int lfanew;
    u8* nt;
    u32 import_rva;
    u8* desc;
    int patched = 0;
    if (!base || *(u16*)base != 0x5A4D) return 0;
    lfanew = *(int*)(base + 0x3C);
    nt = base + lfanew;
    if (*(u32*)nt != 0x4550) return 0;
    import_rva = *(u32*)(nt + 24 + 120);
    if (!import_rva) return 0;
    for (desc = base + import_rva; *(u32*)(desc + 12); desc += 20) {
        u32 orig_rva = *(u32*)desc;
        u32 thunk_rva = *(u32*)(desc + 16);
        u64* orig;
        void** thunk;
        if (!orig_rva) orig_rva = thunk_rva;
        orig = (u64*)(base + orig_rva);
        thunk = (void**)(base + thunk_rva);
        for (; *orig; ++orig, ++thunk) {
            const char* name;
            DWORD old = 0;
            if (*orig & 0x8000000000000000ull) continue;
            name = (const char*)(base + (u32)*orig + 2);
            if (lstrcmpA(name, fn) != 0) continue;
            if (!VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &old)) continue;
            if (original && !*original) *original = *thunk;
            *thunk = detour;
            VirtualProtect(thunk, sizeof(void*), old, &old);
            patched = 1;
        }
    }
    return patched;
}

static int patch_ordinal(HMODULE module, const char* dll, int ordinal, void* detour, void** original) {
    u8* base = (u8*)module;
    int lfanew;
    u8* nt;
    u32 import_rva;
    u8* desc;
    int patched = 0;
    if (!base || *(u16*)base != 0x5A4D) return 0;
    lfanew = *(int*)(base + 0x3C);
    nt = base + lfanew;
    if (*(u32*)nt != 0x4550) return 0;
    import_rva = *(u32*)(nt + 24 + 120);
    if (!import_rva) return 0;
    for (desc = base + import_rva; *(u32*)(desc + 12); desc += 20) {
        u32 orig_rva = *(u32*)desc;
        u32 thunk_rva = *(u32*)(desc + 16);
        const char* dll_name = (const char*)(base + *(u32*)(desc + 12));
        u64* orig;
        void** thunk;
        if (!s_ieq(dll_name, dll)) continue;
        if (!orig_rva) orig_rva = thunk_rva;
        orig = (u64*)(base + orig_rva);
        thunk = (void**)(base + thunk_rva);
        for (; *orig; ++orig, ++thunk) {
            DWORD old = 0;
            if (!(*orig & 0x8000000000000000ull)) continue;
            if ((int)(*orig & 0xFFFFu) != ordinal) continue;
            if (!VirtualProtect(thunk, sizeof(void*), PAGE_READWRITE, &old)) continue;
            if (original && !*original) *original = *thunk;
            *thunk = detour;
            VirtualProtect(thunk, sizeof(void*), old, &old);
            patched = 1;
        }
    }
    return patched;
}

static void apply_choice(int random, const char* stem);
static void hide_window(void);

static unsigned rnd(void) {
    static unsigned step;
    u64 t = GetTickCount64();
    step++;
    g_rng ^= (unsigned)t ^ (unsigned)(t >> 17) ^ (step * 0x9E3779B9u);
    g_rng = g_rng * 1664525u + 1013904223u;
    if (!g_rng) g_rng = 0xA5A5A5A5u;
    return g_rng;
}

static void choice_copy(char* dst, int* random) {
    EnterCriticalSection(&g_lock);
    s_copy(dst, BGM_STEM, g_choice);
    *random = g_random;
    LeaveCriticalSection(&g_lock);
}

static const char* pick_random(const char* avoid) {
    int i = lib_random(g_lib, 0, rnd(), avoid);
    return i >= 0 ? g_cat.tracks[i].stem : 0;
}

static const char* slot_name(int i) {
    u8* base = (u8*)GetModuleHandleW(0);
    const char* path = (const char*)(base + BGM_ENTRY_RVA + BGM_PATH_OFF + (u32)i * BGM_STRIDE);
    const char* name = path;
    const char* p;
    if (path[0] < 'A' || path[0] > 'Z') return 0;
    for (p = path; *p && (int)(p - path) < 60; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

static u32* slot_loop(int i) {
    if (i < 0 || i >= g_slots) return 0;
    return (u32*)((u8*)GetModuleHandleW(0) + BGM_ENTRY_RVA + BGM_LOOP_OFF + (u32)i * BGM_STRIDE);
}

static int game_bgm_index(const char* stem) {
    int i;
    if (!stem || !stem[0]) return -1;
    for (i = 0; i < g_slots; ++i)
        if (s_ieq(slot_name(i), stem)) return i;
    return -1;
}

static void slots_snapshot(void) {
    int i, k;
    g_slots = BGM_SLOTS;
    for (i = 0; i < BGM_SLOTS; ++i) {
        if (!slot_name(i)) { g_slots = i; break; }
    }
    for (i = 0; i < g_slots; ++i) {
        u32* s = slot_loop(i);
        for (k = 0; k < 3; ++k) g_loop_orig[i][k] = s[k];
    }
    log_num("pistas en la tabla del juego ", g_slots);
}

static void loop_for(const char* stem, u32* out) {
    int idx = game_bgm_index(stem);
    out[0] = 1; out[1] = 0; out[2] = 1;
    if (idx < 0) return;
    out[0] = g_loop_orig[idx][0];
    out[1] = g_loop_orig[idx][1];
    out[2] = g_loop_orig[idx][2];
    if (!out[0]) { out[0] = 1; out[1] = 0; }
}

static void loop_arm(int visible) {
    char choice[BGM_STEM];
    int random = 0;
    int i, k;
    u32 target[3];
    choice_copy(choice, &random);
    loop_for(choice, target);
    for (i = 0; i < g_slots; ++i) {
        const char* name = slot_name(i);
        u32* s = slot_loop(i);
        u32 v[3];
        for (k = 0; k < 3; ++k) v[k] = g_loop_orig[i][k];
        if (!visible && bgm_find(&g_cat, name)) {
            if (random) {
                if (!v[2]) { v[0] = 1; v[1] = 0; v[2] = 1; }
            } else if (choice[0] && !s_ieq(name, choice)) {
                for (k = 0; k < 3; ++k) v[k] = target[k];
            }
        }
        for (k = 0; k < 3; ++k) s[k] = v[k];
    }
}

static void loop_point(const char* opened, const char* stem) {
    u32* s = slot_loop(game_bgm_index(opened));
    u32 v[3];
    if (!s) return;
    loop_for(stem, v);
    s[0] = v[0];
    s[1] = v[1];
}

static void loop_restore(const char* opened) {
    int idx = game_bgm_index(opened);
    u32* s = slot_loop(idx);
    if (!s) return;
    s[0] = g_loop_orig[idx][0];
    s[1] = g_loop_orig[idx][1];
}

static int is_order_screen(const wch* name) {
    return name && w_has(name, L"osel") && w_has(name, L".bin");
}

static int is_select_screen(const wch* name) {
    return name && w_has(name, L"psel") && w_has(name, L".bin");
}

static int is_practice_pause(const wch* name) {
    return name && w_has(name, L"pause") && w_has(name, L"practice");
}

static void request_music_screen(const char* why) {
    log_line(why);
    g_request_show = 1;
}

static int loop_file_stem(const wch* name, char* stem) {
    const wchar_t* slash;
    const wchar_t* base;
    const wchar_t* dot;
    int n;
    if (!name) return 0;
    slash = w_rchr(name, L'\\');
    if (!slash) slash = w_rchr(name, L'/');
    base = slash ? slash + 1 : name;
    dot = w_rchr(base, L'.');
    if (!dot || !w_ieq(dot, L".loop")) return 0;
    n = WideCharToMultiByte(CP_UTF8, 0, base, (int)(dot - base), stem, BGM_STEM - 1, 0, 0);
    if (n <= 0) return 0;
    stem[n] = 0;
    s_upper(stem);
    return 1;
}

/* Album songs keep their loop point in "<song>.loop" next to the .ogg (same
   text format as the game: seconds). Without one they loop from the start. */
static int loop_redirect_path(const wch* name, const char* stem, wchar_t* out, int cap) {
    const BgmTrack* t = bgm_find(&g_cat, stem);
    if (t && t->file[0]) {
        const wchar_t* dot;
        w_copy(out, cap, t->file);
        dot = w_rchr(out, L'.');
        if (dot && (int)(dot - out) + 6 < cap) {
            out[dot - out] = 0;
            w_cat(out, cap, L".loop");
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
        if (!g_loop0[0]) return 0;
        w_copy(out, cap, g_loop0);
        return 1;
    }
    return bgm_build_path(name, stem, L".loop", out, cap);
}

static void loop0_prepare(void) {
    HANDLE f;
    DWORD w = 0;
    w_copy(g_loop0, MAX_PATH, g_dll_dir);
    w_cat(g_loop0, MAX_PATH, L"kof2002bgm-desde-inicio.loop");
    if (GetFileAttributesW(g_loop0) != INVALID_FILE_ATTRIBUTES) return;
    f = CreateFileW(g_loop0, GENERIC_READ | 0x40000000u, FILE_SHARE_READ, 0, 2, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE) {
        g_loop0[0] = 0;
        return;
    }
    WriteFile(f, "0.000000", 8, &w, 0);
    CloseHandle(f);
}

static XInputGetState_fn g_xget_game;

static void zero_pad(XState* st) {
    if (!st) return;
    st->pad.buttons = 0;
    st->pad.lt = 0;
    st->pad.rt = 0;
    st->pad.lx = 0;
    st->pad.ly = 0;
    st->pad.rx = 0;
    st->pad.ry = 0;
}

static DWORD WINAPI hk_xinput(DWORD idx, XState* st) {
    DWORD r;
    if (!g_xget_game) return 1167;
    r = g_xget_game(idx, st);
    if (g_block_input && r == 0) zero_pad(st);
    return r;
}

static int select_ogg_ok(const wchar_t* path) {
    unsigned char head[160];
    HANDLE f;
    DWORD got = 0;
    int rate = 0, ch = 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE) return 0;
    if (!ReadFile(f, head, 160, &got, 0)) got = 0;
    CloseHandle(f);
    if (!lib_ogg_check(head, (int)got, &rate, &ch)) return 0;
    return ch == 2 && rate == GAME_RATE;
}

static void select_scan(void) {
    wchar_t pattern[MAX_PATH];
    FIND_DATA_ fd;
    HANDLE h;
    g_sel_count = 0;
    if (w_len(g_dll_dir) + 16 >= MAX_PATH) return;
    w_copy(pattern, MAX_PATH, g_dll_dir);
    w_cat(pattern, MAX_PATH, L"Select\\*.ogg");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE) {
        log_line("sin carpeta Select: la pantalla de personajes usa su musica");
        return;
    }
    do {
        if (fd.attributes & FILE_ATTRIBUTE_DIRECTORY || fd.name[0] == '.') continue;
        if (g_sel_count >= SEL_MAX) break;
        if (w_len(g_dll_dir) + 8 + w_len(fd.name) >= MAX_PATH) continue;
        w_copy(g_sel[g_sel_count], MAX_PATH, g_dll_dir);
        w_cat(g_sel[g_sel_count], MAX_PATH, L"Select\\");
        w_cat(g_sel[g_sel_count], MAX_PATH, fd.name);
        if (!select_ogg_ok(g_sel[g_sel_count])) {
            log_line("tema de seleccion ignorado: no es ogg estereo a 48000");
            continue;
        }
        g_sel_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    log_num("temas de seleccion ", g_sel_count);
}

static void select_reshuffle(void) {
    int i;
    g_sel_bag_n = g_sel_count;
    for (i = 0; i < g_sel_count; ++i) g_sel_bag[i] = i;
    for (i = g_sel_count - 1; i > 0; --i) {
        int j = (int)(rnd() % (unsigned)(i + 1));
        int tmp = g_sel_bag[i];
        g_sel_bag[i] = g_sel_bag[j];
        g_sel_bag[j] = tmp;
    }
    if (g_sel_count > 1 && g_sel_bag_n > 0 && g_sel_bag[g_sel_bag_n - 1] == g_sel_last) {
        int tmp = g_sel_bag[g_sel_bag_n - 1];
        g_sel_bag[g_sel_bag_n - 1] = g_sel_bag[0];
        g_sel_bag[0] = tmp;
    }
}

/* Random real por entrada a seleccion: bolsa sin repetir hasta agotar.
   Solo reutiliza el mismo pick ~400ms (CreateFile A/W doble). */
static int select_pick(void) {
    int i;
    u64 now;
    if (g_sel_count <= 0) return -1;
    now = GetTickCount64();
    if (g_sel_pick >= 0 && g_sel_pick < g_sel_count && now - g_sel_at < 400)
        return g_sel_pick;
    if (g_sel_bag_n <= 0) select_reshuffle();
    i = g_sel_bag[--g_sel_bag_n];
    g_sel_last = i;
    g_sel_pick = i;
    g_sel_at = now;
    return i;
}

static void log_select(const char* opened, const wchar_t* file) {
    char msg[320];
    char name[180];
    const wchar_t* slash = w_rchr(file, L'\\');
    const wchar_t* base = slash ? slash + 1 : file;
    if (WideCharToMultiByte(CP_UTF8, 0, base, -1, name, 179, 0, 0) <= 0) name[0] = 0;
    s_copy(msg, 320, "seleccion ");
    s_cat(msg, 320, opened);
    s_cat(msg, 320, " -> ");
    s_cat(msg, 320, name);
    log_line(msg);
}

static HANDLE open_redirect(const wch* name, const char* opened, const char* stem, DWORD access, DWORD share, void* sec, DWORD disp, DWORD flags, HANDLE templ) {
    wchar_t redirected[MAX_PATH];
    if (!stem || !stem[0]) return INVALID_HANDLE;
    if (!lib_redirect_path(g_lib, name, stem, redirected, MAX_PATH)) return INVALID_HANDLE;
    if (GetFileAttributesW(redirected) == INVALID_FILE_ATTRIBUTES) return INVALID_HANDLE;
    loop_point(opened, stem);
    s_copy(g_redir_from, BGM_STEM, opened);
    s_copy(g_redir_to, BGM_STEM, stem);
    return g_create(redirected, access, share, sec, disp, flags, templ);
}

static int is_loop_of(const char* stem, const char* base) {
    int n = s_len(stem), m = s_len(base);
    char head[BGM_STEM];
    if (!m || n != m + 4 || !s_ieq(stem + m, "LOOP")) return 0;
    s_copy(head, m + 1, stem);
    return s_ieq(head, base);
}

static int is_preview_open(const char* opened) {
    int hit;
    EnterCriticalSection(&g_lock);
    hit = g_prev_stem[0] && s_ieq(opened, g_prev_stem) && GetTickCount64() - g_prev_open_at < 3000;
    LeaveCriticalSection(&g_lock);
    return hit;
}

static void take_highlighted(void);

static HANDLE WINAPI hk_createfile(const wch* name, DWORD access, DWORD share, void* sec, DWORD disp, DWORD flags, HANDLE templ) {
    BgmOpenKind kind;
    char opened[BGM_STEM];
    char choice[BGM_STEM];
    int random = 0;
    int pick = -1;
    const char* stem = 0;
    HANDLE opened_file;
    if (!g_create) return INVALID_HANDLE;
    if (g_trace) trace_open(name);
    if (is_select_screen(name)) {
        g_await_order = 1;
    } else if (is_practice_pause(name)) {
        if (g_await_order && !g_visible) {
            g_await_order = 0;
            request_music_screen("practica: elige la musica");
        }
    }
    if (loop_file_stem(name, opened)) {
        if (g_sel_path[0] && s_ieq(opened, "BGM03")) {
            wchar_t redirected[MAX_PATH];
            const wchar_t* dot;
            w_copy(redirected, MAX_PATH, g_sel_path);
            dot = w_rchr(redirected, L'.');
            if (dot && (int)(dot - redirected) + 6 < MAX_PATH) {
                redirected[dot - redirected] = 0;
                w_cat(redirected, MAX_PATH, L".loop");
                if (GetFileAttributesW(redirected) != INVALID_FILE_ATTRIBUTES)
                    return g_create(redirected, access, share, sec, disp, flags, templ);
            }
        }
        if (g_redir_to[0] && s_ieq(opened, g_redir_from)) {
            wchar_t redirected[MAX_PATH];
            if (loop_redirect_path(name, g_redir_to, redirected, MAX_PATH))
                return g_create(redirected, access, share, sec, disp, flags, templ);
        }
        return g_create(name, access, share, sec, disp, flags, templ);
    }
    if (is_order_screen(name)) {
        opened_file = g_create(name, access, share, sec, disp, flags, templ);
        if (g_await_order && !g_visible) {
            g_await_order = 0;
            request_music_screen("personajes listos: elige la musica");
        } else if (!g_visible) {
            log_line("orden vista antes de la seleccion");
        }
        return opened_file;
    }
    if (!bgm_is_bgm_path(name) || !bgm_stem_from_path(name, opened, BGM_STEM))
        return g_create(name, access, share, sec, disp, flags, templ);
    if (s_ieq(opened, "BGM03")) {
        g_ps_tick = GetTickCount64();
        g_await_order = 1;
        EnterCriticalSection(&g_lock);
        g_phase = 0;
        LeaveCriticalSection(&g_lock);
        pick = select_pick();
        if (pick >= 0) {
            w_copy(g_sel_path, MAX_PATH, g_sel[pick]);
            opened_file = g_create(g_sel[pick], access, share, sec, disp, flags, templ);
            if (opened_file != INVALID_HANDLE) {
                log_select(opened, g_sel[pick]);
                return opened_file;
            }
        }
        g_sel_path[0] = 0;
    } else {
        g_sel_path[0] = 0;
    }
    if (g_redir_to[0] && is_loop_of(opened, g_redir_from)) {
        s_copy(choice, BGM_STEM, g_redir_to);
        opened_file = open_redirect(name, opened, choice, access, share, sec, disp, flags, templ);
        if (opened_file != INVALID_HANDLE) return opened_file;
    }
    g_redir_from[0] = 0;
    g_redir_to[0] = 0;
    if (g_visible) {
        if (is_preview_open(opened)) {
            EnterCriticalSection(&g_lock);
            s_copy(choice, BGM_STEM, g_preview_extra);
            LeaveCriticalSection(&g_lock);
            if (choice[0] && s_ieq(opened, PREVIEW_CARRIER)) {
                opened_file = open_redirect(name, opened, choice, access, share, sec, disp, flags, templ);
                if (opened_file != INVALID_HANDLE) return opened_file;
            }
            loop_restore(opened);
            return g_create(name, access, share, sec, disp, flags, templ);
        }
        if (bgm_classify(&g_cat, name) != BGM_OPEN_FIGHT)
            return g_create(name, access, share, sec, disp, flags, templ);
        take_highlighted();
    }
    kind = bgm_classify(&g_cat, name);
    if (kind == BGM_OPEN_SELECT) {
        g_ps_tick = GetTickCount64();
        g_await_order = 1;
        log_line("musica de seleccion: esperando la orden");
        EnterCriticalSection(&g_lock);
        g_phase = 0;
        LeaveCriticalSection(&g_lock);
    } else if (kind == BGM_OPEN_IGNORE && s_ieq(opened, "WIN_N")) {
        g_after_fight = 1;
    } else if (kind == BGM_OPEN_FIGHT) {
        g_after_fight = 1;
        choice_copy(choice, &random);
        if (random) stem = pick_random(opened);
        else if (choice[0] && !s_ieq(choice, opened)) stem = choice;
        if (stem) {
            opened_file = open_redirect(name, opened, stem, access, share, sec, disp, flags, templ);
            if (opened_file != INVALID_HANDLE) {
                char msg[160];
                s_copy(msg, 160, "BGM ");
                s_cat(msg, 160, opened);
                s_cat(msg, 160, " -> ");
                s_cat(msg, 160, stem);
                log_line(msg);
                return opened_file;
            }
        }
        loop_restore(opened);
    }
    return g_create(name, access, share, sec, disp, flags, templ);
}

static void save_ini(void) {
    wchar_t path[MAX_PATH];
    HANDLE f;
    DWORD w = 0;
    char body[96];
    w_copy(path, MAX_PATH, g_dll_dir);
    w_cat(path, MAX_PATH, L"kof2002bgm.ini");
    if (g_random) s_copy(body, 96, "mode=random\r\n");
    else if (g_choice[0]) {
        s_copy(body, 96, "mode=track\r\ntrack=");
        s_cat(body, 96, g_choice);
        s_cat(body, 96, "\r\n");
    } else {
        s_copy(body, 96, "mode=stage\r\n");
    }
    f = CreateFileW(path, GENERIC_READ | 0x40000000u, FILE_SHARE_READ, 0, 2, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE) return;
    WriteFile(f, body, (DWORD)s_len(body), &w, 0);
    CloseHandle(f);
}

static void load_ini(void) {
    wchar_t path[MAX_PATH];
    HANDLE f;
    char buf[512];
    DWORD got = 0;
    int i = 0;
    g_choice[0] = 0;
    g_random = 0;
    w_copy(path, MAX_PATH, g_dll_dir);
    w_cat(path, MAX_PATH, L"kof2002bgm.ini");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE) return;
    ReadFile(f, buf, (DWORD)sizeof(buf) - 1, &got, 0);
    CloseHandle(f);
    buf[got] = 0;
    while (buf[i]) {
        int start = i;
        while (buf[i] && buf[i] != '\n' && buf[i] != '\r') ++i;
        if (buf[i]) { buf[i] = 0; ++i; if (buf[i] == '\n') ++i; }
        if (s_len(buf + start) >= 5 && buf[start] == 'm') {
            if (s_ieq(buf + start + 5, "random")) g_random = 1;
        } else if (s_len(buf + start) > 6 && buf[start] == 't') {
            s_copy(g_choice, BGM_STEM, buf + start + 6);
        }
    }
}

static void apply_choice(int random, const char* stem) {
    EnterCriticalSection(&g_lock);
    g_random = random;
    if (random || !stem) g_choice[0] = 0;
    else s_copy(g_choice, BGM_STEM, stem);
    LeaveCriticalSection(&g_lock);
    save_ini();
}

static HWND find_game_window(void);
static void vs_bind(void);
static void publish_view(void);
static int g_overlay_logged;

static HWND g_game_hwnd;
static WndProcFn g_game_proc;
static int g_in_play;

static LRESULT CALLBACK game_proc(HWND hwnd, u32 msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PLAY && !g_visible) return 0;
    if (msg == WM_PLAY && !g_in_play) {
        typedef void (__fastcall *PlayFn)(int index);
        u8* base = (u8*)GetModuleHandleW(0);
        PlayFn play = (PlayFn)(base + PLAY_RVA);
        g_in_play = 1;
        *(int*)(base + CURRENT_RVA) = -1;
        play((int)wp);
        g_in_play = 0;
        return 0;
    }
    return CallWindowProcW(g_game_proc, hwnd, msg, wp, lp);
}

static void game_play_stem(const char* stem) {
    int idx = game_bgm_index(stem);
    HWND game = find_game_window();
    if (idx < 0) {
        if (!g_preview_logged) {
            g_preview_logged = 1;
            log_line("esa pista no esta en el reproductor del juego");
        }
        return;
    }
    if (!game) return;
    if (game != g_game_hwnd || !g_game_proc) {
        LONG_PTR prev = SetWindowLongPtrW(game, GWLP_WNDPROC, (LONG_PTR)game_proc);
        if (!prev) return;
        g_game_proc = (WndProcFn)prev;
        g_game_hwnd = game;
        log_line("previa en el reproductor del juego");
    }
    PostMessageW(game, WM_PLAY, (WPARAM)idx, 0);
}

static void set_preview_extra(const char* stem) {
    EnterCriticalSection(&g_lock);
    s_copy(g_preview_extra, BGM_STEM, stem ? stem : "");
    LeaveCriticalSection(&g_lock);
}

static void preview_arm(void) {
    g_prev_pending = 1;
    g_prev_at = GetTickCount64();
}

static void preview_mark(const char* stem) {
    EnterCriticalSection(&g_lock);
    s_copy(g_prev_stem, BGM_STEM, stem);
    g_prev_open_at = GetTickCount64();
    LeaveCriticalSection(&g_lock);
}

static const BgmTrack* item_track(int i) {
    if (i < 0 || i >= g_item_count || g_items[i].kind != LIB_TRACK) return 0;
    if (g_items[i].index < 0 || g_items[i].index >= g_cat.count) return 0;
    return &g_cat.tracks[g_items[i].index];
}

static void preview_launch(void) {
    const char* stem;
    const BgmTrack* t = item_track(g_index);
    if (!t) return;
    stem = t->stem;
    if (game_bgm_index(stem) >= 0) {
        set_preview_extra(0);
        preview_mark(stem);
        game_play_stem(stem);
        return;
    }
    set_preview_extra(stem);
    preview_mark(PREVIEW_CARRIER);
    game_play_stem(PREVIEW_CARRIER);
}

static void preview_poll(void) {
    if (!g_visible) {
        g_prev_pending = 0;
        return;
    }
    if (!g_prev_pending) return;
    if (GetTickCount64() - g_prev_at < 160) return;
    g_prev_pending = 0;
    preview_launch();
}

static void hide_window(void) {
    g_visible = 0;
    if (g_block_input == 1) g_block_input = 2;
    g_ask = 0;
    g_prev_pending = 0;
    set_preview_extra(0);
    loop_arm(0);
    EnterCriticalSection(&g_lock);
    if (g_phase != 2) g_phase = 1;
    LeaveCriticalSection(&g_lock);
    publish_view();
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
}

static void log_choice(const char* what, const BgmTrack* t) {
    char msg[BGM_TITLE + 64];
    s_copy(msg, (int)sizeof(msg), what);
    s_cat(msg, (int)sizeof(msg), t->stem);
    s_cat(msg, (int)sizeof(msg), " ");
    s_cat(msg, (int)sizeof(msg), t->title);
    log_line(msg);
}

static void confirm_track(void) {
    const BgmTrack* t = item_track(g_index);
    if (!t) return;
    apply_choice(0, t->stem);
    log_choice("elegida: ", t);
    hide_window();
}

static void random_track(void) {
    char avoid[BGM_STEM];
    int random = 0, i;
    choice_copy(avoid, &random);
    i = lib_random(g_lib, g_folder, rnd(), avoid);
    if (i < 0) return;
    apply_choice(0, g_cat.tracks[i].stem);
    log_choice(g_folder ? "al azar del album: " : "al azar de toda la biblioteca: ", &g_cat.tracks[i]);
    hide_window();
}

/* Same layout as BgmMenuView in KofVsSelector (ui.h). Drawn on the game frame. */
#define BGM_MENU_MAGIC 0x314E554Du
#define BGM_MENU_ROWS 14
typedef struct BgmMenuView {
    int magic;
    int visible;
    char title[48];
    char crumb[72];
    char status[80];
    char hint[80];
    int page;
    int pages;
    int row_count;
    int selected;
    char row[BGM_MENU_ROWS][56];
    int mark[BGM_MENU_ROWS];
} BgmMenuView;

static BgmMenuView g_menu_view;

static void fold_ascii(char* dst, int cap, const char* src) {
    int i = 0, j = 0;
    if (cap <= 0) return;
    if (!src) src = "";
    while (src[i] && j + 1 < cap) {
        unsigned char c = (unsigned char)src[i++];
        if (c < 0x80) dst[j++] = (char)c;
        else if ((c & 0xE0) == 0xC0) { if (src[i]) ++i; }
        else if ((c & 0xF0) == 0xE0) { if (src[i]) ++i; if (src[i]) ++i; }
        else if ((c & 0xF8) == 0xF0) { int k; for (k = 0; k < 3 && src[i]; ++k) ++i; }
    }
    dst[j] = 0;
}

static void publish_view(void) {
    BgmMenuView v;
    int i, n, pages;
    char crumb[96];
    char buf[160];
    memset(&v, 0, sizeof(v));
    v.magic = BGM_MENU_MAGIC;
    v.visible = g_visible ? 1 : 0;
    if (v.visible && g_lib) {
        fold_ascii(v.title, (int)sizeof(v.title), "SELECT MUSIC");
        crumb[0] = 0;
        if (g_folder > 0) lib_crumb(g_lib, g_folder, crumb, (int)sizeof(crumb));
        fold_ascii(v.crumb, (int)sizeof(v.crumb), g_folder > 0 ? crumb : GAME_NAME);
        n = 0;
        for (i = 0; i < ROW_N && g_scroll + i < g_item_count && n < BGM_MENU_ROWS; ++i) {
            int idx = g_scroll + i;
            v.mark[n] = 0;
            if (g_items[idx].kind == LIB_BACK) {
                fold_ascii(v.row[n], 36, "VOLVER");
                v.mark[n] = 2;
            } else if (g_items[idx].kind == LIB_FOLDER && g_items[idx].index >= 0 && g_items[idx].index < g_lib->folder_count) {
                fold_ascii(v.row[n], 36, g_lib->folders[g_items[idx].index].name);
                v.mark[n] = 1;
            } else if (g_items[idx].kind == LIB_TRACK && g_items[idx].index >= 0 && g_items[idx].index < g_cat.count) {
                fold_ascii(v.row[n], 36, g_cat.tracks[g_items[idx].index].title);
            }
            ++n;
        }
        v.row_count = n;
        v.selected = g_index - g_scroll;
        if (v.selected < 0 || v.selected >= n) v.selected = -1;
        pages = g_item_count > 0 ? (g_item_count + ROW_N - 1) / ROW_N : 1;
        v.page = g_item_count > 0 ? g_index / ROW_N + 1 : 1;
        v.pages = pages < 1 ? 1 : pages;
        if (g_random) fold_ascii(v.status, (int)sizeof(v.status), "EN EL COMBATE SUENA UNA PISTA AL AZAR");
        else if (g_choice[0]) {
            const BgmTrack* t = bgm_find(&g_cat, g_choice);
            s_copy(buf, (int)sizeof(buf), "ELEGIDA: ");
            s_cat(buf, (int)sizeof(buf), t ? t->title : g_choice);
            fold_ascii(v.status, (int)sizeof(v.status), buf);
        } else {
            fold_ascii(v.status, (int)sizeof(v.status), "EN EL COMBATE SUENA LA MUSICA DEL ESCENARIO");
        }
        fold_ascii(v.hint, (int)sizeof(v.hint), "ARRIBA/ABAJO MUEVE  IZQ/DER PAGINA  START/D ELIGE  START+D AZAR");
    }
    g_menu_view = v;
}

__declspec(dllexport) int WINAPI KofBgm_MenuView(BgmMenuView* out) {
    if (!out) return 0;
    *out = g_menu_view;
    if (out->magic != BGM_MENU_MAGIC) return 0;
    return out->visible;
}

static void take_highlighted(void) {
    const BgmTrack* t = item_track(g_index);
    if (t) apply_choice(0, t->stem);
    g_visible = 0;
    g_request_hide = 1;
    publish_view();
    log_line(t ? "la pelea empezo: uso la pista resaltada" : "la pelea empezo: se mantiene la eleccion anterior");
}

static void set_index(int i, int force) {
    int prev = g_index;
    if (g_item_count <= 0) {
        g_index = g_scroll = 0;
        if (g_hwnd) InvalidateRect(g_hwnd, 0, 0);
        publish_view();
        return;
    }
    if (i < 0) i = 0;
    if (i >= g_item_count) i = g_item_count - 1;
    g_index = i;
    g_scroll = (g_index / ROW_N) * ROW_N;
    if (g_hwnd) InvalidateRect(g_hwnd, 0, 0);
    if ((force || g_index != prev) && item_track(g_index)) preview_arm();
    publish_view();
}

static void move_index(int delta) {
    if (g_item_count > 0) set_index(menu_step(g_index, g_item_count, delta), 0);
}

static void move_page(int dir) {
    if (g_item_count > 0) set_index(menu_page(g_index, g_item_count, ROW_N, dir), 0);
}

static void open_folder(int folder, int focus_kind, int focus_index) {
    int pos;
    if (folder < 0 || folder >= g_lib->folder_count) folder = 0;
    g_folder = folder;
    g_item_count = lib_items(g_lib, folder, g_items, LIB_ITEMS);
    pos = focus_kind ? lib_item_find(g_items, g_item_count, focus_kind, focus_index) : -1;
    if (pos < 0) pos = (folder != 0 && g_item_count > 1) ? 1 : 0;
    g_index = -1;
    set_index(pos, 1);
}

static void go_back(void) {
    int from = g_folder;
    if (from <= 0) return;
    open_folder(g_lib->folders[from].parent, LIB_FOLDER, from);
}

static void activate(void) {
    if (g_index < 0 || g_index >= g_item_count) return;
    if (g_items[g_index].kind == LIB_BACK) go_back();
    else if (g_items[g_index].kind == LIB_FOLDER) open_folder(g_items[g_index].index, 0, 0);
    else confirm_track();
}

static void run_actions(unsigned a) {
    if (!a || !g_visible) return;
    if (a & MA_RANDOM) random_track();
    else if (a & MA_PICK) activate();
    else if (a & MA_BACK) go_back();
    else if (a & MA_UP) move_index(-1);
    else if (a & MA_DOWN) move_index(1);
    else if (a & MA_LEFT) move_page(-1);
    else if (a & MA_RIGHT) move_page(1);
}

static void library_refresh(void) {
    LibSpec spec;
    Library* next = g_lib == &g_libs[0] ? &g_libs[1] : &g_libs[0];
    int prev_count = g_lib_built ? g_cat.count : -1;
    int prev_ignored = g_lib_built ? g_lib->ignored : -1;
    memset(&spec, 0, sizeof(spec));
    spec.bgm_dir = g_bgm_dir;
    spec.game_album = GAME_ALBUM;
    spec.sample_rate = GAME_RATE;
    spec.roots[0] = g_music_dir[0];
    spec.roots[1] = g_music_dir[1];
    spec.log_files = !g_lib_built;
    spec.log = log_line;
    lib_build(next, &spec);
    g_lib = next;
    g_lib_built = 1;
    if (next->cat.count != prev_count || next->ignored != prev_ignored) {
        log_num("biblioteca: pistas ", next->cat.count);
        log_num("biblioteca: carpetas ", next->folder_count - 1);
        log_num("biblioteca: pistas de la carpeta Music ", next->scanned);
        if (next->ignored) log_num("biblioteca: archivos ignorados ", next->ignored);
    }
}

static HWND find_game_window(void) {
    HWND hwnd = 0;
    DWORD pid = GetCurrentProcessId();
    while ((hwnd = FindWindowExW(0, hwnd, 0, 0)) != 0) {
        DWORD wpid = 0;
        if (hwnd == g_hwnd || !IsWindowVisible(hwnd)) continue;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == pid && GetWindow(hwnd, GW_OWNER) == 0) return hwnd;
    }
    return 0;
}

static void show_window(void) {
    RECT_ rc;
    HWND game;
    char choice[BGM_STEM];
    int random = 0, t;
    g_last_show = GetTickCount64();
    g_request_show = 0;
    g_block_input = 1;
    vs_bind();
    if (g_vs_menu) g_vs_menu(1);
    library_refresh();
    loop_arm(1);
    memset(&g_menu, 0, sizeof(g_menu));
    g_menu.source = -1;
    g_key_prev = 0xFFFFFFFFu;
    choice_copy(choice, &random);
    g_visible = 1;
    t = random ? -1 : lib_track_index(g_lib, choice);
    if (t >= 0) open_folder(g_cat.tracks[t].album, LIB_TRACK, t);
    else open_folder(g_folder < g_lib->folder_count ? g_folder : 0, 0, 0);
    publish_view();
    if (!g_overlay_logged) {
        g_overlay_logged = 1;
        log_line("ventana SELECT MUSIC");
    }
    game = find_game_window();
    if (game) SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)game);
    if (game && GetWindowRect(game, &rc)) {
        int w = 760, h = 640;
        int x = rc.left + ((rc.right - rc.left) - w) / 2;
        int y = rc.top + ((rc.bottom - rc.top) - h) / 2;
        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    InvalidateRect(g_hwnd, 0, 1);
}

static void draw_utf8(HDC dc, const char* text, RECT_* area, u32 flags) {
    wchar_t wide[512];
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 512) <= 0) return;
    DrawTextW(dc, wide, -1, area, flags | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void paint_status(HDC dc, RECT_* area) {
    if (g_random) {
        SetTextColor(dc, 0x0078D6FFu);
        DrawTextA(dc, "En el combate suena una pista al azar", -1, area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (g_choice[0]) {
        const BgmTrack* t = bgm_find(&g_cat, g_choice);
        char buf[BGM_TITLE + LIB_NAME + 24];
        s_copy(buf, (int)sizeof(buf), "Elegida: ");
        s_cat(buf, (int)sizeof(buf), t ? t->title : g_choice);
        if (t && t->album > 0 && t->album < g_lib->folder_count) {
            s_cat(buf, (int)sizeof(buf), "  (");
            s_cat(buf, (int)sizeof(buf), g_lib->folders[t->album].name);
            s_cat(buf, (int)sizeof(buf), ")");
        }
        SetTextColor(dc, 0x0078D6FFu);
        draw_utf8(dc, buf, area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    SetTextColor(dc, 0x00B0C4D2u);
    DrawTextA(dc, "En el combate suena la musica del escenario", -1, area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint(HWND hwnd) {
    PAINT_ ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT_ rc, row;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ old, old_font;
    HFONT font, title_font;
    HBRUSH bg, gold, hi;
    int i, y;
    GetClientRect(hwnd, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    old = SelectObject(mem, bmp);
    font = CreateFontW(20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    title_font = CreateFontW(34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    old_font = SelectObject(mem, title_font);
    SetBkMode(mem, TRANSPARENT);
    bg = CreateSolidBrush(0x000C0816u);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    row.left = 24; row.top = 18; row.right = rc.right - 24; row.bottom = 28;
    gold = CreateSolidBrush(0x0048A8D4u);
    FillRect(mem, &row, gold);
    DeleteObject(gold);
    SetTextColor(mem, 0x0096E2FFu);
    row.top = 40; row.bottom = 84;
    DrawTextA(mem, "SELECT MUSIC", -1, &row, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, font);
    SetTextColor(mem, 0x008296B0u);
    row.top = 84; row.bottom = 112;
    {
        char head[LIB_NAME * 4];
        s_copy(head, (int)sizeof(head), GAME_NAME);
        if (g_folder > 0) {
            s_cat(head, (int)sizeof(head), "  \xC2\xBB  ");
            lib_crumb(g_lib, g_folder, head + s_len(head), (int)sizeof(head) - s_len(head));
        }
        draw_utf8(mem, head, &row, DT_CENTER | DT_SINGLELINE);
    }
    y = LIST_TOP;
    for (i = 0; i < ROW_N; ++i) {
        int idx = g_scroll + i;
        RECT_ line;
        char text[LIB_NAME + BGM_TITLE + 16];
        line.left = 48; line.top = y; line.right = rc.right - 48; line.bottom = y + ROW_H;
        if (idx >= g_item_count) break;
        if (idx == g_index) {
            hi = CreateSolidBrush(0x00241878u);
            FillRect(mem, &line, hi);
            DeleteObject(hi);
        }
        if (g_items[idx].kind == LIB_BACK) {
            SetTextColor(mem, idx == g_index ? 0x00BEECFFu : 0x0090A8C0u);
            s_copy(text, (int)sizeof(text), "\xC2\xAB Volver");
        } else if (g_items[idx].kind == LIB_FOLDER) {
            const LibFolder* f = &g_lib->folders[g_items[idx].index];
            char count[16];
            int n = f->total, k = 0;
            char digits[12];
            SetTextColor(mem, idx == g_index ? 0x00BEECFFu : 0x0068C8F0u);
            s_copy(text, (int)sizeof(text), "\xC2\xBB ");
            s_cat(text, (int)sizeof(text), f->name);
            do { digits[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 11);
            count[0] = ' '; count[1] = '(';
            n = 2;
            while (k > 0) count[n++] = digits[--k];
            count[n++] = ')';
            count[n] = 0;
            s_cat(text, (int)sizeof(text), count);
        } else {
            SetTextColor(mem, idx == g_index ? 0x00BEECFFu : 0x00C8DCECu);
            s_copy(text, (int)sizeof(text), g_cat.tracks[g_items[idx].index].title);
        }
        line.left += 8; line.right -= 8;
        draw_utf8(mem, text, &line, DT_VCENTER | DT_SINGLELINE);
        y += ROW_H;
    }
    if (g_item_count > ROW_N) {
        char page[48];
        int p = g_index / ROW_N + 1, pages = (g_item_count + ROW_N - 1) / ROW_N, k = 0;
        char digits[12];
        RECT_ line;
        s_copy(page, (int)sizeof(page), "P\xC3\xA1gina ");
        do { digits[k++] = (char)('0' + p % 10); p /= 10; } while (p && k < 11);
        while (k > 0) { char one[2]; one[0] = digits[--k]; one[1] = 0; s_cat(page, (int)sizeof(page), one); }
        s_cat(page, (int)sizeof(page), " / ");
        k = 0;
        do { digits[k++] = (char)('0' + pages % 10); pages /= 10; } while (pages && k < 11);
        while (k > 0) { char one[2]; one[0] = digits[--k]; one[1] = 0; s_cat(page, (int)sizeof(page), one); }
        line.left = 48; line.right = rc.right - 48;
        line.top = LIST_TOP + ROW_N * ROW_H + 2; line.bottom = line.top + 24;
        SetTextColor(mem, 0x008296B0u);
        draw_utf8(mem, page, &line, DT_CENTER | DT_SINGLELINE);
    }
    if (g_cat.count == 0) {
        RECT_ line;
        line.left = 48; line.top = 160; line.right = rc.right - 48; line.bottom = 200;
        SetTextColor(mem, 0x00A0B4FFu);
        DrawTextA(mem, "No encontre las pistas del juego.", -1, &line, DT_SINGLELINE);
    }
    row.top = rc.bottom - 92; row.bottom = rc.bottom - 64;
    paint_status(mem, &row);
    SetTextColor(mem, 0x00768496u);
    row.top = rc.bottom - 62; row.bottom = rc.bottom - 38;
    draw_utf8(mem, "Arriba/abajo: mover    Izq/der: p\xC3\xA1gina    SELECT: volver", &row, DT_CENTER | DT_SINGLELINE);
    row.top = rc.bottom - 38; row.bottom = rc.bottom - 14;
    draw_utf8(mem, "START o D: elegir / abrir    START + D: canci\xC3\xB3n al azar", &row, DT_CENTER | DT_SINGLELINE);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_font);
    SelectObject(mem, old);
    DeleteObject(font);
    DeleteObject(title_font);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void vs_bind(void) {
    HMODULE m;
    u64 now = GetTickCount64();
    if (g_vs_pads) return;
    if (g_vs_probe && now - g_vs_probe < 1000) return;
    g_vs_probe = now;
    m = GetModuleHandleW(L"KofVsSelector.dll");
    if (!m) return;
    g_vs_pads = (VsPads_fn)GetProcAddress(m, "KofVs_MusicPads");
    g_vs_menu = (VsMenu_fn)GetProcAddress(m, "KofVs_MusicMenu");
    if (g_vs_pads) log_line("selector de controles detectado: la musica responde a los mandos que el juego deja jugar");
}

/* Source 1: the controller selector (only the P1/P2 controllers it assigned,
   already in their button layout, Steam duplicates removed).
   Source 0: every XInput slot, as before, when the selector is not loaded. */
static int read_pads(VsPad* out, int max, int* source) {
    int i, n = 0;
    vs_bind();
    if (g_vs_pads) {
        n = g_vs_pads(out, max);
        if (n >= 0) {
            *source = 1;
            return n;
        }
        n = 0;
    }
    *source = 0;
    for (i = 0; i < 4 && n < max; ++i) {
        XState st;
        if (!g_xinput || g_xinput((DWORD)i, &st) != 0) continue;
        out[n].id = i;
        out[n].side = -1;
        out[n].pad = st.pad;
        ++n;
    }
    return n;
}

static int game_focused(void) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static unsigned keyboard_poll(void) {
    static const int keys[6] = { 0x26, 0x28, 0x25, 0x27, VK_PRIOR, VK_NEXT };
    static const unsigned acts[6] = { MA_UP, MA_DOWN, MA_LEFT, MA_RIGHT, MA_LEFT, MA_RIGHT };
    u32 now = 0, pressed;
    unsigned out = 0;
    int i;
    if (game_focused())
        for (i = 0; i < 6; ++i)
            if (GetAsyncKeyState(keys[i]) & 0x8000) now |= 1u << i;
    pressed = now & ~g_key_prev;
    g_key_prev = now;
    for (i = 0; i < 6; ++i)
        if (pressed & (1u << i)) out |= acts[i];
    return out;
}

static void poll_pad(void) {
    VsPad pads[8];
    int n, i, source = 0;
    unsigned acts = 0;
    u64 now = GetTickCount64();
    if (!g_visible) return;
    n = read_pads(pads, 8, &source);
    if (g_vs_menu) g_vs_menu(1);
    menu_begin(&g_menu, source);
    for (i = 0; i < n; ++i)
        acts |= menu_feed(&g_menu, pads[i].id, menu_buttons(pads[i].pad.buttons, pads[i].pad.lx, pads[i].pad.ly), now);
    menu_end(&g_menu);
    acts |= keyboard_poll();
    run_actions(menu_resolve(acts));
}

static void block_release_poll(void) {
    VsPad pads[8];
    int i, n, source = 0;
    int busy = 0;
    if (g_block_input != 2) return;
    n = read_pads(pads, 8, &source);
    for (i = 0; i < n; ++i) {
        const XPad* p = &pads[i].pad;
        if (p->buttons || p->lt > 30 || p->rt > 30) busy = 1;
        if (p->ly > 8000 || p->ly < -8000) busy = 1;
        if (p->lx > 8000 || p->lx < -8000) busy = 1;
    }
    if (busy) {
        if (g_vs_menu) g_vs_menu(1);
        return;
    }
    g_block_input = 0;
    if (g_vs_menu) g_vs_menu(0);
}

static LRESULT CALLBACK wndproc(HWND hwnd, u32 msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) { paint(hwnd); return 0; }
    if (msg == WM_TIMER) {
        if (g_request_hide) {
            g_request_hide = 0;
            hide_window();
        }
        if (g_request_show && !g_visible) show_window();
        poll_pad();
        block_release_poll();
        preview_poll();
        return 0;
    }
    if (msg == WM_LBUTTONDOWN && g_visible) {
        int y = (short)(lp >> 16);
        int idx;
        if (y < LIST_TOP) return 0;
        idx = (y - LIST_TOP) / ROW_H;
        if (idx >= ROW_N) return 0;
        idx += g_scroll;
        if (idx < 0 || idx >= g_item_count) return 0;
        if (idx == g_index) activate();
        else set_index(idx, 0);
        return 0;
    }
    if (msg == WM_CLOSE) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI ui_thread(void* unused) {
    WNDCLASSEXW_ wc;
    wchar_t exe[MAX_PATH];
    wchar_t bgm_dir[MAX_PATH];
    wchar_t sys[MAX_PATH];
    HMODULE xmod;
    MSG_ msg;
    int n;
    (void)unused;
    InitializeCriticalSection(&g_lock);
    n = (int)GetModuleFileNameW(g_self, g_dll_dir, MAX_PATH);
    while (n > 0 && g_dll_dir[n - 1] != L'\\') --n;
    g_dll_dir[n] = 0;
    w_copy(g_log_path, MAX_PATH, g_dll_dir);
    w_cat(g_log_path, MAX_PATH, L"kof2002bgm.log");
    w_copy(sys, MAX_PATH, g_dll_dir);
    w_cat(sys, MAX_PATH, L"kof2002bgm.trace");
    g_trace = GetFileAttributesW(sys) != INVALID_FILE_ATTRIBUTES;
    GetModuleFileNameW(0, exe, MAX_PATH);
    n = w_len(exe);
    while (n > 0 && exe[n - 1] != L'\\') --n;
    exe[n] = 0;
    w_copy(bgm_dir, MAX_PATH, exe);
    w_cat(bgm_dir, MAX_PATH, L"Data\\BGM\\UM48000Hz");
    w_copy(g_bgm_dir, MAX_PATH, bgm_dir);
    w_copy(g_music_dir[0], MAX_PATH, g_dll_dir);
    w_cat(g_music_dir[0], MAX_PATH, L"Music");
    w_copy(g_music_dir[1], MAX_PATH, exe);
    w_cat(g_music_dir[1], MAX_PATH, L"Music");
    loop0_prepare();
    library_refresh();
    log_num("catalogo ", g_cat.count);
    select_scan();
    load_ini();
    slots_snapshot();
    loop_arm(0);
    g_rng = (unsigned)GetTickCount64();
    if (!patch_iat(GetModuleHandleW(0), "CreateFileW", (void*)hk_createfile, (void**)&g_create))
        log_line("no se pudo enganchar CreateFileW");
    else
        log_line("CreateFileW enganchado");
    patch_ordinal(GetModuleHandleW(0), "XINPUT1_4.dll", 2, (void*)hk_xinput, (void**)&g_xget_game);
    patch_ordinal(GetModuleHandleW(0), "XINPUT1_3.dll", 2, (void*)hk_xinput, (void**)&g_xget_game);
    patch_ordinal(GetModuleHandleW(0), "XINPUT9_1_0.dll", 2, (void*)hk_xinput, (void**)&g_xget_game);
    if (g_xget_game) log_line("botones del juego bloqueados mientras eliges");
    else log_line("no pude bloquear los botones del juego");
    GetSystemDirectoryW(sys, MAX_PATH);
    w_cat(sys, MAX_PATH, L"\\xinput1_4.dll");
    xmod = LoadLibraryW(sys);
    if (xmod) g_xinput = (XInputGetState_fn)GetProcAddress(xmod, "XInputGetState");
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = g_self;
    wc.hCursor = LoadCursorW(0, (const wch*)(u64)32512);
    wc.lpszClassName = L"Kof2002MusicSelect";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Kof2002MusicSelect", L"SELECT MUSIC",
        WS_POPUP | WS_BORDER, 80, 80, 760, 640, 0, 0, g_self, 0);
    SetTimer(g_hwnd, 1, 15, 0);
    RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_SHIFT, VK_F8);
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            show_window();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

int WINAPI DllMain(HMODULE self, DWORD reason, void* reserved) {
    (void)reserved;
    if (reason == 1) {
        DisableThreadLibraryCalls(self);
        g_self = self;
        CreateThread(0, 0, ui_thread, 0, 0, 0);
    }
    return 1;
}
