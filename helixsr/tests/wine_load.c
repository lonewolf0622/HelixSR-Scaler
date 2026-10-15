// Stands in for a game in test_wine.sh: loads dxgi.dll the way a game does, from its own folder (Wine picks the
// game's copy because of WINEDLLOVERRIDES=dxgi=n,b), and asks it for a DXGI factory so OptiScaler's hooks run.
#include <stdio.h>
#include <windows.h>

typedef HRESULT(WINAPI* CreateFactory1)(REFIID, void**);
static const GUID IID_IDXGIFactory1_ = {
    0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 }
};

int main(void)
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    char path[MAX_PATH] = "";
    if (!dxgi)
    {
        printf("dxgi.dll didn't load (error %lu)\n", GetLastError());
        fflush(stdout);
        ExitProcess(1);
    }
    GetModuleFileNameA(dxgi, path, MAX_PATH);
    printf("dxgi.dll loaded from %s\n", path);

    CreateFactory1 create = (CreateFactory1) GetProcAddress(dxgi, "CreateDXGIFactory1");
    void* factory = NULL;
    HRESULT hr = create ? create(&IID_IDXGIFactory1_, &factory) : E_NOINTERFACE;
    printf("CreateDXGIFactory1: 0x%08lx\n", (unsigned long) hr);
    fflush(stdout);
    // No Release or FreeLibrary: like a game closing, the process just ends
    ExitProcess(0);
}
