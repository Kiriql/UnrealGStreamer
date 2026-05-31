#pragma once

#include <cstddef>

namespace GstUtils
{
    inline void SafeCopy(char* Dst, size_t DstSize, const char* Src)
    {
        if (!Dst || DstSize == 0) return;
        if (!Src) { Dst[0] = '\0'; return; }
        size_t i = 0;
        for (; i + 1 < DstSize && Src[i]; ++i) Dst[i] = Src[i];
        Dst[i] = '\0';
    }
}
