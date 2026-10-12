/*
 * xinput1_3.dll proxy for Terminator: Resistance (UE 4.25, Terminator-Win64-Shipping.exe, x64).
 *
 * "Dual input": Steam Input turns gyro into native mouse movement ("As Mouse"); the game already
 * accepts that for mouse look. This proxy keeps the mouse linear and gives the right stick
 * (Steam right trackpad) priority: while the stick is deflected, mouse deltas are zeroed.
 *
 * How it works
 *  - The exe imports XINPUT1_3.dll by ordinal only (#2 XInputGetState, #3 XInputSetState),
 *    so this DLL exports the XInput 1.3 ordinals and forwards to the real system DLL.
 *  - Steam Input detours XInputGetState, so code inside our export may never run. We patch
 *    the exe's IAT slot for XINPUT1_3 #2 to HookGetState, which calls whatever the slot held
 *    before (Steam's pointer) and records the right stick.
 *  - The exe's user32!GetRawInputData IAT slot (and GetRawInputBuffer, if imported) is patched;
 *    RIM_TYPEMOUSE relative packets are zeroed while the stick overrides, otherwise scaled/inverted.
 *
 * Terminator-specific design (exe byte patches, all verified against the expected bytes first)
 *  The game has one global byte g_bUsingController (RVA 0x4199e70, 1 = gamepad) that drives BOTH the
 *  button glyphs and the look processing (stick acceleration curve, pad yaw/pitch scales). A mouse
 *  move normally sets it to 0, so glyphs flip to KB/M whenever the gyro-as-mouse moves, and while it
 *  is 1 the game would run the mouse through the gamepad curve. We split the two roles:
 *   A) Glyphs: three conditional jumps are patched (first byte only) so mouse movement can never
 *      clear g (PlayerController InputAxis may only switch to gamepad; widget MouseMove handlers never
 *      clear it). Keyboard keys / mouse buttons still switch to KB/M (InputKey is untouched).
 *      A is applied/reverted live with the F10 toggle.
 *   B) Look: the 8 'cmp byte ptr [g],0' sites in AddYaw/AddPitch/pawn code are re-pointed (rel32) to a
 *      mod-owned FLAG byte in zero slack at the end of .data (RVA 0x4473F00). The XInputGetState hook
 *      (game thread, every frame) sets FLAG=1 only while the right stick is deflected past
 *      PadCurveThreshold, so gyro mouse stays linear while glyphs show the gamepad. With the mod
 *      toggled off, FLAG mirrors g every frame (vanilla behaviour). B is all-or-nothing.
 *   C) ForceGamepadAtStart writes g=1 once after patching.
 *  Ini key Patches=0 skips A/B/C entirely (troubleshooting). The DLL only patches the in-memory image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#ifndef QWORD
typedef unsigned __int64 QWORD;   /* needed by NEXTRAWINPUTBLOCK on x64 */
#endif

/* ---- minimal XInput definitions (no xinput.h so nothing clashes with our exports) ---- */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGAMEPAD;
typedef struct { DWORD dwPacketNumber; XGAMEPAD Gamepad; } XSTATE;

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XSTATE *);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, void *);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, void *);
typedef void  (WINAPI *PFN_Enable)(BOOL);
typedef DWORD (WINAPI *PFN_Guids)(DWORD, LPWSTR, UINT *, LPWSTR, UINT *);
typedef DWORD (WINAPI *PFN_Battery)(DWORD, BYTE, void *);
typedef DWORD (WINAPI *PFN_Keystroke)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *PFN_Wait)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *PFN_Cancel)(DWORD);
typedef DWORD (WINAPI *PFN_PowerOff)(DWORD);
typedef UINT  (WINAPI *PFN_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef UINT  (WINAPI *PFN_GetRawInputBuffer)(PRAWINPUT, PUINT, UINT);

/* ---- globals ---- */
static HMODULE g_self, g_real;
static char    g_realPath[MAX_PATH];
static char    g_exePath[MAX_PATH];
static char    g_ini[MAX_PATH], g_logPath[MAX_PATH], g_logOldPath[MAX_PATH];
static BYTE   *g_exe;
static DWORD   g_exeImageSize;
static ULONGLONG g_exeFileSize;
static LARGE_INTEGER g_freq;

static PFN_GetState  p_GetState, p_GetStateEx;
static PFN_SetState  p_SetState;
static PFN_GetCaps   p_GetCaps;
static PFN_Enable    p_Enable;
static PFN_Guids     p_Guids;
static PFN_Battery   p_Battery;
static PFN_Keystroke p_Keystroke;
static PFN_Wait      p_Wait;
static PFN_Cancel    p_Cancel;
static PFN_PowerOff  p_PowerOff;

/* settings */
static volatile LONG g_enabled = 1;
static int   g_padIndex   = -1;
static float g_override   = 0.20f;
static int   g_releaseMs  = 80;
static float g_gyroSens   = 1.0f;
static int   g_invertX    = 0;
static int   g_invertY    = 0;
static int   g_toggleKey  = 0x79;
static int   g_logLevel   = 1;
static int   g_patches    = 1;          /* 0 = skip patches A/B/C */
static int   g_forceGamepad = 1;        /* write g=1 once after patching */
static float g_padCurveThr = 0.24f;     /* right-stick axis fraction above which FLAG=1 */

/* ---------------------------------------------------------------- logging */
static CRITICAL_SECTION g_logCs;
static FILE *g_logFile;

static void Log(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME st;
    if (!g_logLevel || !g_logFile) return;
    EnterCriticalSection(&g_logCs);
    GetLocalTime(&st);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logCs);
}

