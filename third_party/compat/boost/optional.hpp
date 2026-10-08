// Minimal boost::optional on top of std::optional, enough for libmspub.
// Part of JeffPub's vendoring of libmspub without Boost.
#pragma once
#include <optional>
#include <utility>

namespace boost {

struct none_t {};
inline constexpr none_t none{};

template <class T>
class optional : public std::optional<T> {
public:
    using std::optional<T>::optional;
    optional() = default;
    optional(none_t) {}
    optional(const std::optional<T> &o) : std::optional<T>(o) {}
    optional &operator=(none_t) { this->reset(); return *this; }
    template <class U> optional &operator=(U &&v) { std::optional<T>::operator=(std::forward<U>(v)); return *this; }
    T &get() { return this->value(); }
    const T &get() const { return this->value(); }
    template <class U> T get_value_or(U &&def) const { return this->has_value() ? **this : static_cast<T>(std::forward<U>(def)); }
    bool is_initialized() const { return this->has_value(); }
    T *get_ptr() { return this->has_value() ? &**this : nullptr; }
    const T *get_ptr() const { return this->has_value() ? &**this : nullptr; }
};

template <class T> bool operator==(const optional<T> &a, const optional<T> &b)
{ return static_cast<const std::optional<T> &>(a) == static_cast<const std::optional<T> &>(b); }
template <class T> bool operator!=(const optional<T> &a, const optional<T> &b) { return !(a == b); }
template <class T> bool operator==(const optional<T> &a, none_t) { return !a.has_value(); }
template <class T> bool operator!=(const optional<T> &a, none_t) { return a.has_value(); }

template <class T> T &get(optional<T> &o) { return o.get(); }
template <class T> const T &get(const optional<T> &o) { return o.get(); }
template <class T> T *get(optional<T> *o) { return o ? o->get_ptr() : nullptr; }

} // namespace boost
