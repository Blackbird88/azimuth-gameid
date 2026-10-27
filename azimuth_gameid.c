/*
 * azimuth_gameid.asi - override azimuth.ini "game=" with --gameid=<id>
 *
 *   azimuth.exe --gameid="1a96545b"
 *   azimuth.exe --gameid=1a96545b
 *   azimuth.exe --gameid 1a96545b
 *
 * How it works:
 *   azimuth.exe reads azimuth.ini through kernel32!GetPrivateProfileStringW
 *   (and writes it with WritePrivateProfileStringW). This plugin patches the
 *   exe's import table for those two functions:
 *     - reads of [launcher] game=  -> return the --gameid value instead
 *     - writes of [launcher] game= -> swallowed while an override is active,
 *                                     so your azimuth.ini is never modified
 *   Everything else passes straight through to the real functions.
 *   If --gameid is not on the command line, the plugin does nothing.
 *
 * Build (64-bit only, azimuth.exe is x64):
 *   MinGW-w64:  x86_64-w64-mingw32-gcc -shared -O2 -s -o azimuth_gameid.asi azimuth_gameid.c
 *   MSVC:       cl /LD /O2 azimuth_gameid.c /Fe:azimuth_gameid.asi
 *
 * Install:
 *   azimuth.exe imports DINPUT8.dll, so Ultimate ASI Loader works as a
 *   dinput8.dll proxy. Put dinput8.dll (the loader) next to azimuth.exe and
 *   azimuth_gameid.asi next to it (or in scripts\ or plugins\).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <string.h>

/* ---- what we override ---------------------------------------------------- */
#define ARG_NAME     L"--gameid"
#define INI_FILE     L"azimuth.ini"
#define INI_SECTION  L"launcher"
#define INI_KEY      L"game"

/* Set to 0 if you WANT the exe to be able to save game= back to the ini. */
#define BLOCK_INI_WRITES_OF_KEY 1

/* Set to 1 to get OutputDebugString lines (view with DebugView). */
#define DEBUG_LOG 0

static WCHAR g_gameId[256];

typedef DWORD (WINAPI *GetPPStringW_t)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR);
typedef BOOL  (WINAPI *WritePPStringW_t)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);

static GetPPStringW_t   g_origGet   = NULL;
static WritePPStringW_t g_origWrite = NULL;

static void Log(const char *msg)
{
#if DEBUG_LOG
    OutputDebugStringA(msg);
#else
    (void)msg;
#endif
}

/* ---- command line parsing ------------------------------------------------ */

static void TrimInPlace(WCHAR *s)
{
    size_t n = wcslen(s), start = 0;
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
    while (s[start] == L' ' || s[start] == L'\t') start++;
    if (start) memmove(s, s + start, (n - start + 1) * sizeof(WCHAR));
}

/* Splits the raw command line into tokens (quotes removed) and looks for
 * --gameid=VALUE / --gameid VALUE. cmd.exe passes quotes through verbatim,
 * so --gameid="1a96545b" arrives with the quotes in place; we strip them. */
static void ParseCommandLine(void)
{
    const WCHAR *p = GetCommandLineW();
    const size_t argLen = wcslen(ARG_NAME);
    BOOL first = TRUE;      /* first token is the exe path, skip it */
    BOOL wantValue = FALSE; /* previous token was a bare --gameid */

    if (!p) return;

    while (*p) {
        WCHAR tok[512];
        size_t n = 0;
        BOOL inQuote = FALSE;

        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;

        while (*p && (inQuote || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') { inQuote = !inQuote; p++; continue; }
            if (n < (sizeof(tok) / sizeof(tok[0])) - 1) tok[n++] = *p;
            p++;
        }
        tok[n] = 0;

        if (first) { first = FALSE; continue; }

        if (wantValue) {
            wcsncpy(g_gameId, tok, (sizeof(g_gameId) / sizeof(g_gameId[0])) - 1);
            TrimInPlace(g_gameId);
            return;
        }

        if (_wcsnicmp(tok, ARG_NAME, argLen) == 0) {
            if (tok[argLen] == L'=') {
                wcsncpy(g_gameId, tok + argLen + 1,
                        (sizeof(g_gameId) / sizeof(g_gameId[0])) - 1);
                TrimInPlace(g_gameId);
                return;
            }
            if (tok[argLen] == 0) wantValue = TRUE;
        }
    }
}

/* ---- hooks --------------------------------------------------------------- */

static BOOL IsAzimuthIni(LPCWSTR file)
{
    size_t n, m = wcslen(INI_FILE);
    if (!file) return FALSE;
    n = wcslen(file);
    if (n < m) return FALSE;
    if (n > m) {
        WCHAR c = file[n - m - 1];
        if (c != L'\\' && c != L'/') return FALSE;
    }
    return _wcsicmp(file + (n - m), INI_FILE) == 0;
}

static BOOL IsGameKey(LPCWSTR section, LPCWSTR key, LPCWSTR file)
{
    return g_gameId[0] && section && key &&
           _wcsicmp(section, INI_SECTION) == 0 &&
           _wcsicmp(key, INI_KEY) == 0 &&
           IsAzimuthIni(file);
}

static DWORD WINAPI HookGetPrivateProfileStringW(LPCWSTR section, LPCWSTR key,
                                                 LPCWSTR def, LPWSTR out,
                                                 DWORD size, LPCWSTR file)
{
    if (out && size && IsGameKey(section, key, file)) {
        size_t len = wcslen(g_gameId);
        if (len > size - 1) len = size - 1;
        memcpy(out, g_gameId, len * sizeof(WCHAR));
        out[len] = 0;
        Log("azimuth_gameid: overrode [launcher] game=\n");
        return (DWORD)len;
    }
    return g_origGet(section, key, def, out, size, file);
}

static BOOL WINAPI HookWritePrivateProfileStringW(LPCWSTR section, LPCWSTR key,
                                                  LPCWSTR value, LPCWSTR file)
{
#if BLOCK_INI_WRITES_OF_KEY
    if (IsGameKey(section, key, file)) {
        Log("azimuth_gameid: swallowed write of [launcher] game=\n");
        return TRUE;
    }
#endif
    return g_origWrite(section, key, value, file);
}

/* ---- import table patching ----------------------------------------------- */

static BOOL PatchImport(HMODULE mod, const char *funcName, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *desc;

    if (!dir.VirtualAddress) return FALSE;
    desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);

    for (; desc->Name; desc++) {
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base +
            (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);

        for (; names->u1.AddressOfData; names++, iat++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD oldProt;

            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, funcName) != 0) continue;

            if (!VirtualProtect(&iat->u1.Function, sizeof(void *),
                                PAGE_READWRITE, &oldProt))
                return FALSE;
            *orig = (void *)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), oldProt, &oldProt);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- entry point --------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE exe = GetModuleHandleW(NULL);
        DisableThreadLibraryCalls(inst);

        ParseCommandLine();
        if (!g_gameId[0]) return TRUE; /* no --gameid: stay out of the way */

        if (!PatchImport(exe, "GetPrivateProfileStringW",
                         (void *)HookGetPrivateProfileStringW,
                         (void **)&g_origGet)) {
            Log("azimuth_gameid: GetPrivateProfileStringW import not found\n");
            return TRUE;
        }
        PatchImport(exe, "WritePrivateProfileStringW",
                    (void *)HookWritePrivateProfileStringW,
                    (void **)&g_origWrite);

        if (!g_origWrite) {
            /* Never leave a NULL we might call; fall back to the real API. */
            g_origWrite = (WritePPStringW_t)GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "WritePrivateProfileStringW");
        }
    }
    return TRUE;
}