static LONGLONG Now(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

/* ---------------------------------------------------------------- settings */
static const char k_defaultIni[] =
    "; xinput_mouse.ini - dual input proxy for Terminator: Resistance\r\n"
    "; Gyro (Steam Input \"As Mouse\") stays a native, linear mouse. When the right stick is\r\n"
    "; pushed, the mouse look is suppressed so the stick has priority.\r\n"
    "[Mouse]\r\n"
    "; 1 = mod active, 0 = pure pass-through. The ToggleKey switches this at runtime.\r\n"
    "Enabled=1\r\n"
    "; XInput user index whose right stick has priority (-1 = any connected pad)\r\n"
    "PadIndex=-1\r\n"
    "; right stick (trackpad) magnitude above this fraction of full range (0..1) discards mouse look\r\n"
    "StickOverrideDeadzone=0.20\r\n"
    "; keep discarding mouse look this many ms after the stick returns under the threshold\r\n"
    "; (stops gyro from jumping in during stick return)\r\n"
    "StickReleaseMs=80\r\n"
    "; multiplier on mouse/gyro deltas that are let through (1.0 = unchanged; fractions are accumulated, nothing is lost)\r\n"
    "GyroSensitivity=1.0\r\n"
    "; 1 = invert the axis of the passed-through mouse movement\r\n"
    "InvertX=0\r\n"
    "InvertY=0\r\n"
    "; virtual-key code (hex or decimal) that toggles the mod while the game window is focused. 0x79 = F10, 0 = none\r\n"
    "ToggleKey=0x79\r\n"
    "; 0 = no log, 1 = init info only, 2 = also once-per-second statistics (written to mixed_input.log next to the DLL)\r\n"
    "Log=1\r\n"
    "; 1 = apply the exe patches (keep gamepad glyphs while the mouse moves, gamepad look curve only while the stick is used). 0 = skip all patches\r\n"
    "Patches=1\r\n"
    "; 1 = set the game's 'using controller' state to gamepad once at start (needs Patches=1)\r\n"
    "ForceGamepadAtStart=1\r\n"
    "; right stick axis (|RX| or |RY|, fraction of full range) above which the game's gamepad look curve is enabled; just under the game's 0.25 deadzone\r\n"
    "PadCurveThreshold=0.24\r\n";

static float GetF(const char *key, const char *def)
{
    char buf[64];
    GetPrivateProfileStringA("Mouse", key, def, buf, sizeof buf, g_ini);
    return (float)atof(buf);
}

static void LoadSettings(void)
{
    char buf[64];
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(g_ini, "wb");
        if (f) { fwrite(k_defaultIni, 1, sizeof k_defaultIni - 1, f); fclose(f); }
    }
    g_enabled    = GetPrivateProfileIntA("Mouse", "Enabled", 1, g_ini) != 0;
    g_padIndex   = GetPrivateProfileIntA("Mouse", "PadIndex", -1, g_ini);
    g_override   = GetF("StickOverrideDeadzone", "0.20");
    g_releaseMs  = GetPrivateProfileIntA("Mouse", "StickReleaseMs", 80, g_ini);
    g_gyroSens   = GetF("GyroSensitivity", "1.0");
    g_invertX    = GetPrivateProfileIntA("Mouse", "InvertX", 0, g_ini) != 0;
    g_invertY    = GetPrivateProfileIntA("Mouse", "InvertY", 0, g_ini) != 0;
    g_logLevel   = GetPrivateProfileIntA("Mouse", "Log", 1, g_ini);
    g_patches    = GetPrivateProfileIntA("Mouse", "Patches", 1, g_ini) != 0;
    g_forceGamepad = GetPrivateProfileIntA("Mouse", "ForceGamepadAtStart", 1, g_ini) != 0;
    g_padCurveThr = GetF("PadCurveThreshold", "0.24");
    GetPrivateProfileStringA("Mouse", "ToggleKey", "0x79", buf, sizeof buf, g_ini);
    g_toggleKey  = (int)strtoul(buf, NULL, 0);
    if (g_padIndex < -1 || g_padIndex > 3) g_padIndex = -1;
    if (g_override < 0.0f) g_override = 0.0f;
    if (g_override > 1.0f) g_override = 1.0f;
    if (!(g_padCurveThr >= 0.0f) || g_padCurveThr > 1.0f) g_padCurveThr = 0.24f;
    if (g_releaseMs < 0) g_releaseMs = 0;
    if (g_releaseMs > 2000) g_releaseMs = 2000;
    if (!(g_gyroSens > 0.0f) || g_gyroSens > 100.0f) g_gyroSens = 1.0f;
    if (g_toggleKey < 0 || g_toggleKey > 0xFE) g_toggleKey = 0;
}

