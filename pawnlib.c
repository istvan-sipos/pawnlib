/*
 * pawnlib.c - DDDA pawn archive shim, real-Steam edition.
 *
 * Spike scope so far:
 *   Step 0 - minimal DllMain that proves ddda-dinput8 loads us.
 *   Step 1 - same code, exercised in DDDA on Proton.  CONFIRMED.
 *   Step 2 - resolve real Steam interfaces, log everything we need
 *            to commit to a hooking strategy.  CONFIRMED: real
 *            Valve steam_api.dll v2.77.37.82, legacy global
 *            accessors present, pawndb's slot indices match.
 *   Step 3 - install one passive vtable hook on FindLeaderboard.
 *            Hook just logs the leaderboard name and forwards to
 *            the original.  Validates the patch_vtable machinery
 *            (ported verbatim from pawndb) against real Steam.
 *   Step 4 - async callback dispatcher (approach B-prime).  Hook
 *            the SteamAPI_Register{Callback,CallResult} /
 *            Unregister{...} / RunCallbacks exports via IAT
 *            patching, maintain a parallel registry so we own
 *            the source of truth for our synthetic SteamAPICall_t
 *            handles, and drain a pending-completion queue at the
 *            top of every RunCallbacks tick.  This step is pure
 *            plumbing: trampolines forward unchanged to real Steam,
 *            no game-visible behaviour change.  An internal
 *            self-test exercises the vtable Run() invocation path
 *            against a fake CCallbackBase fixture.  End-to-end
 *            synthesis (intercepting a real game call and serving
 *            an archived response) is Step 5.
 *
 * See HANDOFF.md for the full plan.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "pawnxfs.h"
#include "pawnsave.h"

#ifndef PAWNLIB_VERSION
#define PAWNLIB_VERSION "0.0.0-dev"
#endif

/* ============================================================================
 * Configuration (pawnlib.ini)
 * ----------------------------------------------------------------------------
 * Loaded once at DllMain before the log file is opened, so the `logging` knob
 * controls how the log is opened (or whether at all).  load_ini's own
 * log_line calls are silent pre-open (g_log still NULL); the final config
 * is re-logged from DllMain after open_log has run.
 * ============================================================================ */

enum log_mode { LOG_DISABLED = 0, LOG_TRUNCATE = 1, LOG_APPEND = 2 };

/* Compile-time hard cap on rift entries per leader_<N>; sizes the per-board
 * pawn_idx[] arrays.  g_max_search_results is the runtime (ini-tunable) cap
 * actually used by the DLE intercept's fill loop; clamped to [0, this]. */
#define MAX_ENTRIES_PER_LBE   100

static enum log_mode g_log_mode             = LOG_TRUNCATE;
static int           g_max_search_results   = MAX_ENTRIES_PER_LBE;
static int           g_enable_exports       = 1;      /* archive on inn rest */
static int           g_enable_updates       = 1;      /* writeback on save */

static char *trim_inplace(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = 0;
    }
    return s;
}

/* Forward decl: load_ini logs warnings, but log_line is a no-op while g_log
 * is NULL so the warnings just get discarded pre-open.  By design — same as
 * pawndb. */
static void log_line(const char *fmt, ...);

/* Conservative — invalid values keep the default and log a warning
 * (warnings are silently dropped pre-open and never reach disk; this is
 * by design, matching pawndb). */
static void load_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        log_line("load_ini: '%s' not found, using defaults", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim_inplace(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim_inplace(p);
        char *val = trim_inplace(eq + 1);
        if (!*key) continue;

        if (strcmp(key, "max_search_results") == 0) {
            int n = atoi(val);
            if (n >= 0 && n <= MAX_ENTRIES_PER_LBE) g_max_search_results = n;
            else log_line("load_ini: max_search_results=%d out of range (0..%d), keeping %d",
                          n, MAX_ENTRIES_PER_LBE, g_max_search_results);
        } else if (strcmp(key, "logging") == 0) {
            if      (strcmp(val, "disabled") == 0) g_log_mode = LOG_DISABLED;
            else if (strcmp(val, "truncate") == 0) g_log_mode = LOG_TRUNCATE;
            else if (strcmp(val, "append")   == 0) g_log_mode = LOG_APPEND;
            else log_line("load_ini: logging='%s' not in {disabled,truncate,append}, keeping default", val);
        } else if (strcmp(key, "enable_exports") == 0) {
            g_enable_exports = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "enable_updates") == 0) {
            g_enable_updates = atoi(val) ? 1 : 0;
        } else {
            log_line("load_ini: unknown key '%s'", key);
        }
    }
    fclose(f);
}

/* ============================================================================
 * Logging
 * ============================================================================ */

static FILE *g_log = NULL;
static CRITICAL_SECTION g_log_lock;

static const char *open_log(void)
{
    if (g_log_mode == LOG_DISABLED) return "disabled";

    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    if (n > 0 && n < sizeof(path)) {
        char *slash = strrchr(path, '\\');
        if (slash) { *(slash + 1) = 0; strncat(path, "pawnlib.log", sizeof(path) - strlen(path) - 1); }
        else       { strcpy(path, "pawnlib.log"); }
    } else {
        strcpy(path, "pawnlib.log");
    }
    const char *mode = (g_log_mode == LOG_APPEND) ? "a" : "w";
    g_log = fopen(path, mode);
    if (g_log) setvbuf(g_log, NULL, _IONBF, 0);
    return (g_log_mode == LOG_APPEND) ? "append" : "truncate";
}

static void log_line(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_log_lock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    LeaveCriticalSection(&g_log_lock);
}

/* ============================================================================
 * PE inspection helpers
 *
 * The module is already mapped, so RVAs in its directory entries resolve
 * to (base + rva).  steam_api.dll is 32-bit (DDDA.exe is 32-bit), so
 * IMAGE_NT_HEADERS32 is the right struct.
 * ============================================================================ */

static void log_pe_imports_exports(HMODULE mod) __attribute__((unused));
static void log_pe_imports_exports(HMODULE mod)
{
    BYTE *base = (BYTE*)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log_line("pe: bad DOS signature");
        return;
    }
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        log_line("pe: bad NT signature");
        return;
    }

    /* Imports - top-level DLL names only.  Tells us whether steam_api
     * pulls in steamclient.dll / vstdlib_s / tier0_s (real Steam) or
     * is self-contained (goldberg / gbe_fork). */
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (imp_rva) {
        IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp_rva);
        log_line("pe: imported DLLs:");
        while (imp->Name) {
            log_line("  %s", (const char*)(base + imp->Name));
            imp++;
        }
    } else {
        log_line("pe: no import directory");
    }

    /* Exports - every name.  This is the ground truth for which
     * accessor pattern we can use. */
    DWORD exp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (exp_rva) {
        IMAGE_EXPORT_DIRECTORY *exp = (IMAGE_EXPORT_DIRECTORY*)(base + exp_rva);
        DWORD *names = (DWORD*)(base + exp->AddressOfNames);
        log_line("pe: exports (%lu names, %lu functions):",
                 (unsigned long)exp->NumberOfNames,
                 (unsigned long)exp->NumberOfFunctions);
        for (DWORD i = 0; i < exp->NumberOfNames; i++) {
            log_line("  %s", (const char*)(base + names[i]));
        }
    } else {
        log_line("pe: no export directory");
    }
}

static int64_t file_size_on_disk(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    BOOL ok = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return ok ? sz.QuadPart : -1;
}

static void log_version_info(const char *path) __attribute__((unused));
static void log_version_info(const char *path)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeA(path, &handle);
    if (!size) {
        log_line("version: VS_VERSION_INFO absent (GLE=%lu)", GetLastError());
        return;
    }
    void *data = malloc(size);
    if (!data) return;

    if (GetFileVersionInfoA(path, 0, size, data)) {
        VS_FIXEDFILEINFO *ffi = NULL;
        UINT len = 0;
        if (VerQueryValueA(data, "\\", (LPVOID*)&ffi, &len) && ffi) {
            log_line("version: file=%u.%u.%u.%u product=%u.%u.%u.%u flags=0x%lx os=0x%lx type=0x%lx",
                     HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                     HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS),
                     HIWORD(ffi->dwProductVersionMS), LOWORD(ffi->dwProductVersionMS),
                     HIWORD(ffi->dwProductVersionLS), LOWORD(ffi->dwProductVersionLS),
                     (unsigned long)ffi->dwFileFlags,
                     (unsigned long)ffi->dwFileOS,
                     (unsigned long)ffi->dwFileType);
        }
        struct lang_codepage { WORD lang; WORD code; } *trans = NULL;
        UINT tlen = 0;
        if (VerQueryValueA(data, "\\VarFileInfo\\Translation", (LPVOID*)&trans, &tlen) &&
            trans && tlen >= sizeof(*trans)) {
            static const char *fields[] = {
                "CompanyName", "FileDescription", "FileVersion", "InternalName",
                "LegalCopyright", "OriginalFilename", "ProductName", "ProductVersion"
            };
            for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
                char sub[128];
                _snprintf(sub, sizeof(sub),
                          "\\StringFileInfo\\%04x%04x\\%s",
                          trans[0].lang, trans[0].code, fields[i]);
                char *value = NULL;
                UINT vlen = 0;
                if (VerQueryValueA(data, sub, (LPVOID*)&value, &vlen) && value) {
                    log_line("version:   %s = %s", fields[i], value);
                }
            }
        }
    } else {
        log_line("version: GetFileVersionInfoA failed (GLE=%lu)", GetLastError());
    }
    free(data);
}

/* ============================================================================
 * Steam SDK types
 *
 * Just enough of the SDK to type our hooks.  Identical to pawndb's; the
 * types are SDK-version-stable (only vtable slot indices and method
 * signatures shift across SDK revisions, and our hooks are typed
 * against the specific revision DDDA links against).
 * ============================================================================ */

typedef uint64_t SteamAPICall_t;
typedef uint64_t SteamLeaderboard_t;
typedef uint64_t SteamLeaderboardEntries_t;
typedef uint64_t CSteamID;
typedef uint64_t UGCHandle_t;
typedef int32_t  int32;
typedef int      ELeaderboardDataRequest;

#pragma pack(push, 8)
typedef struct LeaderboardEntry_t {
    CSteamID    m_steamIDUser;
    int32       m_nGlobalRank;
    int32       m_nScore;
    int32       m_cDetails;
    UGCHandle_t m_hUGC;
} LeaderboardEntry_t;
#pragma pack(pop)

/* ISteamUserStats vtable slots — same numbers pawndb uses against gbe_fork.
 * gbe_fork emulates the SDK interface version DDDA was built against, so
 * the layout matches whether the binary loads gbe_fork's stubs or real
 * Steam.  Slot 23 was verified at runtime as FindLeaderboard (Step 3). */
#define IUSERSTATS_FindOrCreateLeaderboard              22
#define IUSERSTATS_FindLeaderboard                      23
#define IUSERSTATS_GetLeaderboardName                   24
#define IUSERSTATS_GetLeaderboardEntryCount             25
#define IUSERSTATS_GetLeaderboardSortMethod             26
#define IUSERSTATS_GetLeaderboardDisplayType            27
#define IUSERSTATS_DownloadLeaderboardEntries           28
#define IUSERSTATS_DownloadLeaderboardEntriesForUsers   29
#define IUSERSTATS_GetDownloadedLeaderboardEntry        30
#define IUSERSTATS_UploadLeaderboardScore               31

/* ISteamRemoteStorage vtable slots — identical to pawndb's, which already
 * works against the same DDDA binary (gbe_fork emulates the same SDK version
 * DDDA links against, so the layout is shared between gbe_fork stubs and
 * real Steam's interface).  An earlier "+2 shift" theory in this file was
 * a misread: ISteamUserStats slots also match pawndb verbatim, not +2. */
#define IREMOTESTORAGE_UGCDownload                      21
#define IREMOTESTORAGE_UGCRead                          24

/* ISteamUser::BLoggedOn — polled by DDDA at ~60 Hz, used as the scrubber's
 * tick source.  See pawndb.c:196 for the same convention. */
#define IUSER_BLoggedOn                                 1

/* RemoteStorageDownloadUGCResult_t — k_iClientRemoteStorageCallbacks (1300) + 17. */
#define RSDUR_K_ICALLBACK 1317
#define RS_FILENAME_MAX   260
#pragma pack(push, 8)
typedef struct RemoteStorageDownloadUGCResult_s {
    int32       m_eResult;                          /* EResult: 1=k_EResultOK */
    UGCHandle_t m_hFile;
    uint32_t    m_nAppID;
    int32       m_nSizeInBytes;
    char        m_pchFileName[RS_FILENAME_MAX];
    uint64_t    m_ulSteamIDOwner;
} RemoteStorageDownloadUGCResult_t;
#pragma pack(pop)
typedef int EUGCReadAction;

/* ============================================================================
 * Vtable patching
 *
 * Direct pointer overwrite — same approach pawndb has used in production
 * for the last few months.  Page is normally read-only, so we wrap the
 * write in VirtualProtect.  No detour, no library dep.
 * ============================================================================ */

static int patch_vtable(void *instance, int method_index,
                        void *new_fn, void **orig_fn_out, const char *name)
{
    if (!instance) {
        log_line("patch_vtable(%s): NULL instance", name);
        return 0;
    }
    void **vtable = *(void***)instance;
    if (!vtable) {
        log_line("patch_vtable(%s): NULL vtable on instance %p", name, instance);
        return 0;
    }

    DWORD old_prot;
    if (!VirtualProtect(&vtable[method_index], sizeof(void*), PAGE_READWRITE, &old_prot)) {
        log_line("patch_vtable(%s): VirtualProtect failed, err=%lu", name, GetLastError());
        return 0;
    }
    *orig_fn_out = vtable[method_index];
    vtable[method_index] = new_fn;
    DWORD tmp;
    VirtualProtect(&vtable[method_index], sizeof(void*), old_prot, &tmp);

    log_line("patch_vtable(%s): slot %d was %p, now %p",
             name, method_index, *orig_fn_out, new_fn);
    return 1;
}

/* ============================================================================
 * IAT patching
 *
 * Steam_api.dll exports the dispatcher entry points (SteamAPI_RegisterCallback
 * etc.) as plain __cdecl C functions.  To intercept them we walk the import
 * table of every loaded module, find any IMAGE_IMPORT_DESCRIPTOR for
 * steam_api.dll, and overwrite the IAT slot for each named import we care
 * about.  This catches every caller that resolves the function via the
 * static-link import table — which is what the game does — without touching
 * the function bytes themselves.  Calls made via GetProcAddress would bypass
 * this; we don't expect any from the game proper, but we keep the originals
 * cached at install time so our own callsites still work.
 * ============================================================================ */

static int patch_module_iat(HMODULE mod, const char *dll_name,
                            const char *const *target_names,
                            void *const *replacements,
                            int target_count)
{
    BYTE *base = (BYTE*)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return 0;

    int patched = 0;
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp_rva);
    for (; imp->Name; imp++) {
        const char *name = (const char*)(base + imp->Name);
        if (_stricmp(name, dll_name) != 0) continue;

        /* OriginalFirstThunk is the names array (read-only); FirstThunk is the
         * IAT (writable, what we patch).  Some bound imports zero out
         * OriginalFirstThunk; fall back to FirstThunk for the names too. */
        IMAGE_THUNK_DATA32 *names = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA32*)(base + imp->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA32 *iat = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);

        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            IMAGE_IMPORT_BY_NAME *ibn =
                (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            for (int k = 0; k < target_count; k++) {
                if (strcmp((const char*)ibn->Name, target_names[k]) != 0) continue;

                DWORD old_prot;
                if (!VirtualProtect(&iat->u1.Function, sizeof(DWORD),
                                    PAGE_READWRITE, &old_prot)) {
                    log_line("iat: VirtualProtect failed for %s in %p (GLE=%lu)",
                             target_names[k], (void*)mod, GetLastError());
                    break;
                }
                log_line("iat: %s!%s in %p was 0x%08x, now %p",
                         dll_name, target_names[k], (void*)mod,
                         (unsigned)iat->u1.Function, replacements[k]);
                iat->u1.Function = (DWORD)(uintptr_t)replacements[k];
                DWORD tmp;
                VirtualProtect(&iat->u1.Function, sizeof(DWORD), old_prot, &tmp);
                patched++;
                break;
            }
        }
    }
    return patched;
}

