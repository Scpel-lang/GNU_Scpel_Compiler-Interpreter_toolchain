// Copyright (C) 2008-2023 Free Software Foundation, Inc.
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

// { dg-do link }

#include <ext/stdio_sync_filebuf.h>

struct my_ssf
: __gnu_cxx::stdio_sync_filebuf<wchar_t>
{
  my_ssf(std::__c_file* __f)
  : __gnu_cxx::stdio_sync_filebuf<wchar_t>(__f) { }
};

// libstdc++/35209
void test01()
{
  my_ssf ss1(0);
}

int main()
{
  test01();
  return 0;
}
