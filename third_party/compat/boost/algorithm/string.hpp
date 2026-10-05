#pragma once
#include <algorithm>
#include <cctype>
#include <string>
namespace boost {
inline void trim(std::string &s)
{
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
}
}