static int patch_all_modules_iat(const char *dll_name,
                                 const char *const *names,
                                 void *const *fns, int n)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        log_line("iat: CreateToolhelp32Snapshot failed (GLE=%lu)", GetLastError());
        return 0;
    }
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    int total = 0;
    if (Module32First(snap, &me)) {
        do {
            total += patch_module_iat((HMODULE)me.modBaseAddr, dll_name, names, fns, n);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return total;
}

/* ============================================================================
 * Async callback dispatcher (Step 4 — approach B-prime)
 *
 * Goal: own the source of truth for *our* synthetic SteamAPICall_t handles
 * without poking real Steam's internal data segment.  We hook five exports
 * from steam_api.dll:
 *
 *     SteamAPI_RegisterCallback(CCallbackBase*, int iCallback)
 *     SteamAPI_UnregisterCallback(CCallbackBase*)
 *     SteamAPI_RegisterCallResult(CCallbackBase*, SteamAPICall_t hCall)
 *     SteamAPI_UnregisterCallResult(CCallbackBase*, SteamAPICall_t hCall)
 *     SteamAPI_RunCallbacks()
 *
 * On every register/unregister we update our parallel registry, then forward
 * to the original so Steam's own book-keeping stays consistent.  Synthetic
 * call handles (top bit set) are filtered out before forwarding —
 * Steam doesn't know about them.
 *
 * On every RunCallbacks we drain a pending-completion queue, looking up
 * matching subscribers in our parallel registry and invoking their
 * vtable Run() entry point directly.  We dispatch outside the registry lock
 * to avoid recursive deadlocks if a Run() handler calls back into
 * Register/Unregister (gbe_fork takes the same precaution in
 * SteamCallResults::runCallResults).
 *
 * CCallbackBase ABI (32-bit MSVC C++, no virtual dtor exported across DLL):
 *     [+0x00] vptr
 *     [+0x04] m_nCallbackFlags (uint8) + 3 bytes pad
 *     [+0x08] m_iCallback (int)
 *
 * Vtable layout:
 *     [0] virtual void Run(void *pvParam)                                   __thiscall
 *     [1] virtual void Run(void *pvParam, bool bIOFailure, SteamAPICall_t)  __thiscall
 *     [2] virtual int  GetCallbackSizeBytes()                               __thiscall
 *
 * Slot 0 is the CCallback path; slot 1 is the CCallResult path.  bIOFailure
 * is a bool, but x86 stack args are 4-byte aligned, so we pass BOOL and
 * the callee reads only the low byte — ABI-equivalent.
 * ============================================================================ */

#define CCB_FLAG_REGISTERED   0x01
#define CCB_FLAG_GAMESERVER   0x02

/* Synthetic SteamAPICall_t handles are tracked by explicit-set membership,
 * not by a bit test.  An earlier draft used "top bit set means ours" — that
 * collides with real Steam's namespace ~50% of the time (DDDA's save-load
 * registers a CCallResult on a real handle 0xd0f4..., we mis-classified it
 * as synthetic, didn't forward the registration to Steam, and the result
 * never fired -> hang). */
#define MAX_FAKE_IDS   1024

#define MAX_CB_SUBS    256
#define MAX_CR_SUBS    256
#define MAX_PENDING    128
#define MAX_PAYLOAD    512    /* RemoteStorageDownloadUGCResult_t is ~296 bytes (5c) */

typedef void (__thiscall *fn_CCB_Run1)(void *self, void *pvParam);
typedef void (__thiscall *fn_CCB_Run2)(void *self, void *pvParam,
                                       BOOL bIOFailure, SteamAPICall_t hCall);

typedef struct cb_sub_s  { void *cb; int iCallback; } cb_sub_t;
typedef struct cr_sub_s  { void *cb; SteamAPICall_t hCall; int iCallback; } cr_sub_t;
typedef struct pending_s {
    int            valid;
    SteamAPICall_t hCall;
    int            iCallback;
    int            io_failure;
    unsigned       size;
    char           payload[MAX_PAYLOAD];
} pending_t;

static CRITICAL_SECTION g_disp_lock;
static cb_sub_t  g_cb_subs[MAX_CB_SUBS];
static int       g_cb_count = 0;
static cr_sub_t  g_cr_subs[MAX_CR_SUBS];
static int       g_cr_count = 0;
static pending_t g_pending[MAX_PENDING];

/* Explicit registry of every synthetic call id we've ever minted.  Bounded
 * to MAX_FAKE_IDS — we never recycle slots, so an extremely long session
 * with heavy synthesis could overflow; the dispatcher will log and the
 * surplus IDs become "unrecognised" (forwarded to Steam, which will treat
 * them as unknown handles and ignore).  In practice the spike workload
 * (one synthesis per FindLeaderboard/DLE/UGCDownload during rift use) is
 * orders of magnitude below 1024 per session. */
static SteamAPICall_t g_fake_ids[MAX_FAKE_IDS];
static int            g_fake_id_count = 0;
/* Distinctive 0x9A.. high byte makes synthetic IDs trivially identifiable
 * in logs when we eyeball them.  Not a correctness mechanism — the set
 * above is.  Just a debugging aid. */
static SteamAPICall_t g_next_fake_call_id = 0x9A00000000000001ULL;

static SteamAPICall_t alloc_fake_call_id(void)
{
    EnterCriticalSection(&g_disp_lock);
    SteamAPICall_t id = g_next_fake_call_id++;
    if (g_fake_id_count < MAX_FAKE_IDS) {
        g_fake_ids[g_fake_id_count++] = id;
    } else {
        log_line("dispatcher: fake-id set full (%d); subsequent IDs may "
                 "leak to Steam as unknown handles", MAX_FAKE_IDS);
    }
    LeaveCriticalSection(&g_disp_lock);
    return id;
}

static int is_fake_call_id(SteamAPICall_t hCall)
{
    EnterCriticalSection(&g_disp_lock);
    int found = 0;
    for (int i = 0; i < g_fake_id_count; i++) {
        if (g_fake_ids[i] == hCall) { found = 1; break; }
    }
    LeaveCriticalSection(&g_disp_lock);
    return found;
}

static void register_cb_local(void *cb, int iCallback)
{
    EnterCriticalSection(&g_disp_lock);
    for (int i = 0; i < g_cb_count; i++) {
        if (g_cb_subs[i].cb == cb) {
            g_cb_subs[i].iCallback = iCallback;
            LeaveCriticalSection(&g_disp_lock);
            return;
        }
    }
    if (g_cb_count < MAX_CB_SUBS) {
        g_cb_subs[g_cb_count].cb = cb;
        g_cb_subs[g_cb_count].iCallback = iCallback;
        g_cb_count++;
    } else {
        log_line("dispatcher: cb subs table full; %p iCb=%d dropped", cb, iCallback);
    }
    LeaveCriticalSection(&g_disp_lock);
}

static void unregister_cb_local(void *cb)
{
    EnterCriticalSection(&g_disp_lock);
    for (int i = 0; i < g_cb_count; i++) {
        if (g_cb_subs[i].cb == cb) {
            g_cb_subs[i] = g_cb_subs[--g_cb_count];
            break;
        }
    }
    LeaveCriticalSection(&g_disp_lock);
}

static void register_cr_local(void *cb, SteamAPICall_t hCall)
{
    int iCb = -1;
    if (cb) iCb = ((int*)cb)[2];   /* offset 8 = m_iCallback */
    EnterCriticalSection(&g_disp_lock);
    if (g_cr_count < MAX_CR_SUBS) {
        g_cr_subs[g_cr_count].cb = cb;
        g_cr_subs[g_cr_count].hCall = hCall;
        g_cr_subs[g_cr_count].iCallback = iCb;
        g_cr_count++;
    } else {
        log_line("dispatcher: cr subs table full; %p call=0x%llx dropped",
                 cb, (unsigned long long)hCall);
    }
    LeaveCriticalSection(&g_disp_lock);
}

static void unregister_cr_local(void *cb, SteamAPICall_t hCall)
{
    EnterCriticalSection(&g_disp_lock);
    for (int i = 0; i < g_cr_count; i++) {
        if (g_cr_subs[i].cb == cb && g_cr_subs[i].hCall == hCall) {
            g_cr_subs[i] = g_cr_subs[--g_cr_count];
            break;
        }
    }
    LeaveCriticalSection(&g_disp_lock);
}

static int queue_pending(SteamAPICall_t hCall, int iCallback,
                         const void *payload, unsigned size, int io_failure)
{
    if (size > MAX_PAYLOAD) {
        log_line("dispatcher: queue_pending: payload too large (%u > %d)",
                 size, MAX_PAYLOAD);
        return 0;
    }
    EnterCriticalSection(&g_disp_lock);
    int slot = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].valid) { slot = i; break; }
    }
    if (slot >= 0) {
        g_pending[slot].hCall = hCall;
        g_pending[slot].iCallback = iCallback;
        g_pending[slot].io_failure = io_failure;
        g_pending[slot].size = size;
        if (size > 0 && payload) memcpy(g_pending[slot].payload, payload, size);
        g_pending[slot].valid = 1;
    } else {
        log_line("dispatcher: pending queue full; hCall=0x%llx iCb=%d dropped",
                 (unsigned long long)hCall, iCallback);
    }
    LeaveCriticalSection(&g_disp_lock);
    return slot >= 0;
}

static void invoke_cb_run1(void *cb, void *payload)
{
    void **vtbl = *(void***)cb;
    fn_CCB_Run1 run1 = (fn_CCB_Run1)vtbl[0];
    run1(cb, payload);
}

static void invoke_cb_run2(void *cb, void *payload,
                           int io_failure, SteamAPICall_t hCall)
{
    void **vtbl = *(void***)cb;
    fn_CCB_Run2 run2 = (fn_CCB_Run2)vtbl[1];
    run2(cb, payload, io_failure ? TRUE : FALSE, hCall);
}

/* All scratch buffers live in BSS, not on the stack — the v0.5.2 logs proved
 * the game's CCallResult user-handler is doing a wide write near pvParam
 * (delivered from stack-allocated pending) that clobbers nearby stack locals
 * including the iteration counts.  Putting snap / cb_snap / cr_snap and the
 * counters in static storage means a handler-side memset/memcpy through
 * pvParam can no longer corrupt our dispatch state.  Re-entrancy is guarded
 * by g_in_dispatch — if the user handler triggers another RunCallbacks
 * (and therefore another dispatch_pendings via our trampoline), the inner
 * call returns immediately rather than scribbling on the outer's buffers.
 * SteamAPI_RunCallbacks is single-threaded for our purposes (the game pumps
 * it from the main thread), so the static buffers don't need locking beyond
 * what already protects g_pending / g_cb_subs / g_cr_subs. */
static pending_t  g_disp_snap[MAX_PENDING];
static int        g_disp_snap_count = 0;
static cb_sub_t   g_disp_cb_snap[MAX_CB_SUBS];
static int        g_disp_cb_n = 0;
static cr_sub_t   g_disp_cr_snap[MAX_CR_SUBS];
static int        g_disp_cr_n = 0;
static volatile int g_in_dispatch = 0;

/* Drain pending completions: deliver each to matching subscribers in our
 * parallel registry.  Snapshot under lock, dispatch outside lock. */
/* Capture the return address at entry; check (and patch back) at exit so we
 * can survive a wide write from the game's CCallResult Run() handler that
 * clobbers our saved RIP at [ebp+4].  Diagnosed in 0.5.10: the trampoline
 * never sees post-dispatch trace lines, even though dispatch_pendings runs
 * cleanly to its closing brace, so the only thing left is the function
 * epilogue (`ret`) jumping to garbage.  Requires -fno-omit-frame-pointer. */
static void __attribute__((noinline)) dispatch_pendings(void)
{
    void *saved_ra = __builtin_return_address(0);

    if (g_in_dispatch) {
        log_line("dispatch: re-entered (call from inside a Run handler), skipping");
        return;
    }
    g_in_dispatch = 1;

    g_disp_snap_count = 0;

    EnterCriticalSection(&g_disp_lock);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i].valid) {
            g_disp_snap[g_disp_snap_count++] = g_pending[i];
            g_pending[i].valid = 0;
        }
    }
    LeaveCriticalSection(&g_disp_lock);

    for (int i = 0; i < g_disp_snap_count; i++) {
        pending_t *p = &g_disp_snap[i];

        g_disp_cb_n = 0;
        g_disp_cr_n = 0;

        EnterCriticalSection(&g_disp_lock);
        for (int j = 0; j < g_cr_count; j++) {
            if (g_cr_subs[j].hCall == p->hCall && g_disp_cr_n < MAX_CR_SUBS) {
                g_disp_cr_snap[g_disp_cr_n++] = g_cr_subs[j];
            }
        }
        for (int j = 0; j < g_cb_count; j++) {
            if (g_cb_subs[j].iCallback == p->iCallback && g_disp_cb_n < MAX_CB_SUBS) {
                g_disp_cb_snap[g_disp_cb_n++] = g_cb_subs[j];
            }
        }
        LeaveCriticalSection(&g_disp_lock);

        /* CCallResults are one-shot: fire then unregister.  The iteration
         * bound g_disp_cr_n lives in BSS and survives any stack writes the
         * user handler might do through pvParam. */
        for (int j = 0; j < g_disp_cr_n; j++) {
            if (g_disp_cr_snap[j].iCallback >= 0 &&
                g_disp_cr_snap[j].iCallback != p->iCallback) continue;
            if ((uintptr_t)g_disp_cr_snap[j].cb < 0x10000) {
                log_line("dispatch: cr %p BOGUS pointer, skipping",
                         g_disp_cr_snap[j].cb);
                continue;
            }
            invoke_cb_run2(g_disp_cr_snap[j].cb, p->payload,
                           p->io_failure, p->hCall);
            unregister_cr_local(g_disp_cr_snap[j].cb, g_disp_cr_snap[j].hCall);
        }
        /* CCallbacks are persistent: fire all matching subs, leave registered. */
        for (int j = 0; j < g_disp_cb_n; j++) {
            if ((uintptr_t)g_disp_cb_snap[j].cb < 0x10000) {
                log_line("dispatch: cb %p BOGUS pointer, skipping",
                         g_disp_cb_snap[j].cb);
                continue;
            }
            invoke_cb_run1(g_disp_cb_snap[j].cb, p->payload);
        }
    }

    g_in_dispatch = 0;

    /* Read the saved RIP slot directly via [ebp+4]; if corrupted, patch back
     * to the entry value so we return to trampoline_RunCallbacks instead of
     * jumping to garbage.  Diagnosed in 0.5.10: game's CCallResult Run
     * handler can do a wide write that clobbers our saved RIP — the value
     * we patched in is the only other lead if we ever need to root-cause
     * it, so we keep the recovery silent on the common path but log the
     * before/after on every actual hit. */
    {
        void *current_ra = NULL;
        __asm__ volatile ("movl 4(%%ebp), %0" : "=r"(current_ra));
        if (current_ra != saved_ra) {
            log_line("RA CORRUPT in dispatch_pendings: saved=%p current=%p — patching back",
                     saved_ra, current_ra);
            __asm__ volatile ("movl %0, 4(%%ebp)" :: "r"(saved_ra) : "memory");
        }
    }
}

/* Trampolines for the five hooked exports.  Each updates the parallel
 * registry, then forwards to the cached original (except for synthetic call
 * IDs, which Steam doesn't know about and must not see). */

typedef void (__cdecl *fn_SteamAPI_RegisterCallback)(void *cb, int iCallback);
typedef void (__cdecl *fn_SteamAPI_UnregisterCallback)(void *cb);
typedef void (__cdecl *fn_SteamAPI_RegisterCallResult)(void *cb, SteamAPICall_t hCall);
typedef void (__cdecl *fn_SteamAPI_UnregisterCallResult)(void *cb, SteamAPICall_t hCall);
typedef void (__cdecl *fn_SteamAPI_RunCallbacks)(void);

static fn_SteamAPI_RegisterCallback     g_orig_RegisterCallback     = NULL;
static fn_SteamAPI_UnregisterCallback   g_orig_UnregisterCallback   = NULL;
static fn_SteamAPI_RegisterCallResult   g_orig_RegisterCallResult   = NULL;
static fn_SteamAPI_UnregisterCallResult g_orig_UnregisterCallResult = NULL;
static fn_SteamAPI_RunCallbacks         g_orig_RunCallbacks         = NULL;

static void __cdecl trampoline_RegisterCallback(void *cb, int iCallback)
{
    register_cb_local(cb, iCallback);
    if (g_orig_RegisterCallback) g_orig_RegisterCallback(cb, iCallback);
}

