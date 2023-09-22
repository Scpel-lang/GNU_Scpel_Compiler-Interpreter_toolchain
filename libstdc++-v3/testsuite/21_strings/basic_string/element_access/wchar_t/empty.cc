// Copyright (C) 2004-2023 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.
//

#include <string>
#include <testsuite_hooks.h>

// as per 21.3.4
int main()
{
  {
    std::wstring empty;
    wchar_t c = empty[0];
    VERIFY( c == wchar_t() );
  }

  {
    const std::wstring empty;
    wchar_t c = empty[0];
    VERIFY( c == wchar_t() );
  }
  return 0;
}
