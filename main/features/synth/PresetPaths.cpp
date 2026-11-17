#include "PresetPaths.h"
#include "SafeStart.h"

#include <cstdio>
#include <cstring>

namespace presetpaths
{
namespace
{
constexpr const char *kUnnamedPath = "/littlefs/presets/current.json";
constexpr const char *kMarkerPath = "/littlefs/presets/.last";
} // namespace

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
        return false; // reserved for marker/internal files
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
    snprintf(out, outSize, "%s/%s.json", kDir, name);
}

const char *unnamedPath()
{
    return kUnnamedPath;
}

bool readLastMarker(char *outName, size_t outSize)
{
    outName[0] = '\0';
    FILE *f = fopen(kMarkerPath, "rb");
    if (f == nullptr)
    {
        return false;
    }
    const size_t n = fread(outName, 1, outSize - 1, f);
    fclose(f);
    outName[n] = '\0';

    // Trim trailing whitespace/newline (the file is written without one,
    // but be defensive against manual edits).
    size_t trimmed = n;
    while (trimmed > 0 && (outName[trimmed - 1] == '\n' || outName[trimmed - 1] == '\r' || outName[trimmed - 1] == ' '))
    {
        outName[--trimmed] = '\0';
    }

    return isSafeName(outName);
}

void writeLastMarker(const char *name)
{
    safestart::leave(); // a deliberate preset selection ends a safe start
    FILE *f = fopen(kMarkerPath, "wb");
    if (f == nullptr)
    {
        return;
    }
    if (name != nullptr && name[0] != '\0')
    {
        fwrite(name, 1, strlen(name), f);
    }
    fclose(f);
}

void resolveActivePath(char *outPath, size_t outPathSize, char *outName, size_t outNameSize)
{
    char marker[kMaxNameLen] = {};
    if (safestart::active())
    {
        // Nothing is loaded and nothing is overwritten: an empty scratch slot is active.
        snprintf(outPath, outPathSize, "%s", safestart::scratchPath());
        outName[0] = '\0';
    }
    else if (readLastMarker(marker, sizeof(marker)))
    {
        pathForName(marker, outPath, outPathSize);
        snprintf(outName, outNameSize, "%s", marker);
    }
    else
    {
        snprintf(outPath, outPathSize, "%s", kUnnamedPath);
        outName[0] = '\0';
    }
}

namespace
{
int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

size_t urlDecode(const char *encoded, char *out, size_t outSize)
{
    if (outSize == 0)
    {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; encoded[i] != '\0' && o + 1 < outSize; ++i)
    {
        char c = encoded[i];
        if (c == '+')
        {
            out[o++] = ' ';
        }
        else if (c == '%' && encoded[i + 1] != '\0' && encoded[i + 2] != '\0')
        {
            const int hi = hexNibble(encoded[i + 1]);
            const int lo = hexNibble(encoded[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out[o++] = static_cast<char>((hi << 4) | lo);
                i += 2;
            }
            else
            {
                out[o++] = c;
            }
        }
        else
        {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

} // namespace presetpaths
