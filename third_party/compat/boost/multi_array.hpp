// Two-dimensional subset of boost::multi_array used by libmspub tables.
#pragma once
#include <cstddef>
#include <vector>
namespace boost {
struct extent_gen2 { std::size_t a = 0, b = 0; int n = 0;
    extent_gen2 operator[](std::size_t v) const { extent_gen2 e = *this; if (n == 0) e.a = v; else e.b = v; e.n = n + 1; return e; } };
inline const extent_gen2 extents{};
template <class T, std::size_t N> class multi_array;
template <class T> class multi_array<T, 2> {
public:
    explicit multi_array(const extent_gen2 &e) : m_shape{e.a, e.b}, m_data(e.a * e.b) {}
    T *operator[](std::size_t r) { return m_data.data() + r * m_shape[1]; }
    const T *operator[](std::size_t r) const { return m_data.data() + r * m_shape[1]; }
    const std::size_t *shape() const { return m_shape; }
private:
    std::size_t m_shape[2];
    std::vector<T> m_data;
};
}
