#include "Packer.h"
#include "framework.h"

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule); // 优化：不需要线程通知
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

extern "C" {

    PACKER_API Packer* CreatePacker()
    {
        return new (std::nothrow) Packer();
    }

    PACKER_API void DestroyPacker(Packer* p)
    {
        delete p;
    }

}