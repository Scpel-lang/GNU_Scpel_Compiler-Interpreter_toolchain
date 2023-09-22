// { dg-do run { target c++11 } }
// { dg-options "-D__STDCPP_WANT_MATH_SPEC_FUNCS__" }
// { dg-skip-if "no extensions in strict dialects" { *-*-* } { "-std=c++*" } }
//
// Copyright (C) 2016-2023 Free Software Foundation, Inc.
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

//  airy_bi

//  Compare against values generated by the GNU Scientific Library.
//  The GSL can be found on the web: http://www.gnu.org/software/gsl/
#include <limits>
#include <cmath>
#if defined(__TEST_DEBUG)
#  include <iostream>
#  define VERIFY(A) \
  if (!(A)) \
    { \
      std::cout << "line " << __LINE__ \
	<< "  max_abs_frac = " << max_abs_frac \
	<< std::endl; \
    }
#else
#  include <testsuite_hooks.h>
#endif
#include <specfun_testcase.h>

// Test data.
// max(|f - f_GSL|): 3.2782554626464844e-06 at index 40
// max(|f - f_GSL| / |f_GSL|): 1.3623770134694109e-13
// mean(f - f_GSL): -8.0102555317125191e-08
// variance(f - f_GSL): 2.6209716422814286e-13
// stddev(f - f_GSL): 5.1195425989842373e-07
const testcase_airy_bi<double>
data001[41] =
{
  { -0.31467982964383845, -10.000000000000000, 0.0 },
  { 0.037785432489467467, -9.5000000000000000, 0.0 },
  { 0.32494732345524480, -9.0000000000000000, 0.0 },
  { 0.0077544364476580746, -8.5000000000000000, 0.0 },
  { -0.33125158075113792, -8.0000000000000000, 0.0 },
  { -0.11246348507649087, -7.5000000000000000, 0.0 },
  { 0.29376207185441372, -7.0000000000000000, 0.0 },
  { 0.26101265763648318, -6.5000000000000000, 0.0 },
  { -0.14669837667055663, -6.0000000000000000, 0.0 },
  { -0.36781345391571185, -5.5000000000000000, 0.0 },
  { -0.13836913490160088, -5.0000000000000000, 0.0 },
  { 0.25387265769693296, -4.5000000000000000, 0.0 },
  { 0.39223470570699931, -4.0000000000000000, 0.0 },
  { 0.16893983748105870, -3.5000000000000000, 0.0 },
  { -0.19828962637492650, -3.0000000000000000, 0.0 },
  { -0.43242247184070520, -2.5000000000000000, 0.0 },
  { -0.41230258795639835, -2.0000000000000000, 0.0 },
  { -0.19178486115704119, -1.5000000000000000, 0.0 },
  { 0.10399738949694459, -1.0000000000000000, 0.0 },
  { 0.38035265975105381, -0.50000000000000000, 0.0 },
  { 0.61492662744600068, 0.0000000000000000, 0.0 },
  { 0.85427704310315555, 0.50000000000000000, 0.0 },
  { 1.2074235949528713, 1.0000000000000000, 0.0 },
  { 1.8789415037478949, 1.5000000000000000, 0.0 },
  { 3.2980949999782148, 2.0000000000000000, 0.0 },
  { 6.4816607384605804, 2.5000000000000000, 0.0 },
  { 14.037328963730236, 3.0000000000000000, 0.0 },
  { 33.055506754611478, 3.5000000000000000, 0.0 },
  { 83.847071408468111, 4.0000000000000000, 0.0 },
  { 227.58808183559950, 4.5000000000000000, 0.0 },
  { 657.79204417117160, 5.0000000000000000, 0.0 },
  { 2016.5800386595315, 5.5000000000000000, 0.0 },
  { 6536.4461048098583, 6.0000000000000000, 0.0 },
  { 22340.607718396990, 6.5000000000000000, 0.0 },
  { 80327.790709430337, 7.0000000000000000, 0.0 },
  { 303229.61511253362, 7.5000000000000000, 0.0 },
  { 1199586.0041244617, 8.0000000000000000, 0.0 },
  { 4965319.5414712988, 8.5000000000000000, 0.0 },
  { 21472868.891435351, 9.0000000000000000, 0.0 },
  { 96892265.580451161, 9.5000000000000000, 0.0 },
  { 455641153.54822654, 10.000000000000000, 0.0 },
};
const double toler001 = 1.0000000000000006e-11;

template<typename Ret, unsigned int Num>
  void
  test(const testcase_airy_bi<Ret> (&data)[Num], Ret toler)
  {
    bool test __attribute__((unused)) = true;
    const Ret eps = std::numeric_limits<Ret>::epsilon();
    Ret max_abs_diff = -Ret(1);
    Ret max_abs_frac = -Ret(1);
    unsigned int num_datum = Num;
    for (unsigned int i = 0; i < num_datum; ++i)
      {
	const Ret f = __gnu_cxx::airy_bi(data[i].x);
	const Ret f0 = data[i].f0;
	const Ret diff = f - f0;
	if (std::abs(diff) > max_abs_diff)
	  max_abs_diff = std::abs(diff);
	if (std::abs(f0) > Ret(10) * eps
	 && std::abs(f) > Ret(10) * eps)
	  {
	    const Ret frac = diff / f0;
	    if (std::abs(frac) > max_abs_frac)
	      max_abs_frac = std::abs(frac);
	  }
      }
    VERIFY(max_abs_frac < toler);
  }

int
main()
{
  test(data001, toler001);
  return 0;
}
