/* Subclasses of diagnostic_event for analyzer diagnostics. */

#include "config.h"
#define INCLUDE_MEMORY
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "function.h"
#include "basic-block.h"
#include "gimple.h"
#include "diagnostic-core.h"
#include "gimple-pretty-print.h"
#include "fold-const.h"
#include "diagnostic-path.h"
#include "options.h"
#include "cgraph.h"
#include "cfg.h"
#include "digraph.h"
#include "diagnostic-event-id.h"
#include "analyzer/analyzer.h"
#include "analyzer/analyzer-logging.h"
#include "analyzer/sm.h"
#include "sbitmap.h"
#include "bitmap.h"
#include "ordered-hash-map.h"
#include "analyzer/call-string.h"
#include "analyzer/program-point.h"
#include "analyzer/store.h"
#include "analyzer/region-model.h"
#include "analyzer/program-state.h"
#include "analyzer/checker-path.h"
#include "gimple-iterator.h"
#include "inlining-iterator.h"
#include "analyzer/supergraph.h"
#include "analyzer/pending-diagnostic.h"
#include "analyzer/diagnostic-manager.h"
#include "analyzer/constraint-manager.h"
#include "analyzer/checker-event.h"
#include "analyzer/exploded-graph.h"

#if ENABLE_ANALYZER

namespace ana {

/* Get a string for EK.  */

const char *
event_kind_to_string (enum event_kind ek)
{
  switch (ek)
    {
    default:
      gcc_unreachable ();
    case EK_DEBUG:
      return "EK_DEBUG";
    case EK_CUSTOM:
      return "EK_CUSTOM";
    case EK_STMT:
      return "EK_STMT";
    case EK_REGION_CREATION:
      return "EK_REGION_CREATION";
    case EK_FUNCTION_ENTRY:
      return "EK_FUNCTION_ENTRY";
    case EK_STATE_CHANGE:
      return "EK_STATE_CHANGE";
    case EK_START_CFG_EDGE:
      return "EK_START_CFG_EDGE";
    case EK_END_CFG_EDGE:
      return "EK_END_CFG_EDGE";
    case EK_CALL_EDGE:
      return "EK_CALL_EDGE";
    case EK_RETURN_EDGE:
      return "EK_RETURN_EDGE";
    case EK_START_CONSOLIDATED_CFG_EDGES:
      return "EK_START_CONSOLIDATED_CFG_EDGES";
    case EK_END_CONSOLIDATED_CFG_EDGES:
      return "EK_END_CONSOLIDATED_CFG_EDGES";
    case EK_INLINED_CALL:
      return "EK_INLINED_CALL";
    case EK_SETJMP:
      return "EK_SETJMP";
    case EK_REWIND_FROM_LONGJMP:
      return "EK_REWIND_FROM_LONGJMP";
    case EK_REWIND_TO_SETJMP:
      return "EK_REWIND_TO_SETJMP";
    case EK_WARNING:
      return "EK_WARNING";
    }
}

/* A class for fixing up fndecls and stack depths in checker_event, based
   on inlining records.

   The early inliner runs before the analyzer, which can lead to confusing
   output.

   Tne base fndecl and depth within a checker_event are from call strings
   in program_points, which reflect the call strings after inlining.
   This class lets us offset the depth and fix up the reported fndecl and
   stack depth to better reflect the user's original code.  */

class inlining_info
{
public:
  inlining_info (location_t loc)
  {
    inlining_iterator iter (loc);
    m_inner_fndecl = iter.get_fndecl ();
    int num_frames = 0;
    while (!iter.done_p ())
      {
	m_outer_fndecl = iter.get_fndecl ();
	num_frames++;
	iter.next ();
      }
    if (num_frames > 1)
      m_extra_frames = num_frames - 1;
    else
      m_extra_frames = 0;
  }

