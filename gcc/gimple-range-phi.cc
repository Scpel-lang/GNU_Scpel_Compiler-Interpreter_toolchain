/* Gimple range phi analysis.
   Copyright (C) 2023 Free Software Foundation, Inc.
   Contributed by Andrew MacLeod <amacleod@redhat.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "insn-codes.h"
#include "tree.h"
#include "gimple.h"
#include "ssa.h"
#include "gimple-pretty-print.h"
#include "gimple-range.h"
#include "gimple-range-cache.h"
#include "value-range-storage.h"
#include "tree-cfg.h"
#include "target.h"
#include "attribs.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "cfganal.h"

// There can be only one running at a time.
static phi_analyzer *phi_analysis_object = NULL;

// Initialize a PHI analyzer with range query Q.

void
phi_analysis_initialize (range_query &q)
{
  gcc_checking_assert (!phi_analysis_object);
  phi_analysis_object = new phi_analyzer (q);
}

// Terminate the current PHI analyzer.  if F is non-null, dump the tables

void
phi_analysis_finalize ()
{
  gcc_checking_assert (phi_analysis_object);
  delete phi_analysis_object;
  phi_analysis_object = NULL;
}

// Return TRUE is there is a PHI analyzer operating.
bool
phi_analysis_available_p ()
{
  return phi_analysis_object != NULL;
}

// Return the phi analyzer object.

phi_analyzer &phi_analysis ()
{
  gcc_checking_assert (phi_analysis_object);
  return *phi_analysis_object;
}

// Initialize a phi_group from another group G.

phi_group::phi_group (const phi_group &g)
{
  m_group = g.m_group;
  m_initial_value = g.m_initial_value;
  m_initial_edge = g.m_initial_edge;
  m_modifier = g.m_modifier;
  m_modifier_op = g.m_modifier_op;
  m_vr = g.m_vr;
}

// Create a new phi_group with members BM, initialvalue INIT_VAL, modifier
// statement MOD, and resolve values using query Q.
// Calculate the range for the gropup if possible, otherwise set it to
// VARYING.

phi_group::phi_group (bitmap bm, tree init_val, edge e, gimple *mod,
		      range_query *q)
{
  // we dont expect a modifer and no inital value, so trap to have a look.
  // perhaps they are dead cycles and we can just used UNDEFINED.
  gcc_checking_assert (init_val);

  m_modifier_op = is_modifier_p (mod, bm);
  m_group = bm;
  m_initial_value = init_val;
  m_initial_edge = e;
  m_modifier = mod;
  if (q->range_on_edge (m_vr, m_initial_edge, m_initial_value))
    {
      // No modifier means the initial range is the full range.
      // Otherwise try to calculate a range.
      if (!m_modifier_op || calculate_using_modifier (q))
	return;
    }
  // Couldn't calculate a range, set to varying.
  m_vr.set_varying (TREE_TYPE (init_val));
}

// Return 0 if S is not a modifier statment for group members BM.
// If it could be a modifier, return which operand position (1 or 2)
// the phi member occurs in.
unsigned
phi_group::is_modifier_p (gimple *s, const bitmap bm)
{
  if (!s)
    return 0;
  gimple_range_op_handler handler (s);
  if (handler)
    {
      tree op1 = gimple_range_ssa_p (handler.operand1 ());
      tree op2 = gimple_range_ssa_p (handler.operand2 ());
      // Also disallow modifiers that have 2 ssa-names.
      if (op1 && !op2 && bitmap_bit_p (bm, SSA_NAME_VERSION (op1)))
	return 1;
      else if (op2 && !op1 && bitmap_bit_p (bm, SSA_NAME_VERSION (op2)))
	return 2;
    }
  return 0;
}

// Calulcate the range of the phi group using range_query Q.

bool
phi_group::calculate_using_modifier (range_query *q)
{
  // Look at the modifier for any relation
  relation_trio trio = fold_relations (m_modifier, q);
  relation_kind k = VREL_VARYING;
  if (m_modifier_op == 1)
    k = trio.lhs_op1 ();
  else if (m_modifier_op == 2)
    k = trio.lhs_op2 ();
  else
    return false;

  // If we can resolve the range using relations, use that range.
  if (refine_using_relation (k, q))
    return true;

 // If the initial value is undefined, do not calculate a range.
  if (m_vr.undefined_p ())
    return false;

  // Examine modifier and run X iterations to see if it convergences.
  // The constructor initilaized m_vr to the initial value already.
  int_range_max nv;
  for (unsigned x = 0; x< 10; x++)
    {
      if (!fold_range (nv, m_modifier, m_vr, q))
	return false;
      // If they are equal, then we have convergence.
      if (nv == m_vr)
	return true;
      // Update range and try again.
      m_vr.union_ (nv);
    }
  // Never converged, so bail for now. we could examine the pattern
  // from m_initial to m_vr as an extension  Especially if we had a way
  // to project the actual number of iterations (SCEV?)
  //
  //  We can also try to identify "parallel" phis to get loop counts and
  //  determine the number of iterations of these parallel PHIs.
  //
  return false;
}


// IF the modifier statement has a relation K between the modifier and the
// PHI member in it, we can project a range based on that.
// Use range_query Q to resolve values.
// ie,  a_2 = PHI <0, a_3>   and a_3 = a_2 + 1
// if the relation a_3 > a_2 is present, the know the range is [0, +INF]

bool
phi_group::refine_using_relation (relation_kind k, range_query *q)
{
  if (k == VREL_VARYING)
    return false;
  tree type = TREE_TYPE (m_initial_value);
  // If the type wraps, then relations dont tell us much.
  if (TYPE_OVERFLOW_WRAPS (type))
    return false;

  switch (k)
    {
    case VREL_LT:
    case VREL_LE:
      {
	// Value always decreases.
	int_range<2> lb;
	int_range<2> ub;
	if (!q->range_on_edge (ub, m_initial_edge, m_initial_value))
	  break;
	if (ub.undefined_p ())
	  return false;
	lb.set_varying (type);
	m_vr.set (type, lb.lower_bound (), ub.upper_bound ());
	return true;
      }

    case VREL_GT:
    case VREL_GE:
      {
	// Value always increases.
	int_range<2> lb;
	int_range<2> ub;
	if (!q->range_on_edge (lb, m_initial_edge, m_initial_value))
	  break;
	if (lb.undefined_p ())
	  return false;
	ub.set_varying (type);
	m_vr.set (type, lb.lower_bound (), ub.upper_bound ());
	return true;
      }

      // If its always equal, then its simply the initial value.
      // which is what m_vr has already been set to.
    case VREL_EQ:
      return true;

    default:
      break;
    }

  return false;
}

// Dump the information for a phi group to file F.

void
phi_group::dump (FILE *f)
{
  unsigned i;
  bitmap_iterator bi;
  fprintf (f, "PHI GROUP <");

  EXECUTE_IF_SET_IN_BITMAP (m_group, 0, i, bi)
    {
      print_generic_expr (f, ssa_name (i), TDF_SLIM);
      fputc (' ',f);
    }

  fprintf (f, ">\n - Initial value : ");
  if (m_initial_value)
    {
      if (TREE_CODE (m_initial_value) == SSA_NAME)
	print_gimple_stmt (f, SSA_NAME_DEF_STMT (m_initial_value), 0, TDF_SLIM);
      else
	print_generic_expr (f, m_initial_value, TDF_SLIM);
      fprintf (f, " on edge %d->%d", m_initial_edge->src->index,
	       m_initial_edge->dest->index);
    }
  else
    fprintf (f, "NONE");
  fprintf (f, "\n - Modifier : ");
  if (m_modifier)
    print_gimple_stmt (f, m_modifier, 0, TDF_SLIM);
  else
    fprintf (f, "NONE\n");
  fprintf (f, " - Range : ");
  m_vr.dump (f);
  fputc ('\n', f);
}

// -------------------------------------------------------------------------

// Construct a phi analyzer which uses range_query G to pick up values.

phi_analyzer::phi_analyzer (range_query &g) : m_global (g)
{
  m_work.create (0);
  m_work.safe_grow (20);

  m_tab.create (0);
//   m_tab.safe_grow_cleared (num_ssa_names + 100);
  bitmap_obstack_initialize (&m_bitmaps);
  m_simple = BITMAP_ALLOC (&m_bitmaps);
  m_current = BITMAP_ALLOC (&m_bitmaps);
}

// Destruct a PHI analyzer.

phi_analyzer::~phi_analyzer ()
{
  bitmap_obstack_release (&m_bitmaps);
  m_tab.release ();
  m_work.release ();
}

//  Return the group, if any, that NAME is part of.  Do no analysis.

phi_group *
phi_analyzer::group (tree name) const
{
  gcc_checking_assert (TREE_CODE (name) == SSA_NAME);
  if (!is_a<gphi *> (SSA_NAME_DEF_STMT (name)))
    return NULL;
  unsigned v = SSA_NAME_VERSION (name);
  if (v >= m_tab.length ())
    return NULL;
  return m_tab[v];
}

// Return the group NAME is associated with, if any.  If name has not been
// procvessed yet, do the analysis to determine if it is part of a group
// and return that.

phi_group *
phi_analyzer::operator[] (tree name)
{
  gcc_checking_assert (TREE_CODE (name) == SSA_NAME);

  //  Initial support for irange only.
  if (!irange::supports_p (TREE_TYPE (name)))
    return NULL;
  if (!is_a<gphi *> (SSA_NAME_DEF_STMT (name)))
    return NULL;

  unsigned v = SSA_NAME_VERSION (name);
  // Already been processed and not part of a group.
  if (bitmap_bit_p (m_simple, v))
    return NULL;

  if (v >= m_tab.length () || !m_tab[v])
    {
      process_phi (as_a<gphi *> (SSA_NAME_DEF_STMT (name)));
      if (bitmap_bit_p (m_simple, v))
	return  NULL;
      // If m_simple bit isn't set, then process_phi allocated the table
      // and should have a group.
      gcc_checking_assert (v < m_tab.length ());
    }
  return m_tab[v];
}

// Process phi node PHI to see if it it part of a group.

void
phi_analyzer::process_phi (gphi *phi)
{
  gcc_checking_assert (!group (gimple_phi_result (phi)));
  bool cycle_p = true;

  // Start with the LHS of the PHI in the worklist.
  unsigned x;
  m_work.truncate (0);
  m_work.safe_push (gimple_phi_result (phi));
  bitmap_clear (m_current);

  // We can only have 2 externals: an initial value and a modifier.
  // Any more than that and this fails to be a group.
  unsigned m_num_extern = 0;
  tree m_external[2];
  edge m_ext_edge[2];

  while (m_work.length () > 0)
    {
      tree phi_def = m_work.pop ();
      gphi *phi_stmt = as_a<gphi *> (SSA_NAME_DEF_STMT (phi_def));
      // if the phi is already in a cycle, its a complex situation, so revert
      // to simple.
      if (group (phi_def))
	{
	  cycle_p = false;
	  continue;
	}
      bitmap_set_bit (m_current, SSA_NAME_VERSION (phi_def));
      // Process the args.
      for (x = 0; x < gimple_phi_num_args (phi_stmt); x++)
	{
	  tree arg = gimple_phi_arg_def (phi_stmt, x);
	  if (arg == phi_def)
	    continue;
	  enum tree_code code = TREE_CODE (arg);
	  if (code == SSA_NAME)
	    {
	      unsigned v = SSA_NAME_VERSION (arg);
	      // Already a member of this potential group.
	      if (bitmap_bit_p (m_current, v))
		continue;
	      // Part of a different group ends cycle possibility.
	      if (group (arg) || bitmap_bit_p (m_simple, v))
		{
		  cycle_p = false;
		  break;
		}
	      // Check if its a PHI to examine.
	      // *FIX* Will miss initial values that originate from a PHI.
	      gimple *arg_stmt = SSA_NAME_DEF_STMT (arg);
	      if (arg_stmt && is_a<gphi *> (arg_stmt))
		{
		  m_work.safe_push (arg);
		  continue;
		}
	    }
	  // Other non-ssa names that arent constants are not understood
	  // and terminate analysis.
	  else if (code != INTEGER_CST && code != REAL_CST)
	    {
	      cycle_p = false;
	      continue;
	    }
	  // More than 2 outside names/CONST is too complicated.
	  if (m_num_extern >= 2)
	    {
	      cycle_p = false;
	      break;
	    }

	  m_external[m_num_extern] = arg;
	  m_ext_edge[m_num_extern++] = gimple_phi_arg_edge (phi_stmt, x);
	}
    }

  // If there are no names in the group, we're done.
  if (bitmap_empty_p (m_current))
    return;

  phi_group *g = NULL;
  if (cycle_p)
    {
      bool valid = true;
      gimple *mod = NULL;
      signed init_idx = -1;
      // At this point all the PHIs have been added to the bitmap.
      // the external list needs to be checked for initial values and modifiers.
      for (x = 0; x < m_num_extern; x++)
	{
	  tree name = m_external[x];
	  if (TREE_CODE (name) == SSA_NAME
	      && phi_group::is_modifier_p (SSA_NAME_DEF_STMT (name), m_current))
	    {
	      // Can't have multiple modifiers.
	      if (mod)
		valid = false;
	      mod = SSA_NAME_DEF_STMT (name);
	      continue;
	    }
	  // Can't have 2 initializers either.
	  if (init_idx != -1)
	    valid = false;
	  init_idx = x;
	}
      if (valid)
	{
	  // Try to create a group based on m_current. If a result comes back
	  // with a range that isn't varying, create the group.
	  phi_group cyc (m_current, m_external[init_idx],
			 m_ext_edge[init_idx], mod, &m_global);
	  if (!cyc.range ().varying_p ())
	    g = new phi_group (cyc);
	}
    }
  // If this dpoesn;t form a group, all members are instead simple phis.
  if (!g)
    {
      bitmap_ior_into (m_simple, m_current);
      return;
    }

  if (num_ssa_names >= m_tab.length ())
    m_tab.safe_grow_cleared (num_ssa_names + 100);

  // Now set all entries in the group to this record.
  unsigned i;
  bitmap_iterator bi;
  EXECUTE_IF_SET_IN_BITMAP (m_current, 0, i, bi)
    {
      // Can't be in more than one group.
      gcc_checking_assert (m_tab[i] == NULL);
      m_tab[i] = g;
    }
  // Allocate a new bitmap for the next time as the original one is now part
  // of the new phi group.
  m_current = BITMAP_ALLOC (&m_bitmaps);
}

void
phi_analyzer::dump (FILE *f)
{
  bool header = false;
  bitmap_clear (m_current);
  for (unsigned x = 0; x < m_tab.length (); x++)
    {
      if (bitmap_bit_p (m_simple, x))
	continue;
      if (bitmap_bit_p (m_current, x))
	continue;
      if (m_tab[x] == NULL)
	continue;
      phi_group *g = m_tab[x];
      bitmap_ior_into (m_current, g->group ());
      if (!header)
	{
	  header = true;
	  fprintf (dump_file, "\nPHI GROUPS:\n");
	}
      g->dump (f);
    }
}