static void __cdecl trampoline_UnregisterCallback(void *cb)
{
    unregister_cb_local(cb);
    if (g_orig_UnregisterCallback) g_orig_UnregisterCallback(cb);
}

static void __cdecl trampoline_RegisterCallResult(void *cb, SteamAPICall_t hCall)
{
    register_cr_local(cb, hCall);
    if (!is_fake_call_id(hCall) && g_orig_RegisterCallResult)
        g_orig_RegisterCallResult(cb, hCall);
}

static void __cdecl trampoline_UnregisterCallResult(void *cb, SteamAPICall_t hCall)
{
    unregister_cr_local(cb, hCall);
    if (!is_fake_call_id(hCall) && g_orig_UnregisterCallResult)
        g_orig_UnregisterCallResult(cb, hCall);
}

static void __cdecl trampoline_RunCallbacks(void)
{
    dispatch_pendings();
    if (g_orig_RunCallbacks) g_orig_RunCallbacks();
}

static int install_dispatcher_hooks(HMODULE steam_api)
{
    g_orig_RegisterCallback = (fn_SteamAPI_RegisterCallback)(void*)
        GetProcAddress(steam_api, "SteamAPI_RegisterCallback");
    g_orig_UnregisterCallback = (fn_SteamAPI_UnregisterCallback)(void*)
        GetProcAddress(steam_api, "SteamAPI_UnregisterCallback");
    g_orig_RegisterCallResult = (fn_SteamAPI_RegisterCallResult)(void*)
        GetProcAddress(steam_api, "SteamAPI_RegisterCallResult");
    g_orig_UnregisterCallResult = (fn_SteamAPI_UnregisterCallResult)(void*)
        GetProcAddress(steam_api, "SteamAPI_UnregisterCallResult");
    g_orig_RunCallbacks = (fn_SteamAPI_RunCallbacks)(void*)
        GetProcAddress(steam_api, "SteamAPI_RunCallbacks");

    if (!g_orig_RegisterCallback || !g_orig_UnregisterCallback ||
        !g_orig_RegisterCallResult || !g_orig_UnregisterCallResult ||
        !g_orig_RunCallbacks) {
        log_line("dispatcher: failed to resolve one or more exports; "
                 "RegCb=%p UnregCb=%p RegCR=%p UnregCR=%p Run=%p",
                 (void*)g_orig_RegisterCallback,
                 (void*)g_orig_UnregisterCallback,
                 (void*)g_orig_RegisterCallResult,
                 (void*)g_orig_UnregisterCallResult,
                 (void*)g_orig_RunCallbacks);
        return 0;
    }

    static const char *names[] = {
        "SteamAPI_RegisterCallback",
        "SteamAPI_UnregisterCallback",
        "SteamAPI_RegisterCallResult",
        "SteamAPI_UnregisterCallResult",
        "SteamAPI_RunCallbacks",
    };
    void *fns[] = {
        (void*)trampoline_RegisterCallback,
        (void*)trampoline_UnregisterCallback,
        (void*)trampoline_RegisterCallResult,
        (void*)trampoline_UnregisterCallResult,
        (void*)trampoline_RunCallbacks,
    };
    int n = patch_all_modules_iat("steam_api.dll", names, fns,
                                  sizeof(names)/sizeof(names[0]));
    log_line("dispatcher: %d IAT slot(s) patched across loaded modules", n);
    return n > 0;
}

/* ----------------------------------------------------------------------------
 * Self-test fixture
 *
 * We instantiate two CCallbackBase-shaped objects whose vtables point at
 * C functions we wrote, register them via the *internal* path (no Steam
 * roundtrip), queue a synthetic 24-byte payload (LeaderboardScoresDownloaded_t
 * shape), run dispatch, and check that the right Run() got invoked with the
 * right arguments.  This validates the vtable invocation logic and the
 * registry/dispatch flow without affecting the game.
 * ---------------------------------------------------------------------------- */

typedef struct selftest_cb_s {
    void   **vtbl;       /* offset 0 */
    uint8_t  flags;      /* offset 4 */
    uint8_t  pad[3];
    int      iCallback;  /* offset 8 */
    /* private fields (off the SDK ABI tail) */
    int            run1_hits;
    int            run2_hits;
    SteamAPICall_t last_hCall;
    int            last_io_failure;
    uint8_t        last_payload_byte0;
} selftest_cb_t;

static void __thiscall selftest_run1(selftest_cb_t *self, void *payload)
{
    self->run1_hits++;
    if (payload) self->last_payload_byte0 = *(uint8_t*)payload;
}

static void __thiscall selftest_run2(selftest_cb_t *self, void *payload,
                                     BOOL io_failure, SteamAPICall_t hCall)
{
    self->run2_hits++;
    self->last_io_failure = io_failure ? 1 : 0;
    self->last_hCall = hCall;
    if (payload) self->last_payload_byte0 = *(uint8_t*)payload;
}

static int __thiscall selftest_size(void *self) { (void)self; return 24; }

static void *g_selftest_vtbl[] = {
    (void*)selftest_run1,
    (void*)selftest_run2,
    (void*)selftest_size,
};

#define LBSD_K_ICALLBACK 1105   /* k_iSteamUserStatsCallbacks(1100) + 5 */

static int run_dispatcher_selftest(void)
{
    selftest_cb_t cr = { g_selftest_vtbl, 0, {0,0,0}, LBSD_K_ICALLBACK,
                         0, 0, 0, 0, 0 };
    selftest_cb_t cb = { g_selftest_vtbl, 0, {0,0,0}, LBSD_K_ICALLBACK,
                         0, 0, 0, 0, 0 };

    SteamAPICall_t hFake = alloc_fake_call_id();
    register_cr_local(&cr, hFake);
    register_cb_local(&cb, LBSD_K_ICALLBACK);

    /* 24-byte LeaderboardScoresDownloaded_t-shaped payload, with a sentinel
     * byte at offset 0 we can verify came through intact. */
    uint8_t payload[24];
    memset(payload, 0, sizeof(payload));
    payload[0] = 0xAB;
    queue_pending(hFake, LBSD_K_ICALLBACK, payload, sizeof(payload), 0);

    dispatch_pendings();

    int ok = 1;
    if (cr.run2_hits != 1) {
        log_line("  FAIL: cr.run2_hits=%d (want 1)", cr.run2_hits);
        ok = 0;
    }
    if (cr.last_hCall != hFake) {
        log_line("  FAIL: cr.last_hCall=0x%llx (want 0x%llx)",
                 (unsigned long long)cr.last_hCall,
                 (unsigned long long)hFake);
        ok = 0;
    }
    if (cr.last_io_failure != 0) {
        log_line("  FAIL: cr.last_io_failure=%d (want 0)", cr.last_io_failure);
        ok = 0;
    }
    if (cr.last_payload_byte0 != 0xAB) {
        log_line("  FAIL: cr.payload[0]=0x%02x (want 0xAB)",
                 cr.last_payload_byte0);
        ok = 0;
    }
    if (cb.run1_hits != 1) {
        log_line("  FAIL: cb.run1_hits=%d (want 1)", cb.run1_hits);
        ok = 0;
    }
    if (cb.last_payload_byte0 != 0xAB) {
        log_line("  FAIL: cb.payload[0]=0x%02x (want 0xAB)",
                 cb.last_payload_byte0);
        ok = 0;
    }

    /* CCallResult should auto-unregister on delivery. */
    EnterCriticalSection(&g_disp_lock);
    int still_registered = 0;
    for (int i = 0; i < g_cr_count; i++) {
        if (g_cr_subs[i].cb == &cr) { still_registered = 1; break; }
    }
    LeaveCriticalSection(&g_disp_lock);
    if (still_registered) {
        log_line("  FAIL: CCallResult still in registry after delivery");
        ok = 0;
    }

    unregister_cb_local(&cb);

    log_line("dispatcher self-test: %s", ok ? "PASS" : "FAIL");
    return ok;
}

/* ============================================================================
 * Step 5a — synthetic leaderboard responses (replace mode, hardcoded entry)
 *
 * Goal: validate the full async dispatch chain end-to-end without depending
 * on disk.  Intercept FindLeaderboard for any "archive board"
 * (leader_<N> per HANDOFF §6.4 inverse map), allocate a synthetic
 * SteamLeaderboard_t, queue a LeaderboardFindResult_t, and DON'T forward
 * to Steam.  When the game follows up with DLE on the synthetic hLB,
 * intercept again and queue a 1-entry LeaderboardScoresDownloaded_t with
 * a synthetic SteamLeaderboardEntries_t.  When the game then calls
 * GetDownloadedLeaderboardEntry on the synthetic hLBE, hand back one
 * hardcoded entry — "PAWNLIB!" packed as cDetails=18, fixed steamID,
 * fixed rank/score.  No archive folder is read in this step (5b adds
 * that); no UGC handle is wired (5c adds that).
 *
 * Special-purpose boards (leader_0/119/227/231/232 and the unobserved
 * gap leader_99..124) pass through to real Steam unchanged — we will
 * intercept leader_231 in 5c for the summon path, but for 5a leave it
 * alone so the user can still summon live online pawns.
 * ============================================================================ */

#define LBFR_K_ICALLBACK 1104   /* k_iSteamUserStatsCallbacks(1100) + 4 */
#define LBSU_K_ICALLBACK 1106   /* k_iSteamUserStatsCallbacks(1100) + 6 */

#pragma pack(push, 8)
typedef struct LeaderboardFindResult_s {
    SteamLeaderboard_t m_hSteamLeaderboard;
    uint8_t            m_bLeaderboardFound;
    uint8_t            _pad[7];
} LeaderboardFindResult_t;

typedef struct LeaderboardScoresDownloaded_s {
    SteamLeaderboard_t        m_hSteamLeaderboard;
    SteamLeaderboardEntries_t m_hSteamLeaderboardEntries;
    int32                     m_cEntryCount;
    int32                     _pad;
} LeaderboardScoresDownloaded_t;

typedef struct LeaderboardScoreUploaded_s {
    uint8_t            m_bSuccess;
    uint8_t            _pad1[7];
    SteamLeaderboard_t m_hSteamLeaderboard;
    int32              m_nScore;
    uint8_t            m_bScoreChanged;
    uint8_t            _pad2[3];
    int                m_nGlobalRankNew;
    int                m_nGlobalRankPrevious;
} LeaderboardScoreUploaded_t;
#pragma pack(pop)

#define MAX_FAKE_LBE          256
#define MAX_PAWNS             1024
/* MAX_ENTRIES_PER_LBE lives in the Configuration section near the top so
 * load_ini can clamp g_max_search_results against it. */

/* Per-pawn record built from the on-disk archive set at startup.  One per
 * .pawn file under <dll_dir>/pawnlib/<NNN>/.  Identity (steamid + UGC pair)
 * is derived deterministically from the .pawn filename stem so subsequent
 * sessions agree on identity, AND so the summon path (5c) can hash a UGC
 * handle the game requests back to a stem -> .pawn file. */
typedef struct fake_pawn_s {
    CSteamID    steamid;
    UGCHandle_t ugc_main;             /* distinct handle per fake; matched in UGCDownload (5c) */
    UGCHandle_t ugc_preview;          /* preview-channel handle (5c) */
    char        pawn_path[MAX_PATH];
    char        meta_path[MAX_PATH];
    int32       details[18];          /* loaded from .meta when has_details=1 */
    int         has_details;
    int         level;                /* parsed from containing folder name (zero-padded "NNN") */
    uint64_t    mtime;                /* FILETIME of .pawn; sort key for "most recent first" */
} fake_pawn_t;

static fake_pawn_t      g_pawns[MAX_PAWNS];
static int              g_pawn_count = 0;
/* g_pawns is built once at worker startup (read-only afterwards from the
 * game's main thread).  Access through g_disp_lock for the publish barrier. */

/* Step 5a no longer mints fake hLBs — FindLeaderboard always forwards.
 * We do still mint fake hLBE values, since the LBSD synthesis hands one
 * back to the game which then passes it to GetDownloadedLeaderboardEntry.
 * Real hLBE values seen in passive logs are tiny (0x30 etc.); use a
 * uniquely-banded small value so the trace makes synth vs. real obvious. */
typedef struct fake_lbe_s {
    SteamLeaderboardEntries_t hLBE;
    int                       board_num;
    int                       archive_level;
    int                       entry_count;
    int                       range_start;
    int                       pawn_idx[MAX_ENTRIES_PER_LBE];  /* indices into g_pawns[] */
} fake_lbe_t;

static fake_lbe_t g_fake_lbes[MAX_FAKE_LBE];
static int        g_fake_lbe_count = 0;
static SteamLeaderboardEntries_t g_next_fake_lbe = 0x000E0001ULL;

/* Resolved at DllMain to <pawnlib.dll's directory>; includes trailing backslash. */
static char g_dll_dir[MAX_PATH] = {0};

static int parse_leader_name(const char *name, int *out_n)
{
    if (!name) return 0;
    if (strncmp(name, "leader_", 7) != 0) return 0;
    char *end = NULL;
    long n = strtol(name + 7, &end, 10);
    if (end == name + 7 || *end) return 0;
    *out_n = (int)n;
    return 1;
}

/* HANDOFF §6.4: the player-visible "level X" search board is leader_<f(X)>
 * where f(X) = X for X in 1..98 and X+26 for X in 99..200.  This inverse
 * recovers the archive folder name from the leader_<N> we see on the wire. */
static int board_to_archive_level(int N)
{
    if (N >= 1   && N <=  98) return N;
    if (N >= 125 && N <= 226) return N - 26;
    return -1;
}

/* Pass-through list per HANDOFF §6.4.  leader_231 is the summon registry
 * (will be intercepted in 5c).  leader_99..124 is the unobserved gap;
 * treat as special until proven otherwise. */
static int is_special_board(int N)
{
    if (N == 0 || N == 119 || N == 227 || N == 231 || N == 232) return 1;
    if (N >= 99 && N <= 124) return 1;
    return 0;
}

static int register_fake_lbe(SteamLeaderboardEntries_t hLBE, int board_num,
                              int archive_level, int entry_count, int range_start,
                              const int *pawn_idx)
{
    EnterCriticalSection(&g_disp_lock);
    int ok = 0;
    if (g_fake_lbe_count < MAX_FAKE_LBE) {
        fake_lbe_t *e = &g_fake_lbes[g_fake_lbe_count++];
        e->hLBE = hLBE;
        e->board_num = board_num;
        e->archive_level = archive_level;
        e->entry_count = entry_count;
        e->range_start = range_start;
        for (int i = 0; i < entry_count && i < MAX_ENTRIES_PER_LBE; i++)
            e->pawn_idx[i] = pawn_idx[i];
        ok = 1;
    }
    LeaveCriticalSection(&g_disp_lock);
    return ok;
}

static int find_fake_lbe(SteamLeaderboardEntries_t hLBE, fake_lbe_t *out)
{
    EnterCriticalSection(&g_disp_lock);
    int found = 0;
    for (int i = 0; i < g_fake_lbe_count; i++) {
        if (g_fake_lbes[i].hLBE == hLBE) { *out = g_fake_lbes[i]; found = 1; break; }
    }
    LeaveCriticalSection(&g_disp_lock);
    return found;
}

/* ============================================================================
 * Step 5b — per-pawn registry built from <dll_dir>/pawnlib/<NNN>/[stem].pawn
 * ============================================================================ */

/* FNV-1a 32-bit. Same as pawndb's hash_str so identities derived here match
 * pawndb's if anyone ever copies archives between the two. */
static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)(*s++); h *= 16777619u; }
    return h;
}

/* Identity derivation, port of pawndb's: keeps the standard 0x01100001 upper
 * (public-universe individual + desktop instance), account_id in 0x80000000+
 * band so it can't collide with a real Steam account.  ugc_main / ugc_preview
 * use the 0xFAFA0001 / 0xFAFA0002 prefix so 5c can distinguish them from real
 * gbe_fork / FileShare-issued handles in UGCDownload. */
static void derive_fake_identity(const char *stem, CSteamID *out_sid,
                                  UGCHandle_t *out_main, UGCHandle_t *out_preview)
{
    uint32_t h = hash_str(stem);
    uint32_t acct = 0x80000000u | (h & 0x7fffffffu);
    *out_sid     = 0x0110000100000000ULL | (uint64_t)acct;
    *out_main    = 0xFAFA000100000000ULL | (uint64_t)h;
    *out_preview = 0xFAFA000200000000ULL | (uint64_t)h;
}