/* ---------------------------------------------------------------- exe helpers */
static BYTE *ExeBase(void)
{
    return g_exe;
}

/* exe base + rva, or NULL if [rva, rva+len) is outside the image */
static BYTE *ExeRva(DWORD rva, DWORD len)
{
    if (!g_exe || (ULONGLONG)rva + len > g_exeImageSize) return NULL;
    return g_exe + rva;
}

/* Verified byte patch at exe RVA: compares `expected` (len bytes) with current contents, refuses on
   mismatch. Returns 1 on success, 0 on refusal/failure. Result is always logged. */
static int PatchExeBytes(DWORD rva, const BYTE *expected, const BYTE *newbytes, DWORD len, const char *what)
{
    BYTE *p = ExeRva(rva, len);
    DWORD old, tmp;
    BYTE cur[64];
    char hex[64 * 3 + 1];
    DWORD i;
    if (!p || len == 0 || len > sizeof cur) { Log("patch '%s' RVA 0x%lx len %lu: bad range, refused", what, (unsigned long)rva, (unsigned long)len); return 0; }
    __try { memcpy(cur, p, len); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("patch '%s' RVA 0x%lx: unreadable, refused", what, (unsigned long)rva); return 0; }
    if (memcmp(cur, expected, len) != 0) {
        for (i = 0; i < len; i++) sprintf(hex + i * 3, "%02X ", cur[i]);
        Log("patch '%s' RVA 0x%lx: byte mismatch, REFUSED (found: %s)", what, (unsigned long)rva, hex);
        return 0;
    }
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) { Log("patch '%s' RVA 0x%lx: VirtualProtect failed err=%lu", what, (unsigned long)rva, GetLastError()); return 0; }
    memcpy(p, newbytes, len);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    VirtualProtect(p, len, old, &tmp);
    Log("patch '%s' RVA 0x%lx (%lu bytes): applied", what, (unsigned long)rva, (unsigned long)len);
    return 1;
}

#define G_RVA     0x4199e70u
#define FLAG_RVA  0x4473F00u

static volatile BYTE *g_gPtr;           /* g_bUsingController, NULL if unavailable */
static volatile BYTE *g_flag;           /* mod-owned look flag, non-NULL only when patch B is applied */
static volatile LONG  g_aApplied;       /* patch A fully applied */

typedef struct { DWORD rva; BYTE orig[7]; BYTE newFirst; const char *what; } PATCH_A;
static const PATCH_A k_patchA[] = {
    { 0xb27524, { 0x74,0x09,0x40,0x0f,0xb6,0xd7,0xe8 }, 0x73, "A InputAxis je->jae" },
    { 0xba7c77, { 0x74,0x1d,0x0f,0x57,0xc0,0x41,0x0f }, 0xeb, "A widget MouseMove" },
    { 0xba7cf0, { 0x75,0x16,0x48,0x8b,0xcb,0x88,0x05 }, 0xeb, "A menu widget MouseMove" },
};

typedef struct { DWORD rva; BYTE expected[7]; BYTE nw[7]; } PATCH_B;
static const PATCH_B k_patchB[] = {
    { 0xb109db, { 0x80,0x3d,0x8e,0x94,0x68,0x03,0x00 }, { 0x80,0x3d,0x1e,0x35,0x96,0x03,0x00 } },
    { 0xb108cb, { 0x80,0x3d,0x9e,0x95,0x68,0x03,0x00 }, { 0x80,0x3d,0x2e,0x36,0x96,0x03,0x00 } },
    { 0xb38c40, { 0x80,0x3d,0x29,0x12,0x66,0x03,0x00 }, { 0x80,0x3d,0xb9,0xb2,0x93,0x03,0x00 } },
    { 0xb60625, { 0x80,0x3d,0x44,0x98,0x63,0x03,0x00 }, { 0x80,0x3d,0xd4,0x38,0x91,0x03,0x00 } },
    { 0xb631ba, { 0x80,0x3d,0xaf,0x6c,0x63,0x03,0x00 }, { 0x80,0x3d,0x3f,0x0d,0x91,0x03,0x00 } },
    { 0xb631e1, { 0x80,0x3d,0x88,0x6c,0x63,0x03,0x00 }, { 0x80,0x3d,0x18,0x0d,0x91,0x03,0x00 } },
    { 0xb7b5da, { 0x80,0x3d,0x8f,0xe8,0x61,0x03,0x00 }, { 0x80,0x3d,0x1f,0x89,0x8f,0x03,0x00 } },
    { 0xb7b601, { 0x80,0x3d,0x68,0xe8,0x61,0x03,0x00 }, { 0x80,0x3d,0xf8,0x88,0x8f,0x03,0x00 } },
};
#define NPATCH_A ((int)(sizeof k_patchA / sizeof k_patchA[0]))
#define NPATCH_B ((int)(sizeof k_patchB / sizeof k_patchB[0]))

