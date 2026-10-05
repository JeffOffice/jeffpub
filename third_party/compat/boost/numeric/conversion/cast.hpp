#pragma once
#include <limits>
#include <stdexcept>
#include <type_traits>
namespace boost {
struct bad_numeric_cast : std::bad_cast {};
template <class Target, class Source>
Target numeric_cast(Source v)
{
    if constexpr (std::is_integral_v<Source> && std::is_integral_v<Target>) {
        if constexpr (std::is_signed_v<Source>) {
            if (v < 0 && !std::is_signed_v<Target>) throw bad_numeric_cast();
        }
        if (static_cast<unsigned long long>(v < 0 ? 0 : v) > static_cast<unsigned long long>(std::numeric_limits<Target>::max())) throw bad_numeric_cast();
    }
    return static_cast<Target>(v);
}
}
