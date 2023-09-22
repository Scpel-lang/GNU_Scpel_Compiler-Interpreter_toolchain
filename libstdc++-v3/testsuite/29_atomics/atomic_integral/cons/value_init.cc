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

// { dg-do run { target c++20 } }
// { dg-require-thread-fence "" }

#include <atomic>
#include <testsuite_hooks.h>

constexpr std::atomic<int> a;

void
test01()
{
  VERIFY(a.load() == 0);
  static_assert(std::is_nothrow_default_constructible_v<std::atomic<int>>);
}

int
main()
{
  test01();
}