static void LogPatchState(const char *why)
{
    Log("STATE (%s): exeBase=%p FLAG addr=%p val=%d | g addr=%p val=%d | Enabled=%ld patches=%d A=%ld B=%d",
        why, (void *)ExeBase(), (void *)g_flag, g_flag ? (int)*g_flag : -1, (void *)g_gPtr, g_gPtr ? (int)*g_gPtr : -1,
        g_enabled, g_patches, g_aApplied, g_flag != NULL);
}

/* Apply (on=1) or revert (on=0) patch A. Both directions compare all 7 bytes and write only the first.
   Sites already in the wanted state count as done. Returns the number of sites in the wanted state. */
static int SetPatchA(int on)
{
    int i, n = 0;
    for (i = 0; i < NPATCH_A; i++) {
        const PATCH_A *t = &k_patchA[i];
        BYTE from[7], to[7], cur[7];
        BYTE *p = ExeRva(t->rva, 7);
        DWORD old, tmp;
        memcpy(from, t->orig, 7); memcpy(to, t->orig, 7);
        if (on) to[0] = t->newFirst; else from[0] = t->newFirst;
        if (!p) { Log("patch '%s': bad range", t->what); continue; }
        __try { memcpy(cur, p, 7); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("patch '%s': unreadable", t->what); continue; }
        if (memcmp(cur, to, 7) == 0) { n++; continue; }
        if (memcmp(cur, from, 7) != 0) { Log("patch '%s' RVA 0x%lx: unexpected bytes, %s REFUSED", t->what, (unsigned long)t->rva, on ? "apply" : "revert"); continue; }
        if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) { Log("patch '%s': VirtualProtect failed err=%lu", t->what, GetLastError()); continue; }
        p[0] = to[0];
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        VirtualProtect(p, 1, old, &tmp);
        Log("patch '%s' RVA 0x%lx: first byte %02X -> %02X (%s)", t->what, (unsigned long)t->rva, from[0], to[0], on ? "applied" : "reverted");
        n++;
    }
    g_aApplied = (on && n == NPATCH_A);
    return n;
}

/* FLAG must lie in the zero slack after the used part of .data, inside its last page. */
static int CheckFlagSlot(void)
{
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    WORD i;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p = ExeRva(FLAG_RVA, 1);
    if (!p) { Log("FLAG: RVA 0x%lx outside image (size 0x%lx), B refused", (unsigned long)FLAG_RVA, (unsigned long)g_exeImageSize); return 0; }
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char nm[9];
        DWORD va = sec[i].VirtualAddress, vs = sec[i].Misc.VirtualSize, end, pageEnd;
        memcpy(nm, sec[i].Name, 8); nm[8] = 0;
        if (strcmp(nm, ".data") != 0) continue;
        end = va + vs;
        pageEnd = (end + 0xFFF) & ~0xFFFu;
        Log("FLAG: .data VA=0x%lx VirtualSize=0x%lx usedEnd=0x%lx pageEnd=0x%lx flags=0x%lx",
            (unsigned long)va, (unsigned long)vs, (unsigned long)end, (unsigned long)pageEnd, (unsigned long)sec[i].Characteristics);
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) { Log("FLAG: .data not writable, B refused"); return 0; }
        if (!(FLAG_RVA >= end && FLAG_RVA < pageEnd)) { Log("FLAG: RVA 0x%lx not in .data slack [0x%lx,0x%lx), B refused", (unsigned long)FLAG_RVA, (unsigned long)end, (unsigned long)pageEnd); return 0; }
        memset(&mbi, 0, sizeof mbi);
        if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
            Log("FLAG: page not committed/writable (protect=0x%lx), B refused", (unsigned long)mbi.Protect);
            return 0;
        }
        if (*p != 0) { Log("FLAG: byte is %02X, expected 0, B refused", *p); return 0; }
        return 1;
    }
    Log("FLAG: .data section not found, B refused");
    return 0;
}

/* Patch B, all-or-nothing: returns 1 if all sites were rewritten, 0 if nothing was changed. */
static int ApplyLookPatchB(void)
{
    int i;
    BYTE built[8][7];
    if (NPATCH_B > 8 || !CheckFlagSlot()) return 0;
    for (i = 0; i < NPATCH_B; i++) {                    /* pass 1: compute + verify every site */
        const PATCH_B *t = &k_patchB[i];
        BYTE *p = ExeRva(t->rva, 7);
        INT32 rel = (INT32)((INT64)FLAG_RVA - ((INT64)t->rva + 7));
        BYTE cur[7];
        memcpy(built[i], t->expected, 7);
        memcpy(&built[i][2], &rel, 4);
        if (memcmp(built[i], t->nw, 7) != 0) { Log("patch B site 0x%lx: computed rel32 != table value, B refused", (unsigned long)t->rva); return 0; }
        if (!p) { Log("patch B site 0x%lx: bad range, B refused", (unsigned long)t->rva); return 0; }
        __try { memcpy(cur, p, 7); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("patch B site 0x%lx: unreadable, B refused", (unsigned long)t->rva); return 0; }
        if (memcmp(cur, t->expected, 7) != 0) {
            char hex[32];
            int k;
            for (k = 0; k < 7; k++) sprintf(hex + k * 3, "%02X ", cur[k]);
            Log("patch B site 0x%lx: byte mismatch (found %s), B NOT applied (all-or-nothing)", (unsigned long)t->rva, hex);
            return 0;
        }
    }
    for (i = 0; i < NPATCH_B; i++)                      /* pass 2: write */
        if (!PatchExeBytes(k_patchB[i].rva, k_patchB[i].expected, built[i], 7, "B cmp [g] -> [FLAG]"))
            Log("patch B: write at site 0x%lx failed after verification (unexpected, B may be partial)", (unsigned long)k_patchB[i].rva);
    g_flag = ExeRva(FLAG_RVA, 1);                       /* hook starts maintaining FLAG from now on */
    *g_flag = (g_gPtr && *g_gPtr) ? 1 : 0;              /* sane start value until the first XInput call */
    return 1;
}

