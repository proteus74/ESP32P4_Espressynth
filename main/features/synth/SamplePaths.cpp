#include "SamplePaths.h"

#include <cstdio>
#include <cstring>

namespace samplepaths
{

bool isSafeName(const char *name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }
    const size_t len = strlen(name);
    if (len >= kMaxNameLen)
    {
        return false;
    }
    if (name[0] == '.')
    {
        return false;
    }
    if (strstr(name, "..") != nullptr)
    {
        return false;
    }
    for (size_t i = 0; i < len; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c == '/' || c == '\\' || c < 0x20)
        {
            return false;
        }
    }
    return true;
}

void pathForName(const char *name, char *out, size_t outSize)
{
    snprintf(out, outSize, "%s/%s", kDir, name);
}

} // namespace samplepaths
