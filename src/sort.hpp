#ifndef SORT_HPP
#define SORT_HPP

#include "internal.hpp"

namespace osh {

template <typename T>
LOs sort_by_keys(Read<T> keys, Int width = 1);

#define INST_DECL(T) extern template LOs sort_by_keys(Read<T> keys, Int width);
INST_DECL(LO)
INST_DECL(GO)
#undef INST_DECL

}  // end namespace osh

#endif