// Test fixture: a tiny game-like executable whose import table names winmm.dll
// (built twice: normal import, and /DELAYLOAD:winmm.dll). Never run by the tests,
// only read by the installer's PE import scan.
#include <windows.h>
#include <mmsystem.h>

int main() {
    return static_cast<int>(timeGetTime() & 1u);
}