/* Runs once from the worker thread, after the exe is loaded. */
static void ApplyGamepadGlyphPatch(void)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    Log("exe identity: fileSize=%llu TimeDateStamp=0x%08lx SizeOfImage=0x%lx",
        g_exeFileSize, (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
    g_gPtr = (volatile BYTE *)ExeRva(G_RVA, 1);
    if (!g_patches) { Log("Patches=0: patches A/B/C skipped"); LogPatchState("init"); return; }
    if (g_enabled) {
        int n = SetPatchA(1);
        Log("patch A: %d/%d sites active", n, NPATCH_A);
    } else Log("mod starts disabled: patch A not applied until toggled on");
    Log("patch B: %s", ApplyLookPatchB() ? "applied (8/8 sites)" : "NOT applied");
    if (g_forceGamepad && g_gPtr) { *g_gPtr = 1; Log("ForceGamepadAtStart: g=1 written"); }
    LogPatchState("init");
}

/* ---------------------------------------------------------------- stick state */
static volatile LONG g_axis[4];                 /* max(|RX|,|RY|) raw (0..32768) per user index */
static volatile LONG g_mag[4];                 /* right stick magnitude * 10000, per user index */
static volatile LONG g_connected;               /* bitmask of pads that last returned success */
static volatile LONGLONG g_lastAbove;           /* QPC time the stick was last above the threshold */
static volatile LONG g_maxMag;                  /* max magnitude * 10000 since last status line */

/* statistics */
static volatile LONG64 s_pass, s_supp, s_calls;

static int StickAbove(void)
{
    LONG best = 0, thr = (LONG)(g_override * 10000.0f);
    int i;
    if (g_padIndex >= 0) best = g_mag[g_padIndex];
    else for (i = 0; i < 4; i++) if (g_mag[i] > best) best = g_mag[i];
    return best > thr;
}

/* returns 1 when mouse deltas must be zeroed right now */
static int ShouldSuppress(void)
{
    LONGLONG now;
    now = Now();
    if (StickAbove()) { g_lastAbove = now; return 1; }
    if (g_releaseMs > 0 && g_lastAbove && now - g_lastAbove < g_freq.QuadPart * g_releaseMs / 1000) return 1;
    return 0;
}

/* ---------------------------------------------------------------- hooks */
static PFN_GetState g_origGetState;
static PFN_GetRawInputData g_origRaw;
static PFN_GetRawInputBuffer g_origRawBuf;

/* game thread, every XInputGetState: keep the mod-owned look flag current */
static void UpdateFlag(void)
{
    volatile BYTE *f = g_flag;
    LONG best = 0, thr;
    int i;
    if (!f) return;
    if (!g_enabled) { *f = (g_gPtr && *g_gPtr) ? 1 : 0; return; }      /* vanilla: mirror g */
    thr = (LONG)(g_padCurveThr * 32767.0f);
    if (g_padIndex >= 0) best = g_axis[g_padIndex];
    else for (i = 0; i < 4; i++) if (g_axis[i] > best) best = g_axis[i];
    *f = best > thr ? 1 : 0;
}

static DWORD WINAPI HookGetState(DWORD user, XSTATE *st)
{
    DWORD r = g_origGetState ? g_origGetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (user > 3) return r;
    InterlockedIncrement64(&s_calls);
    if (r != ERROR_SUCCESS || !st) {
        g_mag[user] = 0;
        g_axis[user] = 0;
        UpdateFlag();
        InterlockedAnd(&g_connected, ~(1L << user));
        return r;
    }
    InterlockedOr(&g_connected, 1L << user);
    {
        float sx = st->Gamepad.sThumbRX / 32767.0f;
        float sy = st->Gamepad.sThumbRY / 32767.0f;
        LONG m = (LONG)(sqrtf(sx * sx + sy * sy) * 10000.0f);
        g_mag[user] = m;
        if (m > g_maxMag) g_maxMag = m;
        {
            LONG ax = abs((int)st->Gamepad.sThumbRX), ay = abs((int)st->Gamepad.sThumbRY);
            g_axis[user] = ax > ay ? ax : ay;
        }
    }
    UpdateFlag();
    return r;
}

/* mouse movement processing, shared by GetRawInputData and GetRawInputBuffer */
static float g_remX, g_remY;

static void ProcessMouse(RAWINPUT *ri)
{
    LONG dx, dy;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;
    dx = ri->data.mouse.lLastX; dy = ri->data.mouse.lLastY;
    if (!dx && !dy) return;
    if (!g_enabled) return;
    if (ShouldSuppress()) {
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        InterlockedIncrement64(&s_supp);
        return;
    }
    if (g_invertX) dx = -dx;
    if (g_invertY) dy = -dy;
    if (g_gyroSens != 1.0f) {
        float vx = dx * g_gyroSens + g_remX, vy = dy * g_gyroSens + g_remY;
        dx = (LONG)vx; dy = (LONG)vy;               /* truncate toward zero, carry the rest */
        g_remX = vx - (float)dx; g_remY = vy - (float)dy;
    }
    ri->data.mouse.lLastX = dx;
    ri->data.mouse.lLastY = dy;
    InterlockedIncrement64(&s_pass);
}

static UINT WINAPI HookGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT pcb, UINT hdr)
{
    UINT r = g_origRaw(h, cmd, data, pcb, hdr);
    /* size query (pData NULL) and RID_HEADER are passed through untouched */
    if (r == (UINT)-1 || cmd != RID_INPUT || !data || r < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) return r;
    ProcessMouse((RAWINPUT *)data);
    return r;
}

