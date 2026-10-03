
#include "framework.h"
CAppModule _Module;

#include "MainFrame.h"

int wmain(int argc, wchar_t** argv)
{
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    _Module.Init(nullptr, ::GetModuleHandleW(nullptr));

    CMessageLoop theLoop;
    _Module.AddMessageLoop(&theLoop);

    CMainFrame wndMain;
    if (wndMain.CreateEx() == nullptr)
    {
        _Module.RemoveMessageLoop();
        _Module.Term();
        ::CoUninitialize();
        return 1;
    }

    wndMain.ShowWindow(SW_SHOWDEFAULT);
    wndMain.UpdateWindow();

    if (argc >= 2 && argv[1] && argv[1][0])
        wndMain.OpenImage(argv[1]);

    int nRet = theLoop.Run();

    _Module.RemoveMessageLoop();
    _Module.Term();
    ::CoUninitialize();
    return nRet;
}