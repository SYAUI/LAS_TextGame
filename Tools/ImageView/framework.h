#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
// ─── 系统头 ───
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h> 

// ─── 标准库 ───
#include <functional>
#include <string>
#include <vector>

// ─── ATL/WTL 头 ───
#include <atlbase.h>
#include <atlstr.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlframe.h>
#include <atlctrls.h>
#include <atldlgs.h>
#include <atluser.h>

// ─── Direct2D ───
#include <d2d1.h>
#include <d2d1helper.h>


// ─── 链接 ───
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")


#include <atlcomcli.h>