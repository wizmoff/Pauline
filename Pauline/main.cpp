#include <windows.h>
#include <wrl/client.h>
#include <iostream>
#include "pauline.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

int main() {
    if (checkDXR())
        std::cout << "DXR supported.";
    else
        std::cout << "DXR not supported.";
    return 0;
}