static UINT WINAPI HookGetRawInputBuffer(PRAWINPUT data, PUINT pcb, UINT hdr)
{
    UINT n = g_origRawBuf(data, pcb, hdr);
    if (n == (UINT)-1 || n == 0 || !data) return n;      /* size query or error */
    {
        PRAWINPUT ri = data;
        UINT i;
        for (i = 0; i < n; i++) {
            ProcessMouse(ri);
            ri = NEXTRAWINPUTBLOCK(ri);
        }
    }
    return n;
}

/* ---- IAT patching ---- */
static volatile LONG g_xiPatched, g_rawPatched, g_rawBufPatched;
static int g_rawBufImported;

static int PatchSlot(ULONG_PTR *slot, ULONG_PTR newval)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *slot = newval;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static int InstallIat(void)
{
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!g_exe) return 0;
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(g_exe + dir->VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(g_exe + d->Name);
        ULONG_PTR *oft = (ULONG_PTR *)(g_exe + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        ULONG_PTR *ft = (ULONG_PTR *)(g_exe + d->FirstThunk);
        int i;
        int isXi = !_stricmp(dll, "xinput1_3.dll");
        int isUser = !_stricmp(dll, "user32.dll");
        if (!isXi && !isUser) continue;
        for (i = 0; oft[i]; i++) {
            int byOrd = (oft[i] & IMAGE_ORDINAL_FLAG64) != 0;
            const char *name = byOrd ? NULL : (const char *)((IMAGE_IMPORT_BY_NAME *)(g_exe + (oft[i] & 0x7FFFFFFFFFFFFFFFULL)))->Name;
            if (isXi && !g_xiPatched && ((byOrd && (oft[i] & 0xFFFF) == 2) || (name && !strcmp(name, "XInputGetState")))) {
                ULONG_PTR cur = ft[i];
                g_origGetState = (PFN_GetState)cur;
                if (PatchSlot(&ft[i], (ULONG_PTR)HookGetState)) {
                    InterlockedExchange(&g_xiPatched, 1);
                    Log("IAT XInputGetState (xinput1_3 %s): slot VA=0x%llx RVA=0x%llx orig=%p ourExport=%p -> hook=%p",
                        byOrd ? "#2" : "by name", (ULONGLONG)(ULONG_PTR)&ft[i], (ULONGLONG)((BYTE *)&ft[i] - g_exe),
                        (void *)cur, (void *)p_GetState, (void *)HookGetState);
                } else {
                    g_origGetState = NULL;
                    Log("IAT XInputGetState: VirtualProtect failed err=%lu", GetLastError());
                }
            }
            if (isUser && name && !g_rawPatched && !strcmp(name, "GetRawInputData")) {
                ULONG_PTR cur = ft[i];
                g_origRaw = (PFN_GetRawInputData)cur;
                if (PatchSlot(&ft[i], (ULONG_PTR)HookGetRawInputData)) {
                    InterlockedExchange(&g_rawPatched, 1);
                    Log("IAT GetRawInputData (user32): slot VA=0x%llx RVA=0x%llx orig=%p -> hook=%p",
                        (ULONGLONG)(ULONG_PTR)&ft[i], (ULONGLONG)((BYTE *)&ft[i] - g_exe), (void *)cur, (void *)HookGetRawInputData);
                } else {
                    g_origRaw = NULL;
                    Log("IAT GetRawInputData: VirtualProtect failed err=%lu", GetLastError());
                }
            }
            if (isUser && name && !g_rawBufPatched && !strcmp(name, "GetRawInputBuffer")) {
                ULONG_PTR cur = ft[i];
                g_rawBufImported = 1;
                g_origRawBuf = (PFN_GetRawInputBuffer)cur;
                if (PatchSlot(&ft[i], (ULONG_PTR)HookGetRawInputBuffer)) {
                    InterlockedExchange(&g_rawBufPatched, 1);
                    Log("IAT GetRawInputBuffer (user32): slot VA=0x%llx RVA=0x%llx orig=%p -> hook=%p",
                        (ULONGLONG)(ULONG_PTR)&ft[i], (ULONGLONG)((BYTE *)&ft[i] - g_exe), (void *)cur, (void *)HookGetRawInputBuffer);
                } else {
                    g_origRawBuf = NULL;
                    Log("IAT GetRawInputBuffer: VirtualProtect failed err=%lu", GetLastError());
                }
            }
        }
    }
    return g_xiPatched && g_rawPatched && (g_rawBufPatched || !g_rawBufImported);
}