/* Parse "NNN" (zero-padded N-digit decimal) -> N. 0 on malformed. */
static int parse_level_folder(const char *name)
{
    if (!name || !*name) return 0;
    int v = 0;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 100000) return 0;
    }
    return v;
}

/* Load a 72-byte .meta sidecar (18 int32) into fp. Sets has_details=1 on
 * success, 0 on failure (caller leaves the slot zeroed; rift will still
 * accept it but display will be generic). */
static void load_fake_meta(fake_pawn_t *fp)
{
    fp->has_details = 0;
    memset(fp->details, 0, sizeof(fp->details));
    FILE *f = fopen(fp->meta_path, "rb");
    if (!f) return;
    if (fread(fp->details, sizeof(int32), 18, f) == 18) fp->has_details = 1;
    fclose(f);
}

static int cmp_fake_mtime_desc(const void *a, const void *b)
{
    uint64_t ma = ((const fake_pawn_t*)a)->mtime;
    uint64_t mb = ((const fake_pawn_t*)b)->mtime;
    if (mb > ma) return  1;
    if (mb < ma) return -1;
    return 0;
}

/* One-shot scan of <dll_dir>/pawnlib/<NNN>/[stem].pawn.  Sorts descending by
 * mtime so cohort responses pick most-recent-first, matching pawndb. */
static int rescan_all_levels(void)
{
    EnterCriticalSection(&g_disp_lock);
    g_pawn_count = 0;

    char root[MAX_PATH];
    _snprintf(root, sizeof(root), "%spawnlib", g_dll_dir);
    for (char *p = root; *p; p++) if (*p == '/') *p = '\\';

    char pat[MAX_PATH];
    _snprintf(pat, sizeof(pat), "%s\\*", root);

    int with_meta = 0, levels_scanned = 0;

    WIN32_FIND_DATAA fd_lvl;
    HANDLE h_lvl = FindFirstFileA(pat, &fd_lvl);
    if (h_lvl != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd_lvl.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            int level = parse_level_folder(fd_lvl.cFileName);
            if (level < 1 || level > 200) continue;
            levels_scanned++;

            char sub_dir[MAX_PATH];
            _snprintf(sub_dir, sizeof(sub_dir), "%s\\%s", root, fd_lvl.cFileName);
            char sub_pat[MAX_PATH];
            _snprintf(sub_pat, sizeof(sub_pat), "%s\\*.pawn", sub_dir);

            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(sub_pat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (g_pawn_count >= MAX_PAWNS) break;

                fake_pawn_t *fp = &g_pawns[g_pawn_count];
                memset(fp, 0, sizeof(*fp));
                _snprintf(fp->pawn_path, sizeof(fp->pawn_path), "%s\\%s", sub_dir, fd.cFileName);

                char stem[64];
                strncpy(stem, fd.cFileName, sizeof(stem) - 1);
                stem[sizeof(stem) - 1] = 0;
                char *dot = strrchr(stem, '.');
                if (dot) *dot = 0;
                _snprintf(fp->meta_path, sizeof(fp->meta_path), "%s\\%s.meta", sub_dir, stem);

                derive_fake_identity(stem, &fp->steamid, &fp->ugc_main, &fp->ugc_preview);
                load_fake_meta(fp);
                if (fp->has_details) with_meta++;
                fp->level = level;
                fp->mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32)
                          | (uint64_t)fd.ftLastWriteTime.dwLowDateTime;
                g_pawn_count++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        } while (FindNextFileA(h_lvl, &fd_lvl));
        FindClose(h_lvl);
    }

    qsort(g_pawns, g_pawn_count, sizeof(g_pawns[0]), cmp_fake_mtime_desc);

    int n = g_pawn_count;
    LeaveCriticalSection(&g_disp_lock);

    log_line("rescan_all_levels: loaded %d fake pawn(s) across %d level folder(s) (%d with .meta)",
             n, levels_scanned, with_meta);
    return n;
}

/* ============================================================================
 * Step 6a — Inn-rest archive
 * ----------------------------------------------------------------------------
 * On every inn rest DDDA emits a deterministic upload sequence:
 *   FileWrite('0', .pawn_blob, 8192)  <-- main-pawn blob into RemoteStorage
 *   FileShare('0')                    <-- arms g_rest_pending here
 *   UploadLeaderboardScore cDetails=2 (leader_231)
 *   FileWrite('1', preview_blob, ...) <-- preview, ignored
 *   FileShare('1')
 *   UploadLeaderboardScore cDetails=2 (leader_232)
 *   UploadLeaderboardScore cDetails=18 x ~4-6  <-- archive on first one
 *
 * We capture the FileWrite('0') buffer in RAM (no Steam-side disk lookup
 * needed) and write it to <dll_dir>/pawnlib/<NNN>/<HEX>.pawn on the first
 * cDetails=18, with the matching <HEX>.meta sidecar carrying that upload's
 * 18 int32s.  Then stamp the mArisenName cName slots inside the .pawn at
 * XFS offsets 0x36F0 / 0x375E with "<NNN>:<HEX>" so a future hire-back can
 * identify the source archive.
 * ============================================================================ */

#define IREMOTESTORAGE_FileWrite     0
#define IREMOTESTORAGE_FileShare     4

/* 2026-01-01 00:00:00 UTC, used so a u32 unix-epoch offset gives ~136 yrs of
 * unique 8-hex stems.  Identical to pawndb's value. */
#define EPOCH_2026_UTC               1767225600u

/* mArisenName cName slots inside the inflated XFS instance: both 0x36F0 and
 * 0x375E carry an identical 25-byte name.  Empirically MARISENNAME_CAP = 25. */
#define MARISENNAME_CAP              25
#define MARISENNAME_OFF_1            0x36F0u
#define MARISENNAME_OFF_2            0x375Eu

/* Pawn-memory layout — character data region starts 0xA7000 from *pBase, the
 * main pawn sits at +0x7F0 from there, and the level u16 at +0xDD0 from the
 * main pawn.  Same constants as pawndb (and ddda-dinput8 before it). */
#define OFF_BASE_REGION              0xA7000
#define PAWN_MAIN_OFFSET             0x7F0
#define OFF_LEVEL                    0xDD0

/* mCmc[] runtime layout — three contiguous cCmc instances starting at
 * mCmc[0] = pBase + 0xA77F0 (= OFF_BASE_REGION + PAWN_MAIN_OFFSET):
 *   mCmc[0] main pawn,  mCmc[1] Hired1,  mCmc[2] Hired2.
 * Stride is 0x1660 (cross-checked against the +0x0AAAB0 / +0x0AC110
 * netuid offsets the scrubber uses).  mArisenName.mEditName lives at
 * +0x1CDE within each cCmc — confirmed empirically by scanning for an
 * archive_rest stamped stem in mCmc[1] AND mCmc[2] of one session.  The
 * XFS-file double-stamp at 0x36F0/0x375E collapses to a single runtime
 * field; only one copy per slot is in live memory. */
#define MCMC_STRIDE                  0x1660u
#define MARISENNAME_RUNTIME_OFF      0x1CDEu

/* Per-cCmc runtime "alive" flag.  Value 0x01 if the slot is occupied by a
 * living pawn, 0x00 if empty or dead.  Cross-checked across five save-time
 * memory dumps with main + 2 hired pawns transitioned through
 * empty/alive/dead states: a single hit per slot, all at cCmc-relative
 * offset 0x6D4.
 *
 * Distinct from the save XML's top-level mCmcBeFlag bool[3] — that array
 * is a save-time aggregate populated by the serializer reading from THIS
 * field on each cCmc.  We use the underlying per-cCmc field because
 * (a) it lives inside the same struct as mLevel / mArisenName, (b) it
 * updates in real time (not just at FileWrite), and (c) it cleanly clears
 * to 00 on death/release so the rift filter self-heals. */
#define MCMC_ALIVE_FLAG_OFF          0x6D4u

/* Rest-cycle state.  Single-threaded with respect to the game's main thread;
 * no lock needed — the FileWrite/FileShare/UploadLeaderboardScore hooks all
 * fire on the same thread that pumps Steam callbacks. */
static int    g_rest_pending          = 0;
static BYTE  *g_pending_pawn_blob     = NULL;
static long   g_pending_pawn_size     = 0;
/* g_enable_exports / g_enable_updates / g_log_mode / g_max_search_results
 * live in the Configuration section near the top of this file (pawnlib.ini
 * is parsed by load_ini at DllMain). */

/* Archive stem of the most recent successful archive_rest, in "NNN:HHHHHHHH"
 * form.  Read by 6b's hook_FileWrite('DDDA.sav') to drop the matching .xml
 * sidecar.  Also handy in logs for cross-referencing rests with archives. */
static char   g_last_archive_stem_full[16] = {0};

/* One-shot flag: armed by archive_rest, drained by the inn-rest auto-save's
 * Path A (main-pawn .xml snapshot).  Without this gate, every save would
 * overwrite the .xml with whatever drift state mCmc[0] happens to have at
 * that moment — capturing the player's main pawn after level-ups / gear
 * changes that happened between the rest and the save.  By tying the
 * snapshot to the FIRST save after archive_rest (which is the inn-rest's
 * own auto-save, fired immediately after the upload cluster), we ensure
 * the .xml's contents match the .pawn captured at the same rest. */
static int    g_archive_xml_pending      = 0;

/* Resolve (PAWN_MAIN_OFFSET + local_offset) to a pointer into live memory.
 * Returns NULL until a save is loaded (which is when *g_pawn_base_var goes
 * non-NULL).  g_pawn_base_var is set up by discover_pawn_base() in the
 * scrubber section below. */
static BYTE **g_pawn_base_var; /* fwd decl — defined in 6c section */

static void *pawn_ptr_main(int local_offset)
{
    if (!g_pawn_base_var) return NULL;
    BYTE *base = *g_pawn_base_var;
    if (!base) return NULL;
    return base + OFF_BASE_REGION + PAWN_MAIN_OFFSET + local_offset;
}

static int read_pawn_level(void)
{
    uint16_t *pLevel = (uint16_t*)pawn_ptr_main(OFF_LEVEL);
    if (!pLevel) return 0;
    return (int)(*pLevel);
}

/* Returns 1 if hired-pawn slot `s` (1 or 2) is occupied by a living pawn,
 * 0 if the slot is empty/dead, or if no save is loaded yet.  Reads the
 * cCmc runtime alive flag at MCMC_ALIVE_FLAG_OFF (see comment block where
 * MCMC_ALIVE_FLAG_OFF is defined for how this was discovered). */
static int read_hired_alive(int s)
{
    if (s < 1 || s > 2) return 0;
    if (!g_pawn_base_var) return 0;
    BYTE *base = *g_pawn_base_var;
    if (!base) return 0;
    BYTE v = *(base + OFF_BASE_REGION + PAWN_MAIN_OFFSET
                    + (uint32_t)s * MCMC_STRIDE
                    + MCMC_ALIVE_FLAG_OFF);
    return v == 0x01;
}

/* Read the in-memory mArisenName for hired-pawn slot `s` (s=1 -> mCmc[1],
 * s=2 -> mCmc[2]).  Copies up to MARISENNAME_CAP-1 printable ASCII bytes
 * into out[], NUL-terminating at the first non-printable byte or at the
 * cap.  Returns 1 if any bytes were copied, 0 if no save is loaded, the
 * slot is out of range, or the field starts with a non-printable byte.
 *
 * NOTE: the bytes can persist after release/death — the game doesn't
 * always clear mArisenName when a hired pawn leaves the party.  Callers
 * that need a "currently in the party" check should gate this with
 * read_hired_alive(). */
static int read_hired_arisen(int s, char *out, size_t out_cap)
{
    if (out_cap == 0) return 0;
    out[0] = 0;
    if (s < 1 || s > 2) return 0;
    if (!g_pawn_base_var) return 0;
    BYTE *base = *g_pawn_base_var;
    if (!base) return 0;

    const BYTE *p = base + OFF_BASE_REGION + PAWN_MAIN_OFFSET
                  + (uint32_t)s * MCMC_STRIDE
                  + MARISENNAME_RUNTIME_OFF;
    size_t cap = out_cap - 1;
    if (cap > MARISENNAME_CAP) cap = MARISENNAME_CAP;
    size_t i = 0;
    for (; i < cap; i++) {
        BYTE c = p[i];
        if (c < 0x20 || c >= 0x7F) break;
        out[i] = (char)c;
    }
    out[i] = 0;
    return i > 0;
}

/* Write 18 int32 to <stem>.meta.  Returns 1 on success. */
static int write_meta(const char *pawn_path, const int32 *details)
{
    char dst[MAX_PATH];
    _snprintf(dst, sizeof(dst), "%s", pawn_path);
    char *ext = strrchr(dst, '.');
    if (!ext) return 0;
    strcpy(ext, ".meta");

    FILE *f = fopen(dst, "wb");
    if (!f) {
        log_line("write_meta: fopen '%s' failed errno=%d", dst, errno);
        return 0;
    }
    size_t w = fwrite(details, sizeof(int32), 18, f);
    fclose(f);
    if (w != 18) {
        log_line("write_meta: short write to '%s' (%zu of 18)", dst, w);
        return 0;
    }
    log_line("write_meta: wrote '%s'", dst);
    return 1;
}

static void archive_rest(const int32 *fresh_details)
{
    if (!g_pending_pawn_blob || g_pending_pawn_size <= 0) {
        log_line("archive_rest: no pending FileWrite('0') blob captured; skipping");
        return;
    }

    int level = read_pawn_level();
    if (level < 1 || level > 200) {
        log_line("archive_rest: pawn level %d out of range (need 1..200); skipping", level);
        return;
    }

    uint32_t epoch_off = (uint32_t)((uint64_t)time(NULL) - (uint64_t)EPOCH_2026_UTC);
    char stem_hex[16];
    _snprintf(stem_hex, sizeof(stem_hex), "%08X", epoch_off);
    char stem_full[16];
    _snprintf(stem_full, sizeof(stem_full), "%03u:%s", (unsigned)level, stem_hex);

    char dir[MAX_PATH];
    _snprintf(dir, sizeof(dir), "%spawnlib\\%03u", g_dll_dir, (unsigned)level);
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';
    /* Idempotent mkdir -p of every prefix. */
    for (char *p = dir + 1; *p; p++) {
        if (*p == '\\') { *p = 0; CreateDirectoryA(dir, NULL); *p = '\\'; }
    }
    CreateDirectoryA(dir, NULL);

    char dst[MAX_PATH];
    _snprintf(dst, sizeof(dst), "%s\\%s.pawn", dir, stem_hex);

    FILE *fo = fopen(dst, "wb");
    if (!fo) {
        log_line("archive_rest: fopen '%s' failed errno=%d", dst, errno);
        return;
    }
    size_t wrote = fwrite(g_pending_pawn_blob, 1, (size_t)g_pending_pawn_size, fo);
    fclose(fo);
    if (wrote != (size_t)g_pending_pawn_size) {
        log_line("archive_rest: short write to '%s' (%zu of %ld)",
                 dst, wrote, g_pending_pawn_size);
        return;
    }
    log_line("archive_rest: wrote %ld bytes (level %d) to '%s'",
             g_pending_pawn_size, level, dst);

    write_meta(dst, fresh_details);

    /* Stamp mArisenName at 0x36F0 / 0x375E so a future hire-back can self-
     * identify against the archive set.  Failure is non-fatal — the .pawn
     * is still playable, just without release-time writeback (6b). */
    uint8_t name_buf[MARISENNAME_CAP];
    memset(name_buf, 0, sizeof(name_buf));
    size_t slen = strlen(stem_full);
    if (slen > MARISENNAME_CAP - 1) slen = MARISENNAME_CAP - 1;
    memcpy(name_buf, stem_full, slen);
    struct pawnxfs_patch patches[2] = {
        { MARISENNAME_OFF_1, name_buf, sizeof(name_buf) },
        { MARISENNAME_OFF_2, name_buf, sizeof(name_buf) },
    };
    char perr[128] = {0};
    int rc = pawnxfs_poke(dst, dst, patches, 2, perr, sizeof(perr));
    if (rc != 0) {
        log_line("archive_rest: mArisenName stamp failed rc=%d (%s) on '%s'",
                 rc, perr, dst);
    } else {
        log_line("archive_rest: stamped mArisenName='%s' into '%s'",
                 stem_full, dst);
    }

    strncpy(g_last_archive_stem_full, stem_full, sizeof(g_last_archive_stem_full) - 1);
    g_last_archive_stem_full[sizeof(g_last_archive_stem_full) - 1] = 0;

    /* Arm the one-shot Path A guard: the FIRST FileWrite('DDDA.sav') after
     * this rest (which is the inn-rest auto-save itself) will snapshot
     * mCmc[0] into <stem>.xml; subsequent saves leave the .xml untouched. */
    g_archive_xml_pending = 1;

    /* Make the new archive browsable in this same session.  Pawndb didn't do
     * this (it relied on next session's startup scan); we should. */
    rescan_all_levels();
}

