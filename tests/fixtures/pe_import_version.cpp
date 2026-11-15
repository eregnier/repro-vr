// Test fixture: a tiny game-like executable whose import table names version.dll.
// Never run by the tests, only read by the installer's PE import scan.
#include <windows.h>

int main() {
    DWORD handle = 0;
    return static_cast<int>(GetFileVersionInfoSizeW(L"kernel32.dll", &handle) & 1u);
}