static CRITICAL_SECTION g_instCs;
static volatile LONG g_lazyTried;

static void TryInstall(void)
{
    EnterCriticalSection(&g_instCs);
    if (!(g_xiPatched && g_rawPatched)) InstallIat();
    LeaveCriticalSection(&g_instCs);
}

static void EnsureInit(void);
static void EnsureInstalled(void)       /* called lazily from every export, first call only does work */
{
    EnsureInit();
    if (InterlockedExchange(&g_lazyTried, 1)) return;
    TryInstall();
}

/* ---- toggle hotkey ---- */
static void PollHotkey(void)
{
    static int prev;
    int down = 0;
    if (g_toggleKey) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        down = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
    }
    if (down && !prev) {
        LONG nv = !g_enabled;
        InterlockedExchange(&g_enabled, nv);
        Log("toggle key 0x%x: Enabled=%ld", g_toggleKey, nv);
        if (g_patches) {
            int n = SetPatchA(nv != 0);
            Log("patch A: %d/%d sites in %s state", n, NPATCH_A, nv ? "patched" : "original");
            /* FLAG itself is mirrored from g by the XInputGetState hook while disabled */
        }
        LogPatchState("toggle");
    }
    prev = down;
}

/* ---- worker thread: retries installation, hotkey, glyph patch, status ---- */
static DWORD WINAPI WorkerThread(LPVOID unused)
{
    int tick = 0, installedLogged = 0;
    LONGLONG nextStatus;
    LONG64 lastPass = -1, lastSupp = -1;
    (void)unused;
    EnsureInit();
    ApplyGamepadGlyphPatch();
    nextStatus = Now() + g_freq.QuadPart;
    for (;;) {
        Sleep(20);
        tick++;
        PollHotkey();
        if (!(g_xiPatched && g_rawPatched) && tick % 12 == 0 && tick < 6000) {      /* retry every ~250 ms, up to ~2 min */
            TryInstall();
            if (tick == 1000) {
                if (!g_xiPatched) Log("WARNING: XInputGetState IAT not patched after 20 s");
                if (!g_rawPatched) Log("WARNING: GetRawInputData IAT not patched after 20 s");
            }
        }
        if (!installedLogged && g_xiPatched && g_rawPatched) {
            installedLogged = 1;
            Log("init complete: xinputIAT=%ld rawIAT=%ld rawBufferImported=%d rawBufferIAT=%ld", g_xiPatched, g_rawPatched, g_rawBufImported, g_rawBufPatched);
        }
        if (g_logLevel >= 2 && Now() >= nextStatus) {
            LONG64 p = s_pass, s = s_supp;
            LONG mm = InterlockedExchange(&g_maxMag, 0);
            nextStatus += g_freq.QuadPart;
            if (p != lastPass || s != lastSupp || mm) {
                Log("STATUS enabled=%ld mouse passed=%lld suppressed=%lld | xinput calls=%lld connected=0x%lx maxStick=%.3f | g=%d FLAG=%d",
                    g_enabled, p, s, s_calls, (unsigned long)g_connected, mm / 10000.0,
                    g_gPtr ? (int)*g_gPtr : -1, g_flag ? (int)*g_flag : -1);
                lastPass = p; lastSupp = s;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- exports */
DWORD WINAPI XInputGetState(DWORD user, XSTATE *st)
{
    EnsureInstalled();
    return p_GetState ? p_GetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, void *v)
{
    EnsureInstalled();
    return p_SetState ? p_SetState(user, v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, void *c)
{
    EnsureInstalled();
    return p_GetCaps ? p_GetCaps(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL e)
{
    EnsureInstalled();
    if (p_Enable) p_Enable(e);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, LPWSTR a, UINT *an, LPWSTR b, UINT *bn)
{
    EnsureInstalled();
    return p_Guids ? p_Guids(user, a, an, b, bn) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE type, void *b)
{
    EnsureInstalled();
    return p_Battery ? p_Battery(user, type, b) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, void *k)
{
    EnsureInstalled();
    return p_Keystroke ? p_Keystroke(user, reserved, k) : ERROR_DEVICE_NOT_CONNECTED;
}

/* undocumented ordinals 100..103 of xinput1_3.dll */
DWORD WINAPI XInputGetStateEx(DWORD user, XSTATE *st)
{
    EnsureInstalled();
    return p_GetStateEx ? p_GetStateEx(user, st) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputWaitForGuideButton(DWORD user, DWORD flags, void *p)
{
    EnsureInstalled();
    return p_Wait ? p_Wait(user, flags, p) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputCancelGuideButtonWait(DWORD user)
{
    EnsureInstalled();
    return p_Cancel ? p_Cancel(user) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputPowerOffController(DWORD user)
{
    EnsureInstalled();
    return p_PowerOff ? p_PowerOff(user) : ERROR_DEVICE_NOT_CONNECTED;
}

/* ---------------------------------------------------------------- init */
static void LoadReal(void)
{
    static const char *names[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
    char sys[MAX_PATH];
    int i;
    GetSystemDirectoryA(sys, MAX_PATH);
    for (i = 0; i < 3 && !g_real; i++) {
        _snprintf(g_realPath, MAX_PATH, "%s\\%s", sys, names[i]);
        g_real = LoadLibraryA(g_realPath);
    }
    if (!g_real) { g_realPath[0] = 0; return; }
    p_GetState   = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
    p_SetState   = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
    p_GetCaps    = (PFN_GetCaps)GetProcAddress(g_real, "XInputGetCapabilities");
    p_Enable     = (PFN_Enable)GetProcAddress(g_real, "XInputEnable");
    p_Guids      = (PFN_Guids)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
    p_Battery    = (PFN_Battery)GetProcAddress(g_real, "XInputGetBatteryInformation");
    p_Keystroke  = (PFN_Keystroke)GetProcAddress(g_real, "XInputGetKeystroke");
    p_GetStateEx = (PFN_GetState)GetProcAddress(g_real, MAKEINTRESOURCEA(100));
    p_Wait       = (PFN_Wait)GetProcAddress(g_real, MAKEINTRESOURCEA(101));
    p_Cancel     = (PFN_Cancel)GetProcAddress(g_real, MAKEINTRESOURCEA(102));
    p_PowerOff   = (PFN_PowerOff)GetProcAddress(g_real, MAKEINTRESOURCEA(103));
    if (!p_GetStateEx) p_GetStateEx = p_GetState;
}

static volatile LONG g_inited;
static CRITICAL_SECTION g_initCs;

static void InitAll(HINSTANCE inst)
{
    char *p;
    HANDLE fh;
    LARGE_INTEGER sz;
    IMAGE_NT_HEADERS *nt;

    LoadReal();

    GetModuleFileNameA(inst, g_ini, MAX_PATH);
    p = strrchr(g_ini, '\\');
    if (p) {
        size_t n = (size_t)(p - g_ini) + 1;
        memcpy(g_logPath, g_ini, n);        strcpy(g_logPath + n, "mixed_input.log");
        memcpy(g_logOldPath, g_ini, n);     strcpy(g_logOldPath + n, "mixed_input.old.log");
        strcpy(p + 1, "xinput_mouse.ini");
    }
    LoadSettings();
    if (g_logLevel && g_logPath[0]) {
        MoveFileExA(g_logPath, g_logOldPath, MOVEFILE_REPLACE_EXISTING);
        g_logFile = fopen(g_logPath, "w");
    }

    g_exe = (BYTE *)GetModuleHandleA(NULL);
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    g_exeImageSize = nt->OptionalHeader.SizeOfImage;
    GetModuleFileNameA(NULL, g_exePath, MAX_PATH);
    fh = CreateFileA(g_exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(fh, &sz)) g_exeFileSize = (ULONGLONG)sz.QuadPart;
        CloseHandle(fh);
    }

    Log("mixed_input proxy loaded (x64). exe='%s' base=%p imageSize=0x%lx fileSize=%llu",
        g_exePath, (void *)g_exe, g_exeImageSize, g_exeFileSize);
    Log("real xinput: '%s' handle=%p GetState=%p", g_realPath, (void *)g_real, (void *)p_GetState);
    Log("settings: Enabled=%ld PadIndex=%d StickOverrideDeadzone=%.2f StickReleaseMs=%d GyroSensitivity=%.3f InvertX=%d InvertY=%d ToggleKey=0x%x Log=%d Patches=%d ForceGamepadAtStart=%d PadCurveThreshold=%.3f",
        g_enabled, g_padIndex, (double)g_override, g_releaseMs, (double)g_gyroSens, g_invertX, g_invertY, g_toggleKey, g_logLevel, g_patches, g_forceGamepad, (double)g_padCurveThr);
}

static void EnsureInit(void)        /* heavy init, never under loader lock */
{
    if (g_inited) return;
    EnterCriticalSection(&g_initCs);
    if (!g_inited) { InitAll(g_self); g_inited = 1; }
    LeaveCriticalSection(&g_initCs);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        QueryPerformanceFrequency(&g_freq);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_instCs);
        InitializeCriticalSection(&g_initCs);
        CloseHandle(CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL));
    }
    return TRUE;
}