/* ============================================================================
 * Step 6b — DDDA.sav writeback
 * ----------------------------------------------------------------------------
 * On every save (FileWrite('DDDA.sav', ...)), parse the inflated XML for
 * cSAVE_DATA_CMC blocks tagged with our mArisenName stem ("NNN:HEX"), then
 * patch the source <NNN>/<HEX>.pawn archive on disk with the rental's gear
 * (12 slots) and knowledge (2x322 u32s).  Also drop a <HEX>.xml sidecar for
 * the player's own main pawn (the cSAVE_DATA_CMC region for mCmc[0]) — the
 * 6e companion CLI splices it back into a future save's main-pawn slot.
 * All offsets ported verbatim from pawndb (empirically validated).
 * ============================================================================ */

#define WB_GEAR_BASE              0x2098u
#define WB_GEAR_STRIDE            0x46u
#define WB_GEAR_COUNT             12
#define WB_ITEM_OFF               0x02u
#define WB_FLAG_OFF               0x08u
#define WB_EQUIPPED_BIT           0x80u
#define WB_STUDY_FLAG_OFF         0x282Cu
#define WB_LOCAL_STUDY_FLAG_OFF   0x2D38u
#define WB_STUDY_COUNT            322

/* Parse "NNN:HHHHHHHH" -> (level, stem).  Returns 1 on success, 0 on any
 * format error.  Used to decode a hired pawn's mArisenName back into the
 * archive coordinates archive_rest stamped into it. */
static int parse_arisen_stem(const char *s, int *out_level,
                             char *out_stem, size_t out_stem_cap)
{
    if (!s || !out_level || !out_stem || out_stem_cap < 9) return 0;
    for (int i = 0; i < 3; i++) if (s[i] < '0' || s[i] > '9') return 0;
    if (s[3] != ':') return 0;
    for (int i = 0; i < 8; i++) {
        char c = s[4 + i];
        int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return 0;
    }
    int level = (s[0]-'0')*100 + (s[1]-'0')*10 + (s[2]-'0');
    if (level < 1 || level > 200) return 0;
    *out_level = level;
    memcpy(out_stem, s + 4, 8);
    out_stem[8] = 0;
    return 1;
}

/* Drop a <HEX>.xml sidecar next to <HEX>.pawn carrying a verbatim
 * cSAVE_DATA_CMC region (the bytes restore_pawn later splices into a target
 * save's main-pawn slot).  Returns 0 on success, -1 on parse / IO failure. */
static int write_pawn_xml_sidecar(const char *stem_full,
                                  const uint8_t *xml, size_t xml_len,
                                  const char *origin_label)
{
    int  level = 0;
    char stem[16] = {0};
    if (!parse_arisen_stem(stem_full, &level, stem, sizeof(stem))) {
        log_line("pawnxml: %s — stem '%s' not parseable, skipping sidecar",
                 origin_label, stem_full ? stem_full : "(null)");
        return -1;
    }
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%spawnlib\\%03d\\%s.xml",
              g_dll_dir, level, stem);
    for (char *q = path; *q; q++) if (*q == '/') *q = '\\';

    FILE *f = fopen(path, "wb");
    if (!f) {
        log_line("pawnxml: %s — fopen('%s', wb) failed errno=%d",
                 origin_label, path, errno);
        return -1;
    }
    size_t w = fwrite(xml, 1, xml_len, f);
    fclose(f);
    if (w != xml_len) {
        log_line("pawnxml: %s — short write (%u of %u) to '%s'",
                 origin_label, (unsigned)w, (unsigned)xml_len, path);
        return -1;
    }
    log_line("pawnxml: %s — wrote %u bytes to '%s'",
             origin_label, (unsigned)xml_len, path);
    return 0;
}

/* Patch the source archive with the 12 equipment records and the two
 * 322-entry knowledge arrays from the save XML.  Source identified by
 * mArisenName, which archive_rest stamped with "NNN:HEX" at write time —
 * so the save itself is authoritative; no hire-binding state needed. */
static void writeback_to_archive(int slot, const struct pawnsave_hired_info *info)
{
    if (!info->present) {
        log_line("writeback: slot %d — no matching cSAVE_DATA_CMC block, skipping", slot);
        return;
    }

    int  level = 0;
    char stem[16] = {0};
    if (!parse_arisen_stem(info->creator_name, &level, stem, sizeof(stem))) {
        log_line("writeback: slot %d — mArisenName='%s' not a pawnlib stem, skipping",
                 slot, info->creator_name);
        return;
    }

    char pawn_path[MAX_PATH];
    _snprintf(pawn_path, sizeof(pawn_path), "%spawnlib\\%03d\\%s.pawn",
              g_dll_dir, level, stem);
    for (char *q = pawn_path; *q; q++) if (*q == '/') *q = '\\';

    if (GetFileAttributesA(pawn_path) == INVALID_FILE_ATTRIBUTES) {
        log_line("writeback: slot %d (%03d:%s) archive not found — '%s'",
                 slot, level, stem, pawn_path);
        return;
    }

    /* Build patch list: 2 patches per gear slot (mItemNo s16, mFlag u32 with
     * the 0x80 'equipped' bit cleared — the game re-applies it on load) plus
     * 1 whole-buffer memcpy per study array.  Backing bytes live on this
     * function's stack / inside `info` for the duration of pawnxfs_poke. */
    struct pawnxfs_patch patches[WB_GEAR_COUNT * 2 + 2];
    uint8_t              item_bytes[WB_GEAR_COUNT][2];
    uint8_t              flag_bytes[WB_GEAR_COUNT][4];
    int npatches = 0;
    for (int k = 0; k < WB_GEAR_COUNT; k++) {
        uint32_t base = WB_GEAR_BASE + (uint32_t)k * WB_GEAR_STRIDE;
        int16_t  item = info->gear[k].mItemNo;
        uint32_t flag = info->gear[k].mFlag & ~(uint32_t)WB_EQUIPPED_BIT;

        item_bytes[k][0] = (uint8_t)(item & 0xFF);
        item_bytes[k][1] = (uint8_t)((item >> 8) & 0xFF);
        flag_bytes[k][0] = (uint8_t)(flag & 0xFF);
        flag_bytes[k][1] = (uint8_t)((flag >> 8) & 0xFF);
        flag_bytes[k][2] = (uint8_t)((flag >> 16) & 0xFF);
        flag_bytes[k][3] = (uint8_t)((flag >> 24) & 0xFF);

        patches[npatches++] = (struct pawnxfs_patch){
            .offset = base + WB_ITEM_OFF, .bytes = item_bytes[k], .len = 2,
        };
        patches[npatches++] = (struct pawnxfs_patch){
            .offset = base + WB_FLAG_OFF, .bytes = flag_bytes[k], .len = 4,
        };
    }
    /* u32 LE on disk == host u32 on x86, so direct point-at-buffer. */
    patches[npatches++] = (struct pawnxfs_patch){
        .offset = WB_STUDY_FLAG_OFF,
        .bytes  = (const uint8_t *)info->study_flag,
        .len    = WB_STUDY_COUNT * sizeof(uint32_t),
    };
    patches[npatches++] = (struct pawnxfs_patch){
        .offset = WB_LOCAL_STUDY_FLAG_OFF,
        .bytes  = (const uint8_t *)info->local_study_flag,
        .len    = WB_STUDY_COUNT * sizeof(uint32_t),
    };

    char err[128] = {0};
    int prc = pawnxfs_poke(pawn_path, pawn_path, patches, npatches, err, sizeof(err));
    if (prc != 0) {
        log_line("writeback: slot %d (%03d:%s) pawnxfs_poke rc=%d (%s) — '%s'",
                 slot, level, stem, prc, err, pawn_path);
        return;
    }
    int equipped = 0;
    for (int k = 0; k < WB_GEAR_COUNT; k++) if (info->gear[k].mItemNo != -1) equipped++;
    int known = 0;
    for (int i = 0; i < WB_STUDY_COUNT; i++)
        if (info->study_flag[i] || info->local_study_flag[i]) known++;
    log_line("writeback: slot %d (%03d:%s) gear=%d study=%d -> '%s'",
             slot, level, stem, equipped, known, pawn_path);

    /* Snapshot the hired pawn's full XML region (vocation, skills, augments,
     * inclinations — none of which the .pawn archive captures) into the .xml
     * sidecar. */
    if (info->region_xml && info->region_xml_len > 0) {
        char origin[64];
        _snprintf(origin, sizeof(origin), "writeback slot %d", slot);
        write_pawn_xml_sidecar(info->creator_name, info->region_xml,
                               info->region_xml_len, origin);
    }
}

/* ISteamRemoteStorage::FileWrite (slot 0) ---------------------------------- *
 * bool FileWrite(const char *pchFile, const void *pvData, int cubData)
 * Two roles:
 *   - 6a: capture pchFile=="0" buffer for archive_rest (avoids depending on
 *         Steam's userdata-path layout).
 *   - 6b: on pchFile=="DDDA.sav", run hired-pawn writeback + main-pawn .xml
 *         sidecar.  Always forwarded *first* so the on-disk save is
 *         independent of whatever we do afterwards. */
typedef BOOL (__thiscall *fn_FileWrite)(void *self, const char *pchFile,
                                        const void *pvData, int cubData);
static fn_FileWrite g_orig_FileWrite = NULL;

static BOOL __thiscall hook_FileWrite(void *self, const char *pchFile,
                                      const void *pvData, int cubData)
{
    /* 6a — capture inn-rest pawn blob in RAM. */
    if (pchFile && strcmp(pchFile, "0") == 0 && pvData && cubData > 0) {
        if (g_pending_pawn_blob) {
            free(g_pending_pawn_blob);
            g_pending_pawn_blob = NULL;
            g_pending_pawn_size = 0;
        }
        g_pending_pawn_blob = (BYTE*)malloc((size_t)cubData);
        if (g_pending_pawn_blob) {
            memcpy(g_pending_pawn_blob, pvData, (size_t)cubData);
            g_pending_pawn_size = cubData;
            log_line("FileWrite('0') -> captured %d bytes for archive", cubData);
        } else {
            log_line("FileWrite('0'): malloc(%d) failed; archive will be skipped", cubData);
            g_pending_pawn_size = 0;
        }
    }

    /* Forward first so the game's own save lands on disk regardless of what
     * we do below. */
    BOOL r = g_orig_FileWrite(self, pchFile, pvData, cubData);

    /* 6b — DDDA.sav writeback path. */
    if (pchFile && strcmp(pchFile, "DDDA.sav") == 0 && pvData && cubData > 32) {
        log_line("FileWrite('DDDA.sav', %d bytes) -> %s",
                 cubData, r ? "true" : "false");
        if (g_enable_updates) {
            struct pawnsave_hired_info info[2];
            int rc = pawnsave_read_hired(pvData, cubData, info);
            if (rc != 0) {
                log_line("writeback: pawnsave_read_hired rc=%d — skipping both slots", rc);
            } else {
                for (int s = 0; s < 2; s++) {
                    /* Skip dead/empty slots: the save XML still carries a
                     * cSAVE_DATA_CMC block tagged with our stem, but its
                     * contents are the pawn's last checkpoint state, not the
                     * latest in-party state.  Writing that back would clobber
                     * the archive with checkpoint data.  read_hired_alive
                     * reads the per-cCmc runtime flag (cleared in real time
                     * on death/release), so info-slot s ↔ mCmc[s+1]. */
                    if (info[s].present && !read_hired_alive(s + 1)) {
                        log_line("writeback: slot %d (mArisenName='%s') "
                                 "not alive — skipping", s, info[s].creator_name);
                    } else {
                        writeback_to_archive(s, &info[s]);
                    }
                    if (info[s].region_xml) {
                        free(info[s].region_xml);
                        info[s].region_xml = NULL;
                        info[s].region_xml_len = 0;
                    }
                }
            }
        } else {
            log_line("writeback: enable_updates=0, skipping hired-pawn writeback");
        }

        /* Main-pawn snapshot for restore_pawn (6e).  Independent of
         * enable_updates — this is about the player's own archive, not
         * hired-pawn writeback.  Gated on g_archive_xml_pending so it only
         * fires on the inn-rest auto-save right after archive_rest, never on
         * subsequent quicksaves / area-transition saves where mCmc[0] has
         * drifted from the archive snapshot.  One-shot — drains immediately
         * regardless of whether the extract / write succeeded, so a failed
         * extract doesn't leak through to the next save's mCmc[0]. */
        if (g_enable_exports && g_archive_xml_pending && g_last_archive_stem_full[0]) {
            uint8_t *mp_xml = NULL;
            size_t   mp_len = 0;
            int erc = pawnsave_extract_main_pawn_xml(pvData, cubData,
                                                     &mp_xml, &mp_len);
            if (erc == 0) {
                write_pawn_xml_sidecar(g_last_archive_stem_full,
                                       mp_xml, mp_len, "main-pawn snapshot");
                free(mp_xml);
            } else {
                log_line("pawnxml: main-pawn extract rc=%d — no sidecar written", erc);
            }
            g_archive_xml_pending = 0;
        }
    }

    return r;
}

/* ISteamRemoteStorage::FileShare (slot 4) ---------------------------------- *
 * Arm g_rest_pending on FileShare('0') — start of an inn-rest cycle.  Pure
 * observation; forwarded unchanged so the player's own pawn upload lands
 * with Capcom (HANDOFF §6.5). */
typedef SteamAPICall_t (__thiscall *fn_FileShare)(void *self, const char *pchFile);
static fn_FileShare g_orig_FileShare = NULL;

static SteamAPICall_t __thiscall hook_FileShare(void *self, const char *pchFile)
{
    if (pchFile && strcmp(pchFile, "0") == 0) {
        g_rest_pending = 1;
        log_line("FileShare('0') -> rest cycle armed");
    }
    SteamAPICall_t r = g_orig_FileShare(self, pchFile);
    log_line("FileShare('%s') -> call=0x%016llx",
             pchFile ? pchFile : "(null)", (unsigned long long)r);
    return r;
}

/* ============================================================================
 * Step 6c — Fake-SteamID scrubber (verbatim port of pawndb's wide-scan)
 *
 * When DDDA hires a mod-served pawn, it copies the leaderboard entry's
 * mod-synthetic SteamID into the hired pawn's mOnlinePawnInfo.mNetUniqueId.mData.
 * That value cascades into mNetRewardStock and on the next inn rest
 * short-circuits the pawn-upload path (no FileShare('1'), no cDetails=18
 * cluster, no archive opportunity for 6a).  Zeroing the fake on every tick
 * keeps the cascade clean — mPawnHistory, mNetRewardStock, and save-time
 * secondary copies are all populated FROM mCmc[hired], so clean primary →
 * clean everywhere else.
 *
 * Detection: walk a 1 MB window from *pBase, match candidates whose
 *   [0..3]   = 0x00000002 LE   (MtNetUniqueId type tag = Steam)
 *   [8..11]  = 0x01100001 LE   (universe/type/instance)
 *   [7] MSB  = set             (mod-synthetic; real account_ids ~ 0x30M-0x40M)
 *   [12..63] = 0x00            (Steam IDs only use bytes 0..11)
 * and zero them (plus the u32 mDataLength four bytes earlier).
 * ============================================================================ */

#define NETUID_WIDE_SCAN_SIZE  0x00100000u   /* 1 MB — covers full char block */
#define NETUID_SEEN_MAX        8
#define DIAG_SCAN_INTERVAL     3600u         /* ~60s at 60 Hz */
#define DIAG_SCAN_MAX_COUNT    10            /* stop after ~10 min */

/* The pawn-array root.  `*g_pawn_base_var` is NULL until a save is loaded. */
static BYTE **g_pawn_base_var = NULL;

static BYTE *g_netuid_seen[NETUID_SEEN_MAX] = {0};
static int   g_netuid_seen_count            = 0;
static BYTE *g_netuid_seen_base             = NULL;
static int      g_scrub_seen_pbase = 0;
static uint32_t g_scrub_ticks      = 0;
static uint32_t __attribute__((unused)) g_scrub_diag_scans = 0;