  tree get_inner_fndecl () const { return m_inner_fndecl; }
  int get_extra_frames () const { return m_extra_frames; }

private:
  tree m_outer_fndecl;
  tree m_inner_fndecl;
  int m_extra_frames;
};

/* class checker_event : public diagnostic_event.  */

/* checker_event's ctor.  */

checker_event::checker_event (enum event_kind kind,
			      const event_loc_info &loc_info)
: m_kind (kind), m_loc (loc_info.m_loc),
  m_original_fndecl (loc_info.m_fndecl),
  m_effective_fndecl (loc_info.m_fndecl),
  m_original_depth (loc_info.m_depth),
  m_effective_depth (loc_info.m_depth),
  m_pending_diagnostic (NULL), m_emission_id (),
  m_logical_loc (loc_info.m_fndecl)
{
  /* Update effective fndecl and depth if inlining has been recorded.  */
  if (flag_analyzer_undo_inlining)
    {
      inlining_info info (m_loc);
      if (info.get_inner_fndecl ())
	{
	  m_effective_fndecl = info.get_inner_fndecl ();
	  m_effective_depth += info.get_extra_frames ();
	  m_logical_loc = tree_logical_location (m_effective_fndecl);
	}
    }
}

/* No-op implementation of diagnostic_event::get_meaning vfunc for
   checker_event: checker events have no meaning by default.  */

diagnostic_event::meaning
checker_event::get_meaning () const
{
  return meaning ();
}

/* Dump this event to PP (for debugging/logging purposes).  */

void
checker_event::dump (pretty_printer *pp) const
{
  label_text event_desc (get_desc (false));
  pp_printf (pp, "\"%s\" (depth %i",
	     event_desc.get (), m_effective_depth);

  if (m_effective_depth != m_original_depth)
    pp_printf (pp, " corrected from %i",
	       m_original_depth);
  if (m_effective_fndecl)
    {
      pp_printf (pp, ", fndecl %qE", m_effective_fndecl);
      if (m_effective_fndecl != m_original_fndecl)
	pp_printf (pp, " corrected from %qE", m_original_fndecl);
    }
  pp_printf (pp, ", m_loc=%x)",
	     get_location ());
}

/* Dump this event to stderr (for debugging/logging purposes).  */

DEBUG_FUNCTION void
checker_event::debug () const
{
  pretty_printer pp;
  pp_format_decoder (&pp) = default_tree_printer;
  pp_show_color (&pp) = pp_show_color (global_dc->printer);
  pp.buffer->stream = stderr;
  dump (&pp);
  pp_newline (&pp);
  pp_flush (&pp);
}

/* Hook for being notified when this event has its final id EMISSION_ID
   and is about to emitted for PD.

   Base implementation of checker_event::prepare_for_emission vfunc;
   subclasses that override this should chain up to it.

   Record PD and EMISSION_ID, and call the get_desc vfunc, so that any
   side-effects of the call to get_desc take place before
   pending_diagnostic::emit is called.

   For example, state_change_event::get_desc can call
   pending_diagnostic::describe_state_change; free_of_non_heap can use this
   to tweak the message (TODO: would be neater to simply capture the
   pertinent data within the sm-state).  */

void
checker_event::prepare_for_emission (checker_path *,
				     pending_diagnostic *pd,
				     diagnostic_event_id_t emission_id)
{
  m_pending_diagnostic = pd;
  m_emission_id = emission_id;

  label_text desc = get_desc (false);
}

/* class debug_event : public checker_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   debug_event.
   Use the saved string as the event's description.  */

label_text
debug_event::get_desc (bool) const
{
  return label_text::borrow (m_desc);
}

/* class precanned_custom_event : public custom_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   precanned_custom_event.
   Use the saved string as the event's description.  */

label_text
precanned_custom_event::get_desc (bool) const
{
  return label_text::borrow (m_desc);
}

/* class statement_event : public checker_event.  */

/* statement_event's ctor.  */

statement_event::statement_event (const gimple *stmt, tree fndecl, int depth,
				  const program_state &dst_state)
: checker_event (EK_STMT,
		 event_loc_info (gimple_location (stmt), fndecl, depth)),
  m_stmt (stmt),
  m_dst_state (dst_state)
{
}

/* Implementation of diagnostic_event::get_desc vfunc for
   statement_event.
   Use the statement's dump form as the event's description.  */

label_text
statement_event::get_desc (bool) const
{
  pretty_printer pp;
  pp_string (&pp, "stmt: ");
  pp_gimple_stmt_1 (&pp, m_stmt, 0, (dump_flags_t)0);
  return label_text::take (xstrdup (pp_formatted_text (&pp)));
}

/* class region_creation_event : public checker_event.  */

region_creation_event::region_creation_event (const event_loc_info &loc_info)
: checker_event (EK_REGION_CREATION, loc_info)
{
}

/* The various region_creation_event subclasses' get_desc
   implementations.  */

label_text
region_creation_event_memory_space::get_desc (bool) const
{
  switch (m_mem_space)
    {
    default:
      return label_text::borrow ("region created here");
    case MEMSPACE_STACK:
      return label_text::borrow ("region created on stack here");
    case MEMSPACE_HEAP:
      return label_text::borrow ("region created on heap here");
    }
}

label_text
region_creation_event_capacity::get_desc (bool can_colorize) const
{
  gcc_assert (m_capacity);
  if (TREE_CODE (m_capacity) == INTEGER_CST)
    {
      unsigned HOST_WIDE_INT hwi = tree_to_uhwi (m_capacity);
      return make_label_text_n (can_colorize,
				hwi,
				"capacity: %wu byte",
				"capacity: %wu bytes",
				hwi);
    }
  else
    return make_label_text (can_colorize,
			    "capacity: %qE bytes", m_capacity);
}

label_text
region_creation_event_allocation_size::get_desc (bool can_colorize) const
{
  if (m_capacity)
    {
      if (TREE_CODE (m_capacity) == INTEGER_CST)
	return make_label_text_n (can_colorize,
				  tree_to_uhwi (m_capacity),
				  "allocated %E byte here",
				  "allocated %E bytes here",
				  m_capacity);
      else
	return make_label_text (can_colorize,
				"allocated %qE bytes here",
				m_capacity);
    }
  return make_label_text (can_colorize, "allocated here");
}

label_text
region_creation_event_debug::get_desc (bool) const
{
  pretty_printer pp;
  pp_format_decoder (&pp) = default_tree_printer;
  pp_string (&pp, "region creation: ");
  m_reg->dump_to_pp (&pp, true);
  if (m_capacity)
    pp_printf (&pp, " capacity: %qE", m_capacity);
  return label_text::take (xstrdup (pp_formatted_text (&pp)));
}

/* class function_entry_event : public checker_event.  */

function_entry_event::function_entry_event (const program_point &dst_point)
: checker_event (EK_FUNCTION_ENTRY,
		 event_loc_info (dst_point.get_supernode
				   ()->get_start_location (),
				 dst_point.get_fndecl (),
				 dst_point.get_stack_depth ()))
{
}

/* Implementation of diagnostic_event::get_desc vfunc for
   function_entry_event.

   Use a string such as "entry to 'foo'" as the event's description.  */

label_text
function_entry_event::get_desc (bool can_colorize) const
{
  return make_label_text (can_colorize, "entry to %qE", m_effective_fndecl);
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   function entry.  */

diagnostic_event::meaning
function_entry_event::get_meaning () const
{
  return meaning (VERB_enter, NOUN_function);
}

/* class state_change_event : public checker_event.  */

/* state_change_event's ctor.  */

state_change_event::state_change_event (const supernode *node,
					const gimple *stmt,
					int stack_depth,
					const state_machine &sm,
					const svalue *sval,
					state_machine::state_t from,
					state_machine::state_t to,
					const svalue *origin,
					const program_state &dst_state,
					const exploded_node *enode)
: checker_event (EK_STATE_CHANGE,
		 event_loc_info (stmt->location,
				 node->m_fun->decl,
				 stack_depth)),
  m_node (node), m_stmt (stmt), m_sm (sm),
  m_sval (sval), m_from (from), m_to (to),
  m_origin (origin),
  m_dst_state (dst_state),
  m_enode (enode)
{
}

/* Implementation of diagnostic_event::get_desc vfunc for
   state_change_event.

   Attempt to generate a nicer human-readable description.
   For greatest precision-of-wording, give the pending diagnostic
   a chance to describe this state change (in terms of the
   diagnostic).
   Note that we only have a pending_diagnostic set on the event once
   the diagnostic is about to being emitted, so the description for
   an event can change.  */

label_text
state_change_event::get_desc (bool can_colorize) const
{
  if (m_pending_diagnostic)
    {
      region_model *model = m_dst_state.m_region_model;
      tree var = model->get_representative_tree (m_sval);
      tree origin = model->get_representative_tree (m_origin);
      label_text custom_desc
	= m_pending_diagnostic->describe_state_change
	    (evdesc::state_change (can_colorize, var, origin,
				   m_from, m_to, m_emission_id, *this));
      if (custom_desc.get ())
	{
	  if (flag_analyzer_verbose_state_changes)
	    {
	      /* Get any "meaning" of event.  */
	      diagnostic_event::meaning meaning = get_meaning ();
	      pretty_printer meaning_pp;
	      meaning.dump_to_pp (&meaning_pp);

	      /* Append debug version.  */
	      if (m_origin)
		return make_label_text
		  (can_colorize,
		   "%s (state of %qE: %qs -> %qs, origin: %qE, meaning: %s)",
		   custom_desc.get (),
		   var,
		   m_from->get_name (),
		   m_to->get_name (),
		   origin,
		   pp_formatted_text (&meaning_pp));
	      else
		return make_label_text
		  (can_colorize,
		   "%s (state of %qE: %qs -> %qs, NULL origin, meaning: %s)",
		   custom_desc.get (),
		   var,
		   m_from->get_name (),
		   m_to->get_name (),
		   pp_formatted_text (&meaning_pp));
	    }
	  else
	    return custom_desc;
	}
    }

  /* Fallback description.  */
  if (m_sval)
    {
      label_text sval_desc = m_sval->get_desc ();
      if (m_origin)
	{
	  label_text origin_desc = m_origin->get_desc ();
	  return make_label_text
	    (can_colorize,
	     "state of %qs: %qs -> %qs (origin: %qs)",
	     sval_desc.get (),
	     m_from->get_name (),
	     m_to->get_name (),
	     origin_desc.get ());
	}
      else
	return make_label_text
	  (can_colorize,
	   "state of %qs: %qs -> %qs (NULL origin)",
	   sval_desc.get (),
	   m_from->get_name (),
	   m_to->get_name ());
    }
  else
    {
      gcc_assert (m_origin == NULL);
      return make_label_text
	(can_colorize,
	 "global state: %qs -> %qs",
	 m_from->get_name (),
	 m_to->get_name ());
    }
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   state change events: delegate to the pending_diagnostic to
   get any meaning.  */

diagnostic_event::meaning
state_change_event::get_meaning () const
{
  if (m_pending_diagnostic)
    {
      region_model *model = m_dst_state.m_region_model;
      tree var = model->get_representative_tree (m_sval);
      tree origin = model->get_representative_tree (m_origin);
      return m_pending_diagnostic->get_meaning_for_state_change
	(evdesc::state_change (false, var, origin,
			       m_from, m_to, m_emission_id, *this));
    }
  else
    return meaning ();
}

/* class superedge_event : public checker_event.  */

/* Get the callgraph_superedge for this superedge_event, which must be
   for an interprocedural edge, rather than a CFG edge.  */

const callgraph_superedge&
superedge_event::get_callgraph_superedge () const
{
  gcc_assert (m_sedge->m_kind != SUPEREDGE_CFG_EDGE);
  return *m_sedge->dyn_cast_callgraph_superedge ();
}

/* Determine if this event should be filtered at the given verbosity
   level.  */

bool
superedge_event::should_filter_p (int verbosity) const
{
  switch (m_sedge->m_kind)
    {
    case SUPEREDGE_CFG_EDGE:
      {
	if (verbosity < 2)
	  return true;

	if (verbosity < 4)
	  {
	    /* Filter events with empty descriptions.  This ought to filter
	       FALLTHRU, but retain true/false/switch edges.  */
	    label_text desc = get_desc (false);
	    gcc_assert (desc.get ());
	    if (desc.get ()[0] == '\0')
	      return true;
	  }
      }
      break;

    default:
      break;
    }
  return false;
}

/* superedge_event's ctor.  */

superedge_event::superedge_event (enum event_kind kind,
				  const exploded_edge &eedge,
				  const event_loc_info &loc_info)
: checker_event (kind, loc_info),
  m_eedge (eedge), m_sedge (eedge.m_sedge),
  m_var (NULL_TREE), m_critical_state (0)
{
}

/* class cfg_edge_event : public superedge_event.  */

/* Get the cfg_superedge for this cfg_edge_event.  */

const cfg_superedge &
cfg_edge_event::get_cfg_superedge () const
{
  return *m_sedge->dyn_cast_cfg_superedge ();
}

/* cfg_edge_event's ctor.  */

cfg_edge_event::cfg_edge_event (enum event_kind kind,
				const exploded_edge &eedge,
				const event_loc_info &loc_info)
: superedge_event (kind, eedge, loc_info)
{
  gcc_assert (eedge.m_sedge->m_kind == SUPEREDGE_CFG_EDGE);
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   CFG edge events.  */

diagnostic_event::meaning
cfg_edge_event::get_meaning () const
{
  const cfg_superedge& cfg_sedge = get_cfg_superedge ();
  if (cfg_sedge.true_value_p ())
    return meaning (VERB_branch, PROPERTY_true);
  else if (cfg_sedge.false_value_p ())
    return meaning (VERB_branch, PROPERTY_false);
  else
    return meaning ();
}

/* class start_cfg_edge_event : public cfg_edge_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   start_cfg_edge_event.

   If -fanalyzer-verbose-edges, then generate low-level descriptions, such
   as
     "taking 'true' edge SN:7 -> SN:8".

   Otherwise, generate strings using the label of the underlying CFG if
   any, such as:
     "following 'true' branch..." or
     "following 'case 3' branch..."
     "following 'default' branch..."

   For conditionals, attempt to supply a description of the condition that
   holds, such as:
     "following 'false' branch (when 'ptr' is non-NULL)..."

   Failing that, return an empty description (which will lead to this event
   being filtered).  */

label_text
start_cfg_edge_event::get_desc (bool can_colorize) const
{
  bool user_facing = !flag_analyzer_verbose_edges;
  label_text edge_desc (m_sedge->get_description (user_facing));
  if (user_facing)
    {
      if (edge_desc.get () && strlen (edge_desc.get ()) > 0)
	{
	  label_text cond_desc = maybe_describe_condition (can_colorize);
	  label_text result;
	  if (cond_desc.get ())
	    return make_label_text (can_colorize,
				    "following %qs branch (%s)...",
				    edge_desc.get (), cond_desc.get ());
	  else
	    return make_label_text (can_colorize,
				    "following %qs branch...",
				    edge_desc.get ());
	}
      else
	return label_text::borrow ("");
    }
  else
    {
      if (strlen (edge_desc.get ()) > 0)
	return make_label_text (can_colorize,
				"taking %qs edge SN:%i -> SN:%i",
				edge_desc.get (),
				m_sedge->m_src->m_index,
				m_sedge->m_dest->m_index);
      else
	return make_label_text (can_colorize,
				"taking edge SN:%i -> SN:%i",
				m_sedge->m_src->m_index,
				m_sedge->m_dest->m_index);
    }
}

/* Attempt to generate a description of any condition that holds at this edge.

   The intent is to make the user-facing messages more clear, especially for
   cases where there's a single or double-negative, such as
   when describing the false branch of an inverted condition.

   For example, rather than printing just:

      |  if (!ptr)
      |     ~
      |     |
      |     (1) following 'false' branch...

   it's clearer to spell out the condition that holds:

      |  if (!ptr)
      |     ~
      |     |
      |     (1) following 'false' branch (when 'ptr' is non-NULL)...
                                          ^^^^^^^^^^^^^^^^^^^^^^

   In the above example, this function would generate the highlighted
   string: "when 'ptr' is non-NULL".

   If the edge is not a condition, or it's not clear that a description of
   the condition would be helpful to the user, return NULL.  */

label_text
start_cfg_edge_event::maybe_describe_condition (bool can_colorize) const
{
  const cfg_superedge& cfg_sedge = get_cfg_superedge ();

  if (cfg_sedge.true_value_p () || cfg_sedge.false_value_p ())
    {
      const gimple *last_stmt = m_sedge->m_src->get_last_stmt ();
      if (const gcond *cond_stmt = dyn_cast <const gcond *> (last_stmt))
	{
	  enum tree_code op = gimple_cond_code (cond_stmt);
	  tree lhs = gimple_cond_lhs (cond_stmt);
	  tree rhs = gimple_cond_rhs (cond_stmt);
	  if (cfg_sedge.false_value_p ())
	    op = invert_tree_comparison (op, false /* honor_nans */);
	  return maybe_describe_condition (can_colorize,
					   lhs, op, rhs);
	}
    }
  return label_text::borrow (NULL);
}

/* Subroutine of maybe_describe_condition above.

   Attempt to generate a user-facing description of the condition
   LHS OP RHS, but only if it is likely to make it easier for the
   user to understand a condition.  */

label_text
start_cfg_edge_event::maybe_describe_condition (bool can_colorize,
						tree lhs,
						enum tree_code op,
						tree rhs)
{
  /* In theory we could just build a tree via
       fold_build2 (op, boolean_type_node, lhs, rhs)
     and print it with %qE on it, but this leads to warts such as
     parenthesizing vars, such as '(i) <= 9', and uses of '<unknown>'.  */

  /* Special-case: describe testing the result of strcmp, as figuring
     out what the "true" or "false" path is can be confusing to the user.  */
  if (TREE_CODE (lhs) == SSA_NAME
      && zerop (rhs))
    {
      if (gcall *call = dyn_cast <gcall *> (SSA_NAME_DEF_STMT (lhs)))
	if (is_special_named_call_p (call, "strcmp", 2))
	  {
	    if (op == EQ_EXPR)
	      return label_text::borrow ("when the strings are equal");
	    if (op == NE_EXPR)
	      return label_text::borrow ("when the strings are non-equal");
	  }
    }

  /* Only attempt to generate text for sufficiently simple expressions.  */
  if (!should_print_expr_p (lhs))
    return label_text::borrow (NULL);
  if (!should_print_expr_p (rhs))
    return label_text::borrow (NULL);

  /* Special cases for pointer comparisons against NULL.  */
  if (POINTER_TYPE_P (TREE_TYPE (lhs))
      && POINTER_TYPE_P (TREE_TYPE (rhs))
      && zerop (rhs))
    {
      if (op == EQ_EXPR)
	return make_label_text (can_colorize, "when %qE is NULL",
				lhs);
      if (op == NE_EXPR)
	return make_label_text (can_colorize, "when %qE is non-NULL",
				lhs);
    }

  return make_label_text (can_colorize, "when %<%E %s %E%>",
			  lhs, op_symbol_code (op), rhs);
}

/* Subroutine of maybe_describe_condition.

   Return true if EXPR is we will get suitable user-facing output
   from %E on it.  */

bool
start_cfg_edge_event::should_print_expr_p (tree expr)
{
  if (TREE_CODE (expr) == SSA_NAME)
    {
      if (SSA_NAME_VAR (expr))
	return should_print_expr_p (SSA_NAME_VAR (expr));
      else
	return false;
    }

  if (DECL_P (expr))
    return true;

  if (CONSTANT_CLASS_P (expr))
    return true;

  return false;
}

/* class call_event : public superedge_event.  */

/* call_event's ctor.  */

call_event::call_event (const exploded_edge &eedge,
			const event_loc_info &loc_info)
: superedge_event (EK_CALL_EDGE, eedge, loc_info)
{
  if (eedge.m_sedge)
    gcc_assert (eedge.m_sedge->m_kind == SUPEREDGE_CALL);

   m_src_snode = eedge.m_src->get_supernode ();
   m_dest_snode = eedge.m_dest->get_supernode ();
}

/* Implementation of diagnostic_event::get_desc vfunc for
   call_event.

   If this call event passes critical state for an sm-based warning,
   allow the diagnostic to generate a precise description, such as:

     "passing freed pointer 'ptr' in call to 'foo' from 'bar'"

   Otherwise, generate a description of the form
   "calling 'foo' from 'bar'".  */

label_text
call_event::get_desc (bool can_colorize) const
{
  if (m_critical_state && m_pending_diagnostic)
    {
      gcc_assert (m_var);
      tree var = fixup_tree_for_diagnostic (m_var);
      label_text custom_desc
	= m_pending_diagnostic->describe_call_with_state
	    (evdesc::call_with_state (can_colorize,
				      m_src_snode->m_fun->decl,
				      m_dest_snode->m_fun->decl,
				      var,
				      m_critical_state));
      if (custom_desc.get ())
	return custom_desc;
    }

  return make_label_text (can_colorize,
			  "calling %qE from %qE",
			  get_callee_fndecl (),
			  get_caller_fndecl ());
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   function call events.  */

diagnostic_event::meaning
call_event::get_meaning () const
{
  return meaning (VERB_call, NOUN_function);
}

/* Override of checker_event::is_call_p for calls.  */

bool
call_event::is_call_p () const
{
  return true;
}

tree
call_event::get_caller_fndecl () const
{
  return m_src_snode->m_fun->decl;
}

tree
call_event::get_callee_fndecl () const
{
  return m_dest_snode->m_fun->decl;
}

/* class return_event : public superedge_event.  */

/* return_event's ctor.  */

return_event::return_event (const exploded_edge &eedge,
			    const event_loc_info &loc_info)
: superedge_event (EK_RETURN_EDGE, eedge, loc_info)
{
  if (eedge.m_sedge)
    gcc_assert (eedge.m_sedge->m_kind == SUPEREDGE_RETURN);

  m_src_snode = eedge.m_src->get_supernode ();
  m_dest_snode = eedge.m_dest->get_supernode ();
}

/* Implementation of diagnostic_event::get_desc vfunc for
   return_event.

   If this return event returns critical state for an sm-based warning,
   allow the diagnostic to generate a precise description, such as:

      "possible of NULL to 'foo' from 'bar'"

   Otherwise, generate a description of the form
   "returning to 'foo' from 'bar'.  */

label_text
return_event::get_desc (bool can_colorize) const
{
  /*  For greatest precision-of-wording, if this is returning the
      state involved in the pending diagnostic, give the pending
      diagnostic a chance to describe this return (in terms of
      itself).  */
  if (m_critical_state && m_pending_diagnostic)
    {
      label_text custom_desc
	= m_pending_diagnostic->describe_return_of_state
	    (evdesc::return_of_state (can_colorize,
				      m_dest_snode->m_fun->decl,
				      m_src_snode->m_fun->decl,
				      m_critical_state));
      if (custom_desc.get ())
	return custom_desc;
    }
  return make_label_text (can_colorize,
			  "returning to %qE from %qE",
			  m_dest_snode->m_fun->decl,
			  m_src_snode->m_fun->decl);
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   function return events.  */

diagnostic_event::meaning
return_event::get_meaning () const
{
  return meaning (VERB_return, NOUN_function);
}

/* Override of checker_event::is_return_p for returns.  */

bool
return_event::is_return_p () const
{
  return true;
}

/* class start_consolidated_cfg_edges_event : public checker_event.  */

label_text
start_consolidated_cfg_edges_event::get_desc (bool can_colorize) const
{
  return make_label_text (can_colorize,
			  "following %qs branch...",
			  m_edge_sense ? "true" : "false");
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   start_consolidated_cfg_edges_event.  */

diagnostic_event::meaning
start_consolidated_cfg_edges_event::get_meaning () const
{
  return meaning (VERB_branch,
		  (m_edge_sense ? PROPERTY_true : PROPERTY_false));
}

/* class inlined_call_event : public checker_event.  */

label_text
inlined_call_event::get_desc (bool can_colorize) const
{
  return make_label_text (can_colorize,
			  "inlined call to %qE from %qE",
			  m_apparent_callee_fndecl,
			  m_apparent_caller_fndecl);
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   reconstructed inlined function calls.  */

diagnostic_event::meaning
inlined_call_event::get_meaning () const
{
  return meaning (VERB_call, NOUN_function);
}

/* class setjmp_event : public checker_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   setjmp_event.  */

label_text
setjmp_event::get_desc (bool can_colorize) const
{
  return make_label_text (can_colorize,
			  "%qs called here",
			  get_user_facing_name (m_setjmp_call));
}

/* Implementation of checker_event::prepare_for_emission vfunc for setjmp_event.

   Record this setjmp's event ID into the path, so that rewind events can
   use it.  */

void
setjmp_event::prepare_for_emission (checker_path *path,
				    pending_diagnostic *pd,
				    diagnostic_event_id_t emission_id)
{
  checker_event::prepare_for_emission (path, pd, emission_id);
  path->record_setjmp_event (m_enode, emission_id);
}

/* class rewind_event : public checker_event.  */

/* Get the fndecl containing the site of the longjmp call.  */

tree
rewind_event::get_longjmp_caller () const
{
  return m_eedge->m_src->get_function ()->decl;
}

/* Get the fndecl containing the site of the setjmp call.  */

tree
rewind_event::get_setjmp_caller () const
{
  return m_eedge->m_dest->get_function ()->decl;
}

/* rewind_event's ctor.  */

rewind_event::rewind_event (const exploded_edge *eedge,
			    enum event_kind kind,
			    const event_loc_info &loc_info,
			    const rewind_info_t *rewind_info)
: checker_event (kind, loc_info),
  m_rewind_info (rewind_info),
  m_eedge (eedge)
{
  gcc_assert (m_eedge->m_custom_info.get () == m_rewind_info);
}

/* class rewind_from_longjmp_event : public rewind_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   rewind_from_longjmp_event.  */

label_text
rewind_from_longjmp_event::get_desc (bool can_colorize) const
{
  const char *src_name
    = get_user_facing_name (m_rewind_info->get_longjmp_call ());

  if (get_longjmp_caller () == get_setjmp_caller ())
    /* Special-case: purely intraprocedural rewind.  */
    return make_label_text (can_colorize,
			    "rewinding within %qE from %qs...",
			    get_longjmp_caller (),
			    src_name);
  else
    return make_label_text (can_colorize,
			    "rewinding from %qs in %qE...",
			    src_name,
			    get_longjmp_caller ());
}

/* class rewind_to_setjmp_event : public rewind_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   rewind_to_setjmp_event.  */

label_text
rewind_to_setjmp_event::get_desc (bool can_colorize) const
{
  const char *dst_name
    = get_user_facing_name (m_rewind_info->get_setjmp_call ());

  /* If we can, identify the ID of the setjmp_event.  */
  if (m_original_setjmp_event_id.known_p ())
    {
      if (get_longjmp_caller () == get_setjmp_caller ())
	/* Special-case: purely intraprocedural rewind.  */
	return make_label_text (can_colorize,
				"...to %qs (saved at %@)",
				dst_name,
				&m_original_setjmp_event_id);
      else
	return make_label_text (can_colorize,
				"...to %qs in %qE (saved at %@)",
				dst_name,
				get_setjmp_caller (),
				&m_original_setjmp_event_id);
    }
  else
    {
      if (get_longjmp_caller () == get_setjmp_caller ())
	/* Special-case: purely intraprocedural rewind.  */
	return make_label_text (can_colorize,
				"...to %qs",
				dst_name,
				get_setjmp_caller ());
      else
	return make_label_text (can_colorize,
				"...to %qs in %qE",
				dst_name,
				get_setjmp_caller ());
    }
}

/* Implementation of checker_event::prepare_for_emission vfunc for
   rewind_to_setjmp_event.

   Attempt to look up the setjmp event ID that recorded the jmp_buf
   for this rewind.  */

void
rewind_to_setjmp_event::prepare_for_emission (checker_path *path,
					      pending_diagnostic *pd,
					      diagnostic_event_id_t emission_id)
{
  checker_event::prepare_for_emission (path, pd, emission_id);
  path->get_setjmp_event (m_rewind_info->get_enode_origin (),
			  &m_original_setjmp_event_id);
}

/* class warning_event : public checker_event.  */

/* Implementation of diagnostic_event::get_desc vfunc for
   warning_event.

   If the pending diagnostic implements describe_final_event, use it,
   generating a precise description e.g.
     "second 'free' here; first 'free' was at (7)"

   Otherwise generate a generic description.  */

label_text
warning_event::get_desc (bool can_colorize) const
{
  if (m_pending_diagnostic)
    {
      tree var = fixup_tree_for_diagnostic (m_var);
      label_text ev_desc
	= m_pending_diagnostic->describe_final_event
	    (evdesc::final_event (can_colorize, var, m_state, *this));
      if (ev_desc.get ())
	{
	  if (m_sm && flag_analyzer_verbose_state_changes)
	    {
	      if (var)
		return make_label_text (can_colorize,
					"%s (%qE is in state %qs)",
					ev_desc.get (),
					var, m_state->get_name ());
	      else
		return make_label_text (can_colorize,
					"%s (in global state %qs)",
					ev_desc.get (),
					m_state->get_name ());
	    }
	  else
	    return ev_desc;
	}
    }

  if (m_sm)
    {
      if (m_var)
	return make_label_text (can_colorize,
				"here (%qE is in state %qs)",
				m_var, m_state->get_name ());
      else
	return make_label_text (can_colorize,
				"here (in global state %qs)",
				m_state->get_name ());
    }
  else
    return label_text::borrow ("here");
}

/* Implementation of diagnostic_event::get_meaning vfunc for
   warning_event.  */

diagnostic_event::meaning
warning_event::get_meaning () const
{
  return meaning (VERB_danger, NOUN_unknown);
}

} // namespace ana

#endif /* #if ENABLE_ANALYZER */
