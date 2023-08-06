/* Copyright (C) 2004-2023 Free Software Foundation, Inc.

This file is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This file is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */


/* Calculate division table for ST40-300 integer division
   Contributed by Joern Rennecke
   joern.rennecke@st.com  */

#include <stdio.h>
#include <math.h>

int
main ()
{
  int i, j;
  double q, r, err, max_err = 0, max_s_err = 0;

  puts("/* This table has been generated by divtab-sh4.cc.  */");
  puts ("\t.balign 4");
  for (i = -128; i < 128; i++)
    {
      int n = 0;
      if (i == 0)
	{
	  /* output some dummy number for 1/0.  */
	  puts ("LOCAL(div_table_clz):\n\t.byte\t0");
	  continue;
	}
      for (j = i < 0 ? -i : i; j < 128; j += j)
	n++;
      printf ("\t.byte\t%d\n", n - 7);
    }
  puts("\
/* 1/-128 .. 1/127, normalized.  There is an implicit leading 1 in bit 32,\n\
   or in bit 33 for powers of two.  */\n\
	.balign 4");
  for (i = -128; i < 128; i++)
    {
      if (i == 0)
	{
	  puts ("LOCAL(div_table_inv):\n\t.long\t0x0");
	  continue;
	}
      j = i < 0 ? -i : i;
      while (j < 64)
	j += j;
      q = 4.*(1<<30)*128/j;
      r = ceil (q);
      printf ("\t.long\t0x%X\n", (unsigned) r);
      err = r - q;
      if (err > max_err)
	max_err = err;
      err = err * j / 128;
      if (err > max_s_err)
	max_s_err = err;
    }
  printf ("\t/* maximum error: %f scaled: %f*/\n", max_err, max_s_err);
  exit (0);
}