static void discover_pawn_base(void)
{
    HMODULE exe = GetModuleHandleA(NULL);       /* DDDA.exe */
    if (!exe) { log_line("discover_pawn_base: no exe handle"); return; }

    BYTE *exe_base = (BYTE*)exe;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)exe_base;
    PIMAGE_NT_HEADERS nt  = (PIMAGE_NT_HEADERS)(exe_base + dos->e_lfanew);
    BYTE *code_base = exe_base + nt->OptionalHeader.BaseOfCode;
    BYTE *code_end  = code_base + nt->OptionalHeader.SizeOfCode;

    /* MOV EDX, [imm32] ; XOR EBX, EBX ; MOV EDI, EAX — known game-code site
     * whose imm32 is the address of the *variable* holding the actual base. */
    static const unsigned char sig[]  = { 0x8B, 0x15, 0xCC, 0xCC, 0xCC, 0xCC, 0x33, 0xDB, 0x8B, 0xF8 };
    static const unsigned char mask[] = {    1,    1,    0,    0,    0,    0,    1,    1,    1,    1 };
    const size_t len = sizeof(sig);
    if ((size_t)(code_end - code_base) < len) return;

    for (BYTE *p = code_base; p <= code_end - len; p++) {
        int ok = 1;
        for (size_t i = 0; i < len; i++) {
            if (mask[i] && p[i] != sig[i]) { ok = 0; break; }
        }
        if (ok) {
            g_pawn_base_var = *(BYTE***)(p + 2);
            log_line("discover_pawn_base: pBase variable at %p (code site %p)",
                     (void*)g_pawn_base_var, (void*)p);
            return;
        }
    }
    log_line("discover_pawn_base: signature not found in .text");
}

static int netuid_pattern_match(const BYTE *p)
{
    if (*(const uint32_t*)p != 0x00000002u) return 0;
    if (*(const uint32_t*)(p + 8) != 0x01100001u) return 0;
    if ((p[7] & 0x80) == 0) return 0;
    for (int i = 12; i < 64; i++) if (p[i]) return 0;
    return 1;
}

static void netuid_zero_at(BYTE *p)
{
    memset(p, 0, 64);
    *(volatile uint32_t*)(p - 4) = 0;
}

static int netuid_seen_contains(BYTE *p)
{
    for (int i = 0; i < g_netuid_seen_count; i++) if (g_netuid_seen[i] == p) return 1;
    return 0;
}
static void netuid_seen_add(BYTE *p)
{
    if (g_netuid_seen_count < NETUID_SEEN_MAX) g_netuid_seen[g_netuid_seen_count++] = p;
}

/* Diagnostic snapshot: one log line per MtNetUniqueId candidate (real or
 * fake) with its offset-from-pBase.  Capped both per-scan and total. */
#define DIAG_MAX_HITS 16
static void diag_wide_scan(BYTE *base) __attribute__((unused));
static void diag_wide_scan(BYTE *base)
{
    uint32_t hits = 0;
    log_line("diag-scan: begin walk [%p .. %p]",
             (void*)base, (void*)(base + NETUID_WIDE_SCAN_SIZE));
    for (uint32_t off = 0; off + 64 <= NETUID_WIDE_SCAN_SIZE; off += 4) {
        const BYTE *p = base + off;
        if (*(const uint32_t*)p != 0x00000002u) continue;
        if (*(const uint32_t*)(p + 8) != 0x01100001u) continue;
        const char *kind = (p[7] & 0x80) ? "FAKE" : "real";
        log_line("  candidate @ +0x%06X  byte[4..7]=%02X %02X %02X %02X  kind=%s",
                 (unsigned)off, p[4], p[5], p[6], p[7], kind);
        if (++hits >= DIAG_MAX_HITS) {
            log_line("  (hit cap %u reached, stopping scan)", DIAG_MAX_HITS);
            break;
        }
    }
    log_line("diag-scan: done, %u hit(s)", hits);
}

/* Walk the 1 MB window and zero every fake mData hit.  Real user IDs
 * (byte[7] MSB clear) don't match netuid_pattern_match and stay untouched. */
static void netuid_scrub_wide(BYTE *base)
{
    if (base != g_netuid_seen_base) {
        g_netuid_seen_base  = base;
        g_netuid_seen_count = 0;
    }
    for (uint32_t off = 0; off + 64 <= NETUID_WIDE_SCAN_SIZE; off += 4) {
        BYTE *p = base + off;
        if (*(const uint32_t*)p != 0x00000002u) continue;
        if (!netuid_pattern_match(p)) continue;
        if (!netuid_seen_contains(p)) {
            log_line("netuid: fake seen at %p (+0x%06X), zeroing this and every subsequent tick",
                     (void*)p, (unsigned)off);
            netuid_seen_add(p);
        }
        netuid_zero_at(p);
    }
}

static void netuid_scrub(void)
{
    g_scrub_ticks++;
    if (!g_pawn_base_var) return;
    BYTE *base = *g_pawn_base_var;
    if (!base) return;                          /* no save loaded yet */

    if (!g_scrub_seen_pbase) {
        g_scrub_seen_pbase = 1;
        log_line("scrubber: first tick with *pBase set — pBase=%p *pBase=%p (tick #%u)",
                 (void*)g_pawn_base_var, (void*)base, g_scrub_ticks);
    }

    /* diag_wide_scan(base) was the diagnostic that originally established
     * the +0x0AAAB0 / +0x0AC110 hired-pawn netuid offsets (10 timed scans
     * per session, dumping ~16 MtNetUniqueId candidates each).  Now that
     * the offsets are confirmed, the scan is gated off; re-enable the
     * `if` block above this comment for future RE work. */

    netuid_scrub_wide(base);
}

/* ISteamUser::BLoggedOn — DDDA polls this from its main loop at ~60 Hz
 * regardless of whether anyone reads the result, which makes it our cheapest
 * tick source for the scrubber.  Intentionally not logged on the common path
 * (would be 60 lines/sec); the scrubber's own first-sighting policy provides
 * the signal we care about. */
typedef int (__thiscall *fn_BLoggedOn)(void *self);
static fn_BLoggedOn g_orig_BLoggedOn = NULL;

static int __thiscall hook_BLoggedOn(void *self)
{
    netuid_scrub();
    return g_orig_BLoggedOn(self);
}

static int find_fake_pawn_by_steamid(CSteamID sid)
{
    for (int i = 0; i < g_pawn_count; i++) {
        if (g_pawns[i].steamid == sid) return i;
    }
    return -1;
}

/* Returns the fake_pawn index whose ugc_main or ugc_preview matches `h`.
 * Sets *is_preview_out=1 if it was the preview channel.  -1 if not found. */
static int find_fake_pawn_by_ugc(UGCHandle_t h, int *is_preview_out)
{
    for (int i = 0; i < g_pawn_count; i++) {
        if (g_pawns[i].ugc_main == h)    { if (is_preview_out) *is_preview_out = 0; return i; }
        if (g_pawns[i].ugc_preview == h) { if (is_preview_out) *is_preview_out = 1; return i; }
    }
    return -1;
}

/* DDDA stores low 32 bits of UGCHandle byte-swapped at det[0]; high 32 bits
 * at det[1] (verified by pawndb's ugc_from_encoded_details / on-disk format). */
static void encode_ugc_to_details(UGCHandle_t ugc, uint32_t *out_d0, uint32_t *out_d1)
{
    uint32_t lo = (uint32_t)(ugc & 0xffffffffu);
    uint32_t hi = (uint32_t)(ugc >> 32);
    *out_d0 = __builtin_bswap32(lo);
    *out_d1 = hi;
}

/* Synthesise a LeaderboardScoresDownloaded_t for an archive board.  We
 * pass the REAL hLB through unchanged in m_hSteamLeaderboard so the game's
 * handler doesn't see a fake handle (synth-LBFR caused return-address
 * corruption in 0.5.x).  Only the hLBE is synthetic — the game uses it
 * as a key when calling GetDownloadedLeaderboardEntry, where we serve
 * per-pawn entry data from g_pawns[]. */
static SteamAPICall_t synth_download_entries_for(SteamLeaderboard_t real_hLB,
                                                 int board_num, int archive_level,
                                                 int range_start)
{
    /* Read mArisenName from each hired slot, gated on the cCmc alive flag
     * so a stale stem left over from a dead/released pawn doesn't keep
     * filtering its archive out of the rift forever.  If the slot is
     * alive AND the name parses as a pawnlib stamp, derive its synthetic
     * SteamID and use that as the filter key.
     *
     * Without this filter, the rift would re-offer a pawn that's already
     * in the party — hiring it a second time would put two instances on
     * one archive, and at save time both writebacks would race on the
     * same .pawn file (last write wins, slot 0's drift silently lost). */
    CSteamID hired_sids[2] = {0, 0};
    for (int s = 1; s <= 2; s++) {
        if (!read_hired_alive(s)) continue;
        char stem_full[16];
        if (!read_hired_arisen(s, stem_full, sizeof(stem_full))) continue;
        int level;
        char hex[16];
        if (!parse_arisen_stem(stem_full, &level, hex, sizeof(hex))) continue;
        UGCHandle_t um, up;
        derive_fake_identity(hex, &hired_sids[s - 1], &um, &up);
    }

    /* Pick all g_pawns where level matches; cap at g_max_search_results
     * (ini knob, default 50, clamped to MAX_ENTRIES_PER_LBE).
     * g_pawns is mtime-sorted descending, so first-K is most-recent-first. */
    int picked[MAX_ENTRIES_PER_LBE];
    int entry_count = 0;
    int filtered = 0;
    EnterCriticalSection(&g_disp_lock);
    for (int i = 0; i < g_pawn_count && entry_count < g_max_search_results; i++) {
        if (g_pawns[i].level != archive_level) continue;
        if ((hired_sids[0] && g_pawns[i].steamid == hired_sids[0]) ||
            (hired_sids[1] && g_pawns[i].steamid == hired_sids[1])) {
            filtered++;
            continue;
        }
        picked[entry_count++] = i;
    }
    SteamLeaderboardEntries_t hLBE = g_next_fake_lbe++;
    LeaveCriticalSection(&g_disp_lock);

    register_fake_lbe(hLBE, board_num, archive_level, entry_count, range_start, picked);

    LeaderboardScoresDownloaded_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.m_hSteamLeaderboard         = real_hLB;
    payload.m_hSteamLeaderboardEntries  = hLBE;
    payload.m_cEntryCount               = entry_count;

    SteamAPICall_t hCall = alloc_fake_call_id();
    queue_pending(hCall, LBSD_K_ICALLBACK, &payload, sizeof(payload), 0);

    log_line("synth DLE board=%d level=%d (real hLB=0x%llx) -> hLBE=0x%llx count=%d (filtered=%d) call=0x%llx",
             board_num, archive_level, (unsigned long long)real_hLB,
             (unsigned long long)hLBE, entry_count, filtered, (unsigned long long)hCall);
    return hCall;
}

/* Summon-side counterpart: DLEForUsers returns a 1-entry LBSD whose
 * referenced pawn is `pawn_idx`.  The game will then call GDLE with
 * cDetailsMax=2 and we encode that pawn's UGC into det[0..1]. */
static SteamAPICall_t synth_download_entries_for_user(SteamLeaderboard_t real_hLB,
                                                       int board_num, int pawn_idx)
{
    int picked[MAX_ENTRIES_PER_LBE] = { pawn_idx };
    EnterCriticalSection(&g_disp_lock);
    SteamLeaderboardEntries_t hLBE = g_next_fake_lbe++;
    LeaveCriticalSection(&g_disp_lock);

    /* archive_level / range_start are nominal here — m_nGlobalRank=1 is
     * conventional for a "single-user lookup" response. */
    register_fake_lbe(hLBE, board_num, g_pawns[pawn_idx].level, 1, 1, picked);

    LeaderboardScoresDownloaded_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.m_hSteamLeaderboard         = real_hLB;
    payload.m_hSteamLeaderboardEntries  = hLBE;
    payload.m_cEntryCount               = 1;

    SteamAPICall_t hCall = alloc_fake_call_id();
    queue_pending(hCall, LBSD_K_ICALLBACK, &payload, sizeof(payload), 0);

    log_line("synth DLEFU board=%d pawn_idx=%d (real hLB=0x%llx) -> hLBE=0x%llx call=0x%llx",
             board_num, pawn_idx, (unsigned long long)real_hLB,
             (unsigned long long)hLBE, (unsigned long long)hCall);
    return hCall;
}

/* Synthesise a RemoteStorageDownloadUGCResult_t for a fake UGC handle.
 * The game pumps RunCallbacks, dispatch_pendings delivers this to the
 * CCallResult registered by the caller, and the game then calls UGCRead
 * which our hook serves from <pawn_path> on disk. */
static SteamAPICall_t synth_ugc_download(int pawn_idx, int is_preview)
{
    const fake_pawn_t *fp = &g_pawns[pawn_idx];
    UGCHandle_t ugc = is_preview ? fp->ugc_preview : fp->ugc_main;

    /* Stat the .pawn file so the dispatched RSDUR carries the real size —
     * pawn blobs are nominally 8KB but we don't assume.  Failures fall
     * through to size=0; the game's UGCRead will then read 0 bytes. */
    int32 size = 0;
    HANDLE hf = CreateFileA(fp->pawn_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(hf, &sz) && sz.QuadPart < INT32_MAX) size = (int32)sz.QuadPart;
        CloseHandle(hf);
    } else {
        log_line("synth UGCDownload: stat('%s') failed (errno=%d); size=0",
                 fp->pawn_path, GetLastError());
    }

    RemoteStorageDownloadUGCResult_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.m_eResult        = 1;                /* k_EResultOK */
    payload.m_hFile          = ugc;
    payload.m_nAppID         = 367500;           /* DDDA */
    payload.m_nSizeInBytes   = size;
    /* m_pchFileName: pawndb leaves this empty; the game reads UGC by handle. */
    payload.m_ulSteamIDOwner = (uint64_t)fp->steamid;

    SteamAPICall_t hCall = alloc_fake_call_id();
    queue_pending(hCall, RSDUR_K_ICALLBACK, &payload, sizeof(payload), 0);

    log_line("synth UGCDownload pawn=%d (%s) ugc=0x%llx -> size=%d call=0x%llx",
             pawn_idx, is_preview ? "preview" : "main",
             (unsigned long long)ugc, size, (unsigned long long)hCall);
    return hCall;
}

/* Fill pEntry / pDetails for index `idx` against a fake hLBE.  Pulls the
 * pawn record out of g_pawns[] by way of lbe->pawn_idx[], so each entry
 * gets the per-pawn steamID + .meta-loaded details.  Returns 1 on
 * success, 0 if idx is out of range or pawn slot is invalid. */
static int synth_get_entry(const fake_lbe_t *lbe, int idx,
                            LeaderboardEntry_t *pEntry,
                            int32 *pDetails, int cDetailsMax)
{
    if (idx < 0 || idx >= lbe->entry_count) return 0;
    if (!pEntry) return 0;

    int pi = lbe->pawn_idx[idx];
    if (pi < 0 || pi >= g_pawn_count) return 0;
    const fake_pawn_t *fp = &g_pawns[pi];

    pEntry->m_steamIDUser = fp->steamid;
    pEntry->m_nGlobalRank = lbe->range_start + idx;
    pEntry->m_nScore      = 10000 - idx;
    pEntry->m_hUGC        = 0;

    /* Two response shapes share this hook:
     *   - browse  (DLE on leader_<cohort>, cDetailsMax >= 18) -> 18-int .meta
     *   - summon  (DLEForUsers,            cDetailsMax == 2)  -> encoded UGC
     * Pawndb gates on cDetailsMax==2 the same way; the game decides which
     * mode by what it passes in. */
    if (cDetailsMax >= 18) {
        pEntry->m_cDetails = 18;
        if (pDetails) {
            int n = 18 < cDetailsMax ? 18 : cDetailsMax;
            memcpy(pDetails, fp->details, n * sizeof(int32));
        }
    } else {
        pEntry->m_cDetails = 2;
        if (pDetails && cDetailsMax >= 2) {
            uint32_t d0, d1;
            encode_ugc_to_details(fp->ugc_main, &d0, &d1);
            ((uint32_t*)pDetails)[0] = d0;
            ((uint32_t*)pDetails)[1] = d1;
        }
    }
    return 1;
}

