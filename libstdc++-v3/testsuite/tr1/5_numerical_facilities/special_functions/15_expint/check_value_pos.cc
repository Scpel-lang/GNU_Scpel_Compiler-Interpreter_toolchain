// 2007-02-04  Edward Smith-Rowland <3dw4rd@verizon.net>
//
// Copyright (C) 2007-2023 Free Software Foundation, Inc.
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

//  expint


//  Compare against values generated by the GNU Scientific Library.
//  The GSL can be found on the web: http://www.gnu.org/software/gsl/

#include <tr1/cmath>
#if defined(__TEST_DEBUG)
#include <iostream>
#define VERIFY(A) \
if (!(A)) \
  { \
    std::cout << "line " << __LINE__ \
      << "  max_abs_frac = " << max_abs_frac \
      << std::endl; \
  }
#else
#include <testsuite_hooks.h>
#endif
#include "../testcase.h"


// Test data.
testcase_expint<double> data001[] = {
  { 1.8951178163559366, 1.0000000000000000 },
  { 4.9542343560018907, 2.0000000000000000 },
  { 9.9338325706254160, 3.0000000000000000 },
  { 19.630874470056217, 4.0000000000000000 },
  { 40.185275355803178, 5.0000000000000000 },
  { 85.989762142439204, 6.0000000000000000 },
  { 191.50474333550139, 7.0000000000000000 },
  { 440.37989953483827, 8.0000000000000000 },
  { 1037.8782907170896, 9.0000000000000000 },
  { 2492.2289762418782, 10.000000000000000 },
  { 6071.4063740986112, 11.000000000000000 },
  { 14959.532666397527, 12.000000000000000 },
  { 37197.688490689041, 13.000000000000000 },
  { 93192.513633965369, 14.000000000000000 },
  { 234955.85249076830, 15.000000000000000 },
  { 595560.99867083703, 16.000000000000000 },
  { 1516637.8940425171, 17.000000000000000 },
  { 3877904.3305974435, 18.000000000000000 },
  { 9950907.2510468438, 19.000000000000000 },
  { 25615652.664056588, 20.000000000000000 },
  { 66127186.355484918, 21.000000000000000 },
  { 171144671.30036369, 22.000000000000000 },
  { 443966369.83027118, 23.000000000000000 },
  { 1154115391.8491828, 24.000000000000000 },
  { 3005950906.5255489, 25.000000000000000 },
  { 7842940991.8981876, 26.000000000000000 },
  { 20496497119.880810, 27.000000000000000 },
  { 53645118592.314682, 28.000000000000000 },
  { 140599195758.40689, 29.000000000000000 },
  { 368973209407.27423, 30.000000000000000 },
  { 969455575968.39392, 31.000000000000000 },
  { 2550043566357.7866, 32.000000000000000 },
  { 6714640184076.4980, 33.000000000000000 },
  { 17698037244116.266, 34.000000000000000 },
  { 46690550144661.594, 35.000000000000000 },
  { 123285207991209.75, 36.000000000000000 },
  { 325798899867226.44, 37.000000000000000 },
  { 861638819996578.62, 38.000000000000000 },
  { 2280446200301902.5, 39.000000000000000 },
  { 6039718263611242.0, 40.000000000000000 },
  { 16006649143245042., 41.000000000000000 },
  { 42447960921368504., 42.000000000000000 },
  { 1.1263482901669667e+17, 43.000000000000000 },
  { 2.9904447186323366e+17, 44.000000000000000 },
  { 7.9439160357044531e+17, 45.000000000000000 },
  { 2.1113423886478239e+18, 46.000000000000000 },
  { 5.6143296808103434e+18, 47.000000000000000 },
  { 1.4936302131129930e+19, 48.000000000000000 },
  { 3.9754427479037444e+19, 49.000000000000000 },
  { 1.0585636897131690e+20, 50.000000000000000 },
};

// Test function.
template <typename Tp>
void test001()
{
  const Tp eps = std::numeric_limits<Tp>::epsilon();
  Tp max_abs_diff = -Tp(1);
  Tp max_abs_frac = -Tp(1);
  unsigned int num_datum = sizeof(data001)
                         / sizeof(testcase_expint<double>);
  for (unsigned int i = 0; i < num_datum; ++i)
    {
      const Tp f = std::tr1::expint(Tp(data001[i].x));
      const Tp f0 = data001[i].f0;
      const Tp diff = f - f0;
      if (std::abs(diff) > max_abs_diff)
        max_abs_diff = std::abs(diff);
      if (std::abs(f0) > Tp(10) * eps
       && std::abs(f) > Tp(10) * eps)
        {
          const Tp frac = diff / f0;
          if (std::abs(frac) > max_abs_frac)
            max_abs_frac = std::abs(frac);
        }
    }
  VERIFY(max_abs_frac < Tp(2.5000000000000020e-13));
}

int main(int, char**)
{
  test001<double>();
  return 0;
}
