// Copyright (C) 2020-2023 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

// { dg-do compile { target c++20 } }

#include <string_view>

#ifndef __cpp_lib_constexpr_string_view
# error "Feature test macro for constexpr copy is missing in <string_view>"
#elif __cpp_lib_constexpr_string_view < 201811L
# error "Feature test macro for constexpr copy has wrong value in <string_view>"
#endif

constexpr bool
test01()
{
  std::string_view s = "Everything changes and nothing stands still.";
  char buf[7]{};
  auto n = s.copy(buf, 7, 11);
  return std::string_view(buf, n) == "changes";
}

static_assert( test01() );
