/* Preamble and helpers for the autogenerated generic-match.cc file.
   Copyright (C) 2014-2024 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "ssa.h"
#include "cgraph.h"
#include "vec-perm-indices.h"
#include "fold-const.h"
#include "fold-const-call.h"
#include "stor-layout.h"
#include "tree-dfa.h"
#include "builtins.h"
#include "case-cfn-macros.h"
#include "gimplify.h"
#include "optabs-tree.h"
#include "dbgcnt.h"
#include "tm.h"
#include "tree-eh.h"
#include "langhooks.h"
#include "tree-pass.h"
#include "attribs.h"
#include "asan.h"

/* Routine to determine if the types T1 and T2 are effectively
   the same for GENERIC.  If T1 or T2 is not a type, the test
   applies to their TREE_TYPE.  */

static inline bool
types_match (tree t1, tree t2)
{
  if (!TYPE_P (t1))
    t1 = TREE_TYPE (t1);
  if (!TYPE_P (t2))
    t2 = TREE_TYPE (t2);

  return TYPE_MAIN_VARIANT (t1) == TYPE_MAIN_VARIANT (t2);
}

/* Return if T has a single use.  For GENERIC, we assume this is
   always true.  */

static inline bool
single_use (tree t ATTRIBUTE_UNUSED)
{
  return true;
}

/* Return true if math operations should be canonicalized,
   e.g. sqrt(sqrt(x)) -> pow(x, 0.25).  */

static inline bool
canonicalize_math_p ()
{
  return !cfun || (cfun->curr_properties & PROP_gimple_opt_math) == 0;
}

/* Return true if math operations that are beneficial only after
   vectorization should be canonicalized.  */

static inline bool
canonicalize_math_after_vectorization_p ()
{
  return false;
}

/* Return true if we can still perform transformations that may introduce
   vector operations that are not supported by the target. Vector lowering
   normally handles those, but after that pass, it becomes unsafe.  */

static inline bool
optimize_vectors_before_lowering_p ()
{
  return !cfun || (cfun->curr_properties & PROP_gimple_lvec) == 0;
}

/* Return true if successive divisions can be optimized.
   Defer to GIMPLE opts.  */

static inline bool
optimize_successive_divisions_p (tree, tree)
{
  return false;
}

/* Return true if EXPR1 and EXPR2 have the same value, but not necessarily
   same type.  The types can differ through nop conversions.  */

static inline bool
bitwise_equal_p (tree expr1, tree expr2)
{
  STRIP_NOPS (expr1);
  STRIP_NOPS (expr2);
  if (expr1 == expr2)
    return true;
  if (!tree_nop_conversion_p (TREE_TYPE (expr1), TREE_TYPE (expr2)))
    return false;
  if (TREE_CODE (expr1) == INTEGER_CST && TREE_CODE (expr2) == INTEGER_CST)
    return wi::to_wide (expr1) == wi::to_wide (expr2);
  return operand_equal_p (expr1, expr2, 0);
}

/* Return true if EXPR1 and EXPR2 have the bitwise opposite value,
   but not necessarily same type.
   The types can differ through nop conversions.  */

static inline bool
bitwise_inverted_equal_p (tree expr1, tree expr2, bool &wascmp)
{
  STRIP_NOPS (expr1);
  STRIP_NOPS (expr2);
  wascmp = false;
  if (expr1 == expr2)
    return false;
  if (!tree_nop_conversion_p (TREE_TYPE (expr1), TREE_TYPE (expr2)))
    return false;
  if (TREE_CODE (expr1) == INTEGER_CST && TREE_CODE (expr2) == INTEGER_CST)
    return wi::to_wide (expr1) == ~wi::to_wide (expr2);
  if (operand_equal_p (expr1, expr2, 0))
    return false;
  if (TREE_CODE (expr1) == BIT_NOT_EXPR
      && bitwise_equal_p (TREE_OPERAND (expr1, 0), expr2))
    return true;
  if (TREE_CODE (expr2) == BIT_NOT_EXPR
      && bitwise_equal_p (expr1, TREE_OPERAND (expr2, 0)))
    return true;
  if (COMPARISON_CLASS_P (expr1)
      && COMPARISON_CLASS_P (expr2))
    {
      tree op10 = TREE_OPERAND (expr1, 0);
      tree op20 = TREE_OPERAND (expr2, 0);
      wascmp = true;
      if (!operand_equal_p (op10, op20))
	return false;
      tree op11 = TREE_OPERAND (expr1, 1);
      tree op21 = TREE_OPERAND (expr2, 1);
      if (!operand_equal_p (op11, op21))
	return false;
      if (invert_tree_comparison (TREE_CODE (expr1),
				  HONOR_NANS (op10))
	  == TREE_CODE (expr2))
	return true;
    }
  return false;
}
