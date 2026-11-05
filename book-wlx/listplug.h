/*
 * WLX (Lister plugin) C ABI, Unix/Qt flavour, trimmed to what MarkdownView uses.
 *
 * Double Commander on Unix loads WLX plugins as ELF shared objects and calls
 * the entry points with the C calling convention. A "window handle" is a
 * QWidget* (see doublecmd's src/uwlxmodule.pas): ListLoad hands the plugin a
 * container widget and expects a plugin-owned widget back, which DC then
 * moves, resizes and focuses. On macOS the same slots carry NSView*.
 */
#ifndef MARKDOWNVIEW_LINUX_LISTPLUG_H
#define MARKDOWNVIEW_LINUX_LISTPLUG_H

#include <cstdint>

#if defined(_WIN32)
#define DCPCALL __stdcall
#else
#define DCPCALL
#endif

/* Pointer-sized opaque handle; DC uses THandle (QWord on 64-bit). */
typedef std::uintptr_t HWND;

/* DC's own MAX_PATH for the ini record is 32000, not the Windows 260. */
#define MDV_MAX_PATH 32000

typedef struct {
    int size;
    std::uint32_t PluginInterfaceVersionLow;
    std::uint32_t PluginInterfaceVersionHi;
    char DefaultIniName[MDV_MAX_PATH];
} ListDefaultParamStruct;

#define lc_copy       1
#define lc_newparams  2
#define lc_selectall  3
#define lc_setpercent 4

#define lcp_wraptext     1
#define lcp_fittowindow  2
#define lcp_ansi         4
#define lcp_ascii        8
#define lcp_variable     12
#define lcp_forceshow    16
#define lcp_fitlargeronly 32
#define lcp_center       64
#define lcp_darkmode     128
#define lcp_darkmodenative 256

#define lcs_findfirst   1
#define lcs_matchcase   2
#define lcs_wholewords  4
#define lcs_backwards   8

#define LISTPLUGIN_OK    0
#define LISTPLUGIN_ERROR 1

#endif /* MARKDOWNVIEW_LINUX_LISTPLUG_H */
