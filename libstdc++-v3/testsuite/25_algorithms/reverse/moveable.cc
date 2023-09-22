// { dg-do compile { target c++11 } }

// Copyright (C) 2005-2023 Free Software Foundation, Inc.
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

// 25.2.9 Reverse

#undef _GLIBCXX_CONCEPT_CHECKS

#include <algorithm>
#include <testsuite_iterators.h>

using __gnu_test::bidirectional_iterator_wrapper;

struct X 
{
  X() = delete;
  X(const X&) = delete;
  void operator=(const X&) = delete;
};

void
swap(X&, X&) { }

void
test1(bidirectional_iterator_wrapper<X>& begin, 
      bidirectional_iterator_wrapper<X>& end)
{ std::reverse(begin, end); }
