/* JeffPub 79 addition: lets the host application supply legacy codepage
 * conversion so libmspub does not need ICU. */
#ifndef INCLUDED_LIBMSPUB_JP_HOOKS_H
#define INCLUDED_LIBMSPUB_JP_HOOKS_H

#include <cstddef>
#include <string>

namespace libmspub
{
// Decode `len` bytes in `encoding` (e.g. "windows-1251") to UTF-32. Return false if unsupported.
typedef bool (*DecodeHook)(const char *encoding, const unsigned char *data, std::size_t len, std::u32string &out);
void setDecodeHook(DecodeHook hook);
}

#endif