/* ============================================================================
 * Hooks - passive (Step 3 + extended observation)
 *
 * All hooks log and forward.  No behaviour change.  Goal is to capture
 * a complete trace of DDDA's leaderboard read surface so we can pin
 * down the wire protocol before designing Step 4 / 5.
 * ============================================================================ */

/* Function-pointer typedef used to resolve GetLeaderboardName synchronously
 * from inside other hooks - cheaper than re-walking the vtable each call,
 * and lets us print a name next to every handle. */
typedef const char* (__thiscall *fn_GetLeaderboardName)(void *self, SteamLeaderboard_t hLB);
static fn_GetLeaderboardName g_GetLeaderboardName = NULL;
static void *g_steam_userstats = NULL;

static const char *resolve_lb_name(SteamLeaderboard_t hLB)
{
    if (!g_GetLeaderboardName || !g_steam_userstats || hLB == 0) return "?";
    const char *name = g_GetLeaderboardName(g_steam_userstats, hLB);
    return name ? name : "?";
}

/* FindLeaderboard ---------------------------------------------------------- */
typedef SteamAPICall_t (__thiscall *fn_FindLeaderboard)(void *self, const char *name);
static fn_FindLeaderboard g_orig_FindLeaderboard = NULL;

static SteamAPICall_t __thiscall hook_FindLeaderboard(void *self, const char *name)
{
    /* Always forward.  Earlier versions intercepted FindLeaderboard for
     * archive boards and synthesised an LBFR with a fake hLB; this caused
     * a reliable crash in the game's CCallResult handler ~1 ms after Run2
     * returned (return-address corruption on dispatch_pendings's stack
     * frame from a wide write inside the user handler).  Synthesis moved
     * to DLE/DLEForUsers, where the game only ever sees fake hLBE values
     * (which it just passes back to GetDownloadedLeaderboardEntry).  This
     * matches HANDOFF §6.5 — read path starts at DLE, not FindLeaderboard. */
    SteamAPICall_t call = g_orig_FindLeaderboard(self, name);
    log_line("FindLeaderboard('%s') self=%p -> call=0x%016llx",
             name ? name : "(null)", self, (unsigned long long)call);
    return call;
}

/* FindOrCreateLeaderboard -------------------------------------------------- */
typedef SteamAPICall_t (__thiscall *fn_FindOrCreateLeaderboard)(
    void *self, const char *name, int eSortMethod, int eDisplayType);
static fn_FindOrCreateLeaderboard g_orig_FindOrCreateLeaderboard = NULL;

static SteamAPICall_t __thiscall hook_FindOrCreateLeaderboard(
    void *self, const char *name, int eSort, int eDisplay)
{
    SteamAPICall_t call = g_orig_FindOrCreateLeaderboard(self, name, eSort, eDisplay);
    log_line("FindOrCreateLeaderboard('%s', sort=%d, display=%d) -> call=0x%016llx",
             name ? name : "(null)", eSort, eDisplay, (unsigned long long)call);
    return call;
}

/* Synchronous accessors slots 25-27.  Now that FindLeaderboard isn't
 * intercepted, all hLBs the game sees are real and we just forward.  The
 * hook still exists so we have a place to log calls (helps see the read
 * sequence around the rift entry). */
typedef int (__thiscall *fn_GetLeaderboardEntryCount)(
    void *self, SteamLeaderboard_t hLB);
static fn_GetLeaderboardEntryCount g_orig_GetLeaderboardEntryCount = NULL;

static int __thiscall hook_GetLeaderboardEntryCount(
    void *self, SteamLeaderboard_t hLB)
{
    int n = g_orig_GetLeaderboardEntryCount(self, hLB);
    log_line("GetLeaderboardEntryCount(0x%llx '%s') -> %d",
             (unsigned long long)hLB, resolve_lb_name(hLB), n);
    return n;
}

typedef int (__thiscall *fn_GetLeaderboardSortMethod)(
    void *self, SteamLeaderboard_t hLB);
static fn_GetLeaderboardSortMethod g_orig_GetLeaderboardSortMethod = NULL;

static int __thiscall hook_GetLeaderboardSortMethod(
    void *self, SteamLeaderboard_t hLB)
{
    int v = g_orig_GetLeaderboardSortMethod(self, hLB);
    log_line("GetLeaderboardSortMethod(0x%llx '%s') -> %d",
             (unsigned long long)hLB, resolve_lb_name(hLB), v);
    return v;
}

typedef int (__thiscall *fn_GetLeaderboardDisplayType)(
    void *self, SteamLeaderboard_t hLB);
static fn_GetLeaderboardDisplayType g_orig_GetLeaderboardDisplayType = NULL;

static int __thiscall hook_GetLeaderboardDisplayType(
    void *self, SteamLeaderboard_t hLB)
{
    int v = g_orig_GetLeaderboardDisplayType(self, hLB);
    log_line("GetLeaderboardDisplayType(0x%llx '%s') -> %d",
             (unsigned long long)hLB, resolve_lb_name(hLB), v);
    return v;
}

/* DownloadLeaderboardEntries ----------------------------------------------- */
typedef SteamAPICall_t (__thiscall *fn_DownloadLeaderboardEntries)(
    void *self, SteamLeaderboard_t hLB,
    ELeaderboardDataRequest eRequest, int nRangeStart, int nRangeEnd);
static fn_DownloadLeaderboardEntries g_orig_DownloadLeaderboardEntries = NULL;

static SteamAPICall_t __thiscall hook_DownloadLeaderboardEntries(
    void *self, SteamLeaderboard_t hLB,
    ELeaderboardDataRequest eRequest, int nRangeStart, int nRangeEnd)
{
    /* Detect archive-board hLBs by asking real Steam for the name.  Real
     * Steam returns the canonical "leader_<N>" string for every hLB it
     * knows, so name-based detection is robust against the unpredictable
     * value of the handle itself. */
    const char *name = g_GetLeaderboardName ? g_GetLeaderboardName(self, hLB) : NULL;
    int board_num;
    if (name && parse_leader_name(name, &board_num) && !is_special_board(board_num)) {
        int level = board_to_archive_level(board_num);
        if (level >= 0) {
            log_line("DLE INTERCEPT hLB=0x%llx ('%s' board=%d level=%d) req=%d range=[%d..%d]",
                     (unsigned long long)hLB, name, board_num, level,
                     eRequest, nRangeStart, nRangeEnd);
            return synth_download_entries_for(hLB, board_num, level, nRangeStart);
        }
    }
    SteamAPICall_t call = g_orig_DownloadLeaderboardEntries(
        self, hLB, eRequest, nRangeStart, nRangeEnd);
    log_line("DownloadLeaderboardEntries(hLB=0x%llx '%s', req=%d, range=[%d..%d]) -> call=0x%016llx",
             (unsigned long long)hLB, name ? name : "?",
             eRequest, nRangeStart, nRangeEnd, (unsigned long long)call);
    return call;
}

/* DownloadLeaderboardEntriesForUsers --------------------------------------- */
typedef SteamAPICall_t (__thiscall *fn_DownloadLeaderboardEntriesForUsers)(
    void *self, SteamLeaderboard_t hLB, CSteamID *prgUsers, int cUsers);
static fn_DownloadLeaderboardEntriesForUsers g_orig_DownloadLeaderboardEntriesForUsers = NULL;

static SteamAPICall_t __thiscall hook_DownloadLeaderboardEntriesForUsers(
    void *self, SteamLeaderboard_t hLB, CSteamID *prgUsers, int cUsers)
{
    /* Step 5c: when the game asks "what's pawn X up to", it hands us a
     * single fake steamID we minted in 5b.  Synthesise a 1-entry LBSD
     * pointing at that pawn; the game's GDLE call (cDetailsMax=2) gets
     * the encoded UGC and proceeds to UGCDownload. */
    if (cUsers == 1 && prgUsers) {
        int pawn_idx = find_fake_pawn_by_steamid(prgUsers[0]);
        if (pawn_idx >= 0) {
            const char *name = resolve_lb_name(hLB);
            int board_num = 0;
            parse_leader_name(name, &board_num);
            log_line("DLEFU INTERCEPT hLB=0x%llx '%s' user=0x%016llx -> pawn_idx=%d",
                     (unsigned long long)hLB, name,
                     (unsigned long long)prgUsers[0], pawn_idx);
            return synth_download_entries_for_user(hLB, board_num, pawn_idx);
        }
    }

    SteamAPICall_t call = g_orig_DownloadLeaderboardEntriesForUsers(
        self, hLB, prgUsers, cUsers);

    char users[1024]; users[0] = 0;
    int off = 0;
    for (int i = 0; i < cUsers && off < (int)sizeof(users) - 32; i++) {
        off += _snprintf(users + off, sizeof(users) - off,
                         "%s0x%016llx", i ? "," : "",
                         (unsigned long long)prgUsers[i]);
    }
    log_line("DownloadLeaderboardEntriesForUsers(hLB=0x%llx '%s', cUsers=%d, [%s]) -> call=0x%016llx",
             (unsigned long long)hLB, resolve_lb_name(hLB),
             cUsers, users, (unsigned long long)call);
    return call;
}

/* GetDownloadedLeaderboardEntry -------------------------------------------- */
typedef int (__thiscall *fn_GetDownloadedLeaderboardEntry)(
    void *self, SteamLeaderboardEntries_t hLBE, int index,
    LeaderboardEntry_t *pEntry, int32 *pDetails, int cDetailsMax);
static fn_GetDownloadedLeaderboardEntry g_orig_GetDownloadedLeaderboardEntry = NULL;

static int __thiscall hook_GetDownloadedLeaderboardEntry(
    void *self, SteamLeaderboardEntries_t hLBE, int index,
    LeaderboardEntry_t *pEntry, int32 *pDetails, int cDetailsMax)
{
    fake_lbe_t lbe;
    if (find_fake_lbe(hLBE, &lbe)) {
        int ok = synth_get_entry(&lbe, index, pEntry, pDetails, cDetailsMax);
        log_line("GDLE INTERCEPT hLBE=0x%llx idx=%d (board=%d level=%d count=%d) -> %s",
                 (unsigned long long)hLBE, index,
                 lbe.board_num, lbe.archive_level, lbe.entry_count,
                 ok ? "ok" : "out-of-range");
        return ok;
    }
    int ok = g_orig_GetDownloadedLeaderboardEntry(
        self, hLBE, index, pEntry, pDetails, cDetailsMax);

    if (ok && pEntry) {
        /* Dump first few details in hex so we can recognise the
         * 18-int packed metadata blob (cDetails=18) vs the 2-int
         * upload-handle blob (cDetails=2). */
        char dbuf[256]; dbuf[0] = 0;
        int doff = 0;
        int n = pDetails ? pEntry->m_cDetails : 0;
        if (n > cDetailsMax) n = cDetailsMax;
        for (int i = 0; i < n && doff < (int)sizeof(dbuf) - 16; i++) {
            doff += _snprintf(dbuf + doff, sizeof(dbuf) - doff,
                              "%s%d", i ? "," : "", (int)pDetails[i]);
        }
        log_line("GetDownloadedLeaderboardEntry(hLBE=0x%llx, idx=%d) -> ok rank=%d score=%d steamID=0x%016llx hUGC=0x%016llx cDetails=%d [%s]",
                 (unsigned long long)hLBE, index,
                 pEntry->m_nGlobalRank, pEntry->m_nScore,
                 (unsigned long long)pEntry->m_steamIDUser,
                 (unsigned long long)pEntry->m_hUGC,
                 pEntry->m_cDetails, dbuf);
    } else {
        log_line("GetDownloadedLeaderboardEntry(hLBE=0x%llx, idx=%d) -> %d (no data)",
                 (unsigned long long)hLBE, index, ok);
    }
    return ok;
}

/* UploadLeaderboardScore --------------------------------------------------- *
 * gbe_fork's logs show the game calls UploadLeaderboardScore on every
 * leaderboard handle ~150 ms after the FindLeaderboard CCallResult fires —
 * uploading the player's own pawn data to each board it cares about.  For
 * REAL handles we forward unchanged (HANDOFF §6.5: the user's own pawn
 * upload at inn rest is normal traffic that should reach Capcom's backend).
 * For our FAKE archive-board handles, real Steam doesn't recognise the hLB
 * and crashes on deref, so we synthesise a "success" LeaderboardScoreUploaded_t
 * and never let the call leave the process.
 */
typedef SteamAPICall_t (__thiscall *fn_UploadLeaderboardScore)(
    void *self, SteamLeaderboard_t hLB, int eUploadMethod, int32 nScore,
    const int32 *pDetails, int cDetailsCount);
static fn_UploadLeaderboardScore g_orig_UploadLeaderboardScore = NULL;

static SteamAPICall_t __thiscall hook_UploadLeaderboardScore(
    void *self, SteamLeaderboard_t hLB, int eUploadMethod, int32 nScore,
    const int32 *pDetails, int cDetailsCount)
{
    /* Always forward — the user's own pawn upload at inn rest is normal
     * traffic Capcom expects (HANDOFF §6.5).  All hLBs are real now that
     * FindLeaderboard isn't intercepted, so there's nothing to defend
     * against. */
    SteamAPICall_t call = g_orig_UploadLeaderboardScore(
        self, hLB, eUploadMethod, nScore, pDetails, cDetailsCount);
    log_line("UploadLeaderboardScore(0x%llx '%s', method=%d, score=%d, cDetails=%d) -> call=0x%016llx",
             (unsigned long long)hLB, resolve_lb_name(hLB), eUploadMethod, nScore,
             cDetailsCount, (unsigned long long)call);

    /* Step 6a: first cDetails=18 of an armed rest cycle is the archive
     * trigger.  Subsequent 18-detail uploads in the same cluster (~4-6
     * fire rapid-fire for stat boards) get forwarded but don't re-archive. */
    if (cDetailsCount == 18 && pDetails && g_rest_pending) {
        if (g_enable_exports) {
            archive_rest(pDetails);
        } else {
            log_line("archive_rest: enable_exports=0, skipping pawn export");
        }
        g_rest_pending = 0;
        if (g_pending_pawn_blob) {
            free(g_pending_pawn_blob);
            g_pending_pawn_blob = NULL;
            g_pending_pawn_size = 0;
        }
    }
    return call;
}

/* ISteamRemoteStorage::UGCDownload ----------------------------------------- *
 * Step 5c: when the game asks to download one of our fake UGC handles
 * (the encoded pair stored in cDetails[0..1] from synth_get_entry's
 * summon-mode response), we don't forward to real Steam — real Steam has
 * never heard of the handle.  Instead we synthesise a successful
 * RemoteStorageDownloadUGCResult_t with the .pawn file's size, and remember
 * which pawn the imminent UGCRead should serve. */
typedef SteamAPICall_t (__thiscall *fn_UGCDownload)(void *self, UGCHandle_t h, uint32_t prio);
static fn_UGCDownload g_orig_UGCDownload = NULL;

/* Set by UGCDownload, consumed by UGCRead.  The summon flow is single-
 * threaded with respect to UGC reads (game pumps UGCDownload completion
 * callback, then calls UGCRead) so a single global is enough. */
static int g_pending_pawn_idx   = -1;
static int g_pending_is_preview = 0;

static SteamAPICall_t __thiscall hook_UGCDownload(void *self, UGCHandle_t h, uint32_t prio)
{
    int is_preview = 0;
    int idx = find_fake_pawn_by_ugc(h, &is_preview);
    if (idx >= 0) {
        g_pending_pawn_idx   = idx;
        g_pending_is_preview = is_preview;
        log_line("UGCDownload INTERCEPT pawn=%d (%s) handle=0x%016llx prio=%u",
                 idx, is_preview ? "preview" : "main",
                 (unsigned long long)h, (unsigned)prio);
        return synth_ugc_download(idx, is_preview);
    }
    SteamAPICall_t call = g_orig_UGCDownload(self, h, prio);
    log_line("UGCDownload(handle=0x%016llx, prio=%u) -> call=0x%016llx",
             (unsigned long long)h, (unsigned)prio, (unsigned long long)call);
    return call;
}

/* ISteamRemoteStorage::UGCRead --------------------------------------------- *
 * Synchronous read.  When a fake UGCDownload preceded this call we serve
 * from the cached .pawn file path on disk; otherwise pass through. */
typedef int32 (__thiscall *fn_UGCRead)(void *self, UGCHandle_t h, void *pvData,
                                        int32 cub, uint32_t cOffset, EUGCReadAction act);
static fn_UGCRead g_orig_UGCRead = NULL;

static int32 __thiscall hook_UGCRead(void *self, UGCHandle_t h, void *pvData,
                                      int32 cub, uint32_t cOffset, EUGCReadAction act)
{
    if (g_pending_pawn_idx >= 0 && g_pending_pawn_idx < g_pawn_count) {
        const fake_pawn_t *fp = &g_pawns[g_pending_pawn_idx];
        const char *src = fp->pawn_path;
        /* Preview channel (5c future): pawndb serves the user's remote/1
         * preview from %APPDATA%\GSE Saves\.. — pawnlib doesn't have
         * gbe_fork's preview-on-disk layout, so for now we serve the same
         * .pawn blob.  TODO: add a real preview source if the game depends
         * on it for hover-display. */
        FILE *f = fopen(src, "rb");
        if (!f) {
            log_line("UGCRead: pawn=%d fopen('%s') failed errno=%d; falling through",
                     g_pending_pawn_idx, src, errno);
            g_pending_pawn_idx = -1;
        } else {
            fseek(f, (long)cOffset, SEEK_SET);
            size_t nread = fread(pvData, 1, (size_t)cub, f);
            fclose(f);
            log_line("UGCRead INTERCEPT pawn=%d (%s) cub=%d offset=%u -> %zu bytes from '%s'",
                     g_pending_pawn_idx, g_pending_is_preview ? "preview" : "main",
                     cub, (unsigned)cOffset, nread, src);
            g_pending_pawn_idx = -1;
            return (int32)nread;
        }
    }
    int32 r = g_orig_UGCRead(self, h, pvData, cub, cOffset, act);
    log_line("UGCRead(handle=0x%016llx, cub=%d, offset=%u, act=%d) -> %d",
             (unsigned long long)h, cub, (unsigned)cOffset, act, r);
    return r;
}

/* ============================================================================
 * Vtable dump
 *
 * iface points to an object whose first machine word is the vtable
 * pointer.  Each slot is a function pointer; we just print the raw
 * address.  We dump a generous number of slots so we can identify
 * the SDK version by visual diff against gbe_fork's headers.
 * ============================================================================ */

static void dump_vtable(const char *iface_name, void *iface, int n_slots) __attribute__((unused));
static void dump_vtable(const char *iface_name, void *iface, int n_slots)
{
    if (!iface) {
        log_line("vtable[%s]: <null interface>", iface_name);
        return;
    }
    void **vtbl = *(void***)iface;
    log_line("vtable[%s] iface=%p vtbl=%p:", iface_name, iface, (void*)vtbl);
    for (int i = 0; i < n_slots; i++) {
        log_line("  [%2d] %p", i, vtbl[i]);
    }
}

/* ============================================================================
 * Worker
 *
 * Kicked off from DllMain.  Polls for steam_api.dll, dumps everything
 * we want to see, then exits.  No hooks installed in this step.
 * ============================================================================ */

typedef void* (__cdecl *fn_steam_accessor)(void);

__attribute__((unused))
static const char *PROBE_EXPORTS[] = {
    /* lifecycle / dispatcher */
    "SteamAPI_Init",
    "SteamAPI_InitSafe",
    "SteamAPI_Shutdown",
    "SteamAPI_RunCallbacks",
    "SteamAPI_RegisterCallback",
    "SteamAPI_UnregisterCallback",
    "SteamAPI_RegisterCallResult",
    "SteamAPI_UnregisterCallResult",
    "SteamAPI_GetHSteamUser",
    "SteamAPI_GetHSteamPipe",
    "SteamAPI_IsSteamRunning",
    /* newer-SDK interface factory */
    "SteamInternal_CreateInterface",
    "SteamInternal_ContextInit",
    "SteamInternal_FindOrCreateUserInterface",
    "SteamInternal_FindOrCreateGameServerInterface",
    /* legacy global accessors (what pawndb uses today) */
    "SteamClient",
    "SteamUser",
    "SteamUserStats",
    "SteamRemoteStorage",
    "SteamUtils",
    "SteamApps",
    "SteamFriends",
    "SteamMatchmaking",
    "SteamMatchmakingServers",
    "SteamNetworking",
    "SteamScreenshots",
    "SteamHTTP",
    "SteamUGC",
    "SteamAppList",
    "SteamMusic",
    "SteamMusicRemote",
    "SteamHTMLSurface",
    "SteamInventory",
    "SteamVideo",
    "SteamParentalSettings",
    NULL
};

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;

    /* Wait for steam_api.dll.  pawndb uses 30s (300 * 100ms); copy that. */
    HMODULE steam_api = NULL;
    for (int i = 0; i < 300 && !steam_api; i++) {
        steam_api = GetModuleHandleA("steam_api.dll");
        if (!steam_api) Sleep(100);
    }
    if (!steam_api) {
        log_line("worker: steam_api.dll never loaded after 30s, giving up");
        return 0;
    }

    char sa_path[MAX_PATH] = {0};
    GetModuleFileNameA(steam_api, sa_path, sizeof(sa_path));
    int64_t sa_size = file_size_on_disk(sa_path);

    log_line("steam_api.dll: base=%p path='%s' size=%lld",
             (void*)steam_api, sa_path, (long long)sa_size);

    /* log_version_info(sa_path), log_pe_imports_exports(steam_api), and the
     * probe-exports loop fired at every launch during steps 2/3 to validate
     * the SDK build / surface area; all confirmed and documented in §10 of
     * HANDOFF.md, so the per-launch noise is gone now.  Re-enable here for
     * future SDK forensics. */

    /* Legacy global-accessor pattern (pre-2016 SDK era; what pawndb uses).
     * Confirmed present at step 2 — the SteamInternal_* fallbacks aren't
     * needed for this game. */
    fn_steam_accessor get_userstats     = (fn_steam_accessor)(void*)GetProcAddress(steam_api, "SteamUserStats");
    fn_steam_accessor get_remotestorage = (fn_steam_accessor)(void*)GetProcAddress(steam_api, "SteamRemoteStorage");
    fn_steam_accessor get_user          = (fn_steam_accessor)(void*)GetProcAddress(steam_api, "SteamUser");
    fn_steam_accessor get_utils         = (fn_steam_accessor)(void*)GetProcAddress(steam_api, "SteamUtils");

    void *iface_userstats = NULL, *iface_remotestorage = NULL,
         *iface_user = NULL,      *iface_utils = NULL;

    if (get_userstats || get_remotestorage || get_user || get_utils) {
        /* The accessors return NULL until the game's SteamAPI_Init has
         * succeeded.  Poll for up to 30s. */
        for (int i = 0; i < 300; i++) {
            if (get_userstats     && !iface_userstats)     iface_userstats     = get_userstats();
            if (get_remotestorage && !iface_remotestorage) iface_remotestorage = get_remotestorage();
            if (get_user          && !iface_user)          iface_user          = get_user();
            if (get_utils         && !iface_utils)         iface_utils         = get_utils();

            int got  = (!!iface_userstats) + (!!iface_remotestorage) + (!!iface_user) + (!!iface_utils);
            int want = (!!get_userstats)   + (!!get_remotestorage)   + (!!get_user)   + (!!get_utils);
            if (got == want) break;
            Sleep(100);
        }
    }

    log_line("accessors (post-SteamAPI_Init poll): userstats=%p remotestorage=%p user=%p utils=%p",
             iface_userstats, iface_remotestorage, iface_user, iface_utils);

    /* Vtable dumps live in dump_vtable() but aren't called any more.
     * Re-enable here if a future SDK rev shifts slot indices and we need
     * to re-walk the layout:
     *   if (iface_userstats)     dump_vtable("ISteamUserStats",     iface_userstats,     40);
     *   if (iface_remotestorage) dump_vtable("ISteamRemoteStorage", iface_remotestorage, 40);
     *   if (iface_user)          dump_vtable("ISteamUser",          iface_user,          30);
     *   if (iface_utils)         dump_vtable("ISteamUtils",         iface_utils,         30);
     */

    /* Step 4: install the async callback dispatcher first.  The
     * trampolines forward unchanged to real Steam, so this is a no-op
     * for game-visible behaviour — but it puts the parallel registry
     * under our control for Step 5's synthesis work, and the self-test
     * exercises the vtable Run() invocation path against a fake
     * CCallbackBase fixture so we know the calling-convention plumbing
     * is right before any synthetic completion ever reaches the game. */
    int disp_ok = install_dispatcher_hooks(steam_api);
    if (disp_ok) run_dispatcher_selftest();

    /* Step 3 + extended observation: install passive hooks across the
     * leaderboard read surface so we can characterise DDDA's wire
     * protocol (rift initial roster vs. zone-entry queries vs.
     * per-level search) before designing Steps 4 and 5. */
    if (iface_userstats) {
        g_steam_userstats = iface_userstats;

        /* Cache slot 24 (GetLeaderboardName) without patching — we use the
         * original from our DLE intercept to get a board's canonical name,
         * and the game's calls on real hLBs should reach Steam directly. */
        void **vtbl = *(void***)iface_userstats;
        g_GetLeaderboardName = (fn_GetLeaderboardName)vtbl[IUSERSTATS_GetLeaderboardName];
        log_line("worker: GetLeaderboardName cached at %p (slot %d)",
                 (void*)g_GetLeaderboardName, IUSERSTATS_GetLeaderboardName);

        patch_vtable(iface_userstats, IUSERSTATS_FindLeaderboard,
                     (void*)hook_FindLeaderboard,
                     (void**)&g_orig_FindLeaderboard,
                     "FindLeaderboard");
        patch_vtable(iface_userstats, IUSERSTATS_FindOrCreateLeaderboard,
                     (void*)hook_FindOrCreateLeaderboard,
                     (void**)&g_orig_FindOrCreateLeaderboard,
                     "FindOrCreateLeaderboard");
        /* Slots 25-27: forward-only hooks (logging).  Game calls these
         * on real hLBs; we just record the trace. */
        patch_vtable(iface_userstats, IUSERSTATS_GetLeaderboardEntryCount,
                     (void*)hook_GetLeaderboardEntryCount,
                     (void**)&g_orig_GetLeaderboardEntryCount,
                     "GetLeaderboardEntryCount");
        patch_vtable(iface_userstats, IUSERSTATS_GetLeaderboardSortMethod,
                     (void*)hook_GetLeaderboardSortMethod,
                     (void**)&g_orig_GetLeaderboardSortMethod,
                     "GetLeaderboardSortMethod");
        patch_vtable(iface_userstats, IUSERSTATS_GetLeaderboardDisplayType,
                     (void*)hook_GetLeaderboardDisplayType,
                     (void**)&g_orig_GetLeaderboardDisplayType,
                     "GetLeaderboardDisplayType");
        patch_vtable(iface_userstats, IUSERSTATS_DownloadLeaderboardEntries,
                     (void*)hook_DownloadLeaderboardEntries,
                     (void**)&g_orig_DownloadLeaderboardEntries,
                     "DownloadLeaderboardEntries");
        patch_vtable(iface_userstats, IUSERSTATS_DownloadLeaderboardEntriesForUsers,
                     (void*)hook_DownloadLeaderboardEntriesForUsers,
                     (void**)&g_orig_DownloadLeaderboardEntriesForUsers,
                     "DownloadLeaderboardEntriesForUsers");
        patch_vtable(iface_userstats, IUSERSTATS_GetDownloadedLeaderboardEntry,
                     (void*)hook_GetDownloadedLeaderboardEntry,
                     (void**)&g_orig_GetDownloadedLeaderboardEntry,
                     "GetDownloadedLeaderboardEntry");
        /* Slot 31: UploadLeaderboardScore.  gbe_fork logs (STEAM_LOG_*.log)
         * show the game calls this on every leaderboard handle shortly
         * after the FindLeaderboard CCallResult fires.  Without this hook,
         * real Steam crashes on our fake archive-board hLBs. */
        patch_vtable(iface_userstats, IUSERSTATS_UploadLeaderboardScore,
                     (void*)hook_UploadLeaderboardScore,
                     (void**)&g_orig_UploadLeaderboardScore,
                     "UploadLeaderboardScore");
    } else {
        log_line("worker: ISteamUserStats null, skipping all leaderboard hooks");
    }

    /* ISteamRemoteStorage hooks.
     * Step 5c — summon path:
     *   UGCDownload (slot 21): synthesise RSDUR for fake handles.
     *   UGCRead     (slot 24): serve .pawn bytes from disk for the pending fake.
     * Step 6a — inn-rest archive:
     *   FileWrite   (slot  0): capture pchFile=='0' buffer for archive_rest.
     *   FileShare   (slot  4): arm g_rest_pending on '0'. */
    if (iface_remotestorage) {
        patch_vtable(iface_remotestorage, IREMOTESTORAGE_FileWrite,
                     (void*)hook_FileWrite,
                     (void**)&g_orig_FileWrite,
                     "FileWrite");
        patch_vtable(iface_remotestorage, IREMOTESTORAGE_FileShare,
                     (void*)hook_FileShare,
                     (void**)&g_orig_FileShare,
                     "FileShare");
        patch_vtable(iface_remotestorage, IREMOTESTORAGE_UGCDownload,
                     (void*)hook_UGCDownload,
                     (void**)&g_orig_UGCDownload,
                     "UGCDownload");
        patch_vtable(iface_remotestorage, IREMOTESTORAGE_UGCRead,
                     (void*)hook_UGCRead,
                     (void**)&g_orig_UGCRead,
                     "UGCRead");
    } else {
        log_line("worker: ISteamRemoteStorage null, skipping RemoteStorage hooks");
    }

    /* Step 6c: scrubber tick source.  Patch ISteamUser::BLoggedOn (slot 1)
     * to drive netuid_scrub at ~60 Hz.  Without this, fake steamIDs that
     * cascade into mNetRewardStock break the next inn-rest upload sequence
     * (which is what 6a needs to observe). */
    if (iface_user) {
        patch_vtable(iface_user, IUSER_BLoggedOn,
                     (void*)hook_BLoggedOn,
                     (void**)&g_orig_BLoggedOn,
                     "BLoggedOn");
    } else {
        log_line("worker: ISteamUser null, skipping BLoggedOn / scrubber tick");
    }

    log_line("worker: passive observation hooks installed");

    /* 5b: build the per-pawn registry so DLE intercepts have entries to
     * serve.  Done after the dispatcher + leaderboard hooks are up so any
     * synth response we produce later this session has the latest data. */
    rescan_all_levels();

    /* 6c: locate DDDA's pawn-array root variable by signature scan.  Done
     * after rescan since it's independent and the scrubber tick can begin
     * firing as soon as the BLoggedOn hook is in place above. */
    discover_pawn_base();

    return 0;
}

/* ============================================================================
 * DllMain
 * ============================================================================ */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InitializeCriticalSection(&g_log_lock);
        InitializeCriticalSection(&g_disp_lock);

        char self[MAX_PATH] = {0};
        GetModuleFileNameA(hinstDLL, self, sizeof(self));
        char host[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, host, sizeof(host));

        /* Resolve our own DLL's directory (with trailing backslash) — the
         * 5b folder scan reads <dll_dir>/pawnlib/<NNN>/[stem].pawn from here. */
        DWORD n = GetModuleFileNameA(hinstDLL, g_dll_dir, sizeof(g_dll_dir));
        if (n > 0 && n < sizeof(g_dll_dir)) {
            char *slash = strrchr(g_dll_dir, '\\');
            if (slash) *(slash + 1) = 0;
        } else {
            strcpy(g_dll_dir, ".\\");
        }

        /* 6d: load pawnlib.ini before opening the log so `logging=` controls
         * how the log file is opened (or whether at all).  load_ini's
         * log_line warnings are silently dropped pre-open. */
        char ini_path[MAX_PATH];
        _snprintf(ini_path, sizeof(ini_path), "%spawnlib.ini", g_dll_dir);
        load_ini(ini_path);

        const char *log_mode_label = open_log();

        log_line("=== pawnlib " PAWNLIB_VERSION " loaded (log mode=%s) ===", log_mode_label);
        log_line("self_dll = '%s'", self);
        log_line("host_exe = '%s'", host);
        log_line("dll_dir  = '%s'", g_dll_dir);
        log_line("pid      = %lu", GetCurrentProcessId());
        log_line("config   : max_search_results=%d enable_exports=%d enable_updates=%d logging=%d",
                 g_max_search_results, g_enable_exports, g_enable_updates, (int)g_log_mode);

        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
