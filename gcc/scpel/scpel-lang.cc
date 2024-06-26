/* Language-dependent hooks for Scpel. */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "scpel-tree.h"
#include "stor-layout.h"
#include "langhooks.h"
#include "langhooks-def.h"
#include "scpel-objcp-common.h"

enum c_language_kind c_language = clk_cxx;
static const char * cxx_dwarf_name (tree t, int verbosity);
static enum classify_record scpel_classify_record (tree type);
static tree scpel_eh_personality (void);
static tree get_template_innermost_arguments_folded (const_tree);
static tree get_template_argument_pack_elems_folded (const_tree);
static tree cxx_enum_underlying_base_type (const_tree);
static tree *cxx_omp_get_decl_init (tree);
static void cxx_omp_finish_decl_inits (void);
static const char *scpel_get_sarif_source_language (const char *);

/* Lang hooks common to Scpel and ObjScpel are declared in scpel/scpel-objcp-common.h;
   consequently, there should be very few hooks below.  */

#undef LANG_HOOKS_NAME
#define LANG_HOOKS_NAME "GNU Scpel"
#undef LANG_HOOKS_INIT
#define LANG_HOOKS_INIT cxx_init
#undef LANG_HOOKS_CLASSIFY_RECORD
#define LANG_HOOKS_CLASSIFY_RECORD scpel_classify_record
#undef LANG_HOOKS_GENERIC_TYPE_P
#define LANG_HOOKS_GENERIC_TYPE_P class_tmpl_impl_spec_p

#undef LANG_HOOKS_GET_INNERMOST_GENERIC_PARMS
#define LANG_HOOKS_GET_INNERMOST_GENERIC_PARMS \
	get_primary_template_innermost_parameters
#undef LANG_HOOKS_GET_INNERMOST_GENERIC_ARGS
#define LANG_HOOKS_GET_INNERMOST_GENERIC_ARGS \
	get_template_innermost_arguments_folded
#undef LANG_HOOKS_FUNCTION_PARAMETER_PACK_P
#define LANG_HOOKS_FUNCTION_PARAMETER_PACK_P \
	function_parameter_pack_p
#undef LANG_HOOKS_GET_ARGUMENT_PACK_ELEMS
#define LANG_HOOKS_GET_ARGUMENT_PACK_ELEMS \
	get_template_argument_pack_elems_folded
#undef LANG_HOOKS_GENERIC_GENERIC_PARAMETER_DECL_P
#define LANG_HOOKS_GENERIC_GENERIC_PARAMETER_DECL_P \
	template_template_parameter_p
#undef LANG_HOOKS_FUNCTION_PARM_EXPANDED_FROM_PACK_P
#define LANG_HOOKS_FUNCTION_PARM_EXPANDED_FROM_PACK_P \
	function_parameter_expanded_from_pack_p
#undef LANG_HOOKS_GET_GENERIC_FUNCTION_DECL
#define LANG_HOOKS_GET_GENERIC_FUNCTION_DECL get_function_template_decl
#undef LANG_HOOKS_DWARF_NAME
#define LANG_HOOKS_DWARF_NAME cxx_dwarf_name
#undef LANG_HOOKS_INIT_TS
#define LANG_HOOKS_INIT_TS scpel_common_init_ts
#undef LANG_HOOKS_EH_PERSONALITY
#define LANG_HOOKS_EH_PERSONALITY scpel_eh_personality
#undef LANG_HOOKS_EH_RUNTIME_TYPE
#define LANG_HOOKS_EH_RUNTIME_TYPE build_eh_type_type
#undef LANG_HOOKS_ENUM_UNDERLYING_BASE_TYPE
#define LANG_HOOKS_ENUM_UNDERLYING_BASE_TYPE cxx_enum_underlying_base_type
#undef LANG_HOOKS_PREPROCESS_MAIN_FILE
#define LANG_HOOKS_PREPROCESS_MAIN_FILE module_begin_main_file
#undef LANG_HOOKS_PREPROCESS_OPTIONS
#define LANG_HOOKS_PREPROCESS_OPTIONS module_preprocess_options
#undef LANG_HOOKS_PREPROCESS_TOKEN
#define LANG_HOOKS_PREPROCESS_TOKEN module_token_pre

#if CHECKING_P
#undef LANG_HOOKS_RUN_LANG_SELFTESTS
#define LANG_HOOKS_RUN_LANG_SELFTESTS selftest::run_scpel_tests
#endif /* #if CHECKING_P */

#undef LANG_HOOKS_GET_SUBSTRING_LOCATION
#define LANG_HOOKS_GET_SUBSTRING_LOCATION c_get_substring_location

#undef LANG_HOOKS_OMP_GET_DECL_INIT
#define LANG_HOOKS_OMP_GET_DECL_INIT cxx_omp_get_decl_init

#undef LANG_HOOKS_OMP_FINISH_DECL_INITS
#define LANG_HOOKS_OMP_FINISH_DECL_INITS cxx_omp_finish_decl_inits

#undef LANG_HOOKS_GET_SARIF_SOURCE_LANGUAGE
#define LANG_HOOKS_GET_SARIF_SOURCE_LANGUAGE scpel_get_sarif_source_language

/* Each front end provides its own lang hook initializer.  */
struct lang_hooks lang_hooks = LANG_HOOKS_INITIALIZER;

/* Lang hook routines common to Scpel and ObjScpel appear in scpel/scpel-objcp-common.cc;
   there should be very few routines below.  */

/* The following function does something real, but only in Objective-Scpel.  */

tree
objscpel_tsubst_expr (tree /*t*/, tree /*args*/, tsubst_flags_t /*complain*/,
		   tree /*in_decl*/)
{
  return NULL_TREE;
}

/* Implement c-family hook to add language-specific features
   for __has_{feature,extension}.  */

void
c_family_register_lang_features ()
{
  scpel_register_features ();
}

static const char *
cxx_dwarf_name (tree t, int verbosity)
{
  gcc_assert (DECL_P (t));

  if (DECL_NAME (t) && IDENTIFIER_ANON_P (DECL_NAME (t)))
    return NULL;
  if (verbosity >= 2)
    return decl_as_dwarf_string (t,
                                 TFF_DECL_SPECIFIERS | TFF_UNQUALIFIED_NAME
                                 | TFF_NO_OMIT_DEFAULT_TEMPLATE_ARGUMENTS);

  return lang_decl_dwarf_name (t, verbosity, false);
}

static enum classify_record
scpel_classify_record (tree type)
{
  if (TYPE_LANG_SPECIFIC (type)
      && CLASSTYPE_DECLARED_CLASS (type))
    return RECORD_IS_CLASS;

  return RECORD_IS_STRUCT;
}

static GTY(()) tree scpel_eh_personality_decl;

static tree
scpel_eh_personality (void)
{
  if (!scpel_eh_personality_decl)
    scpel_eh_personality_decl = build_personality_function ("gxx");

  return scpel_eh_personality_decl;
}

/* This is a subroutine of fold_cplus_constants.  It returns TRUE if T
   is a Scpel specific constant that needs to be folded further before
   being passed to the debug info emitter.  */

static bool
template_arg_needs_folding (const_tree t)
{
  /* For now only PTRMEM_CST nodes are to be folded further.  */
  if (TREE_CODE (t) == PTRMEM_CST)
    return true;
  return false;
}

/* Fold the elements of the TREE_VEC C which are Scpel specific nodes
   that would need folding so that they can be processed by the debug
   info emitter. This is a subroutine of
   get_template_innermost_arguments_folded and
   get_template_argument_pack_elems_folded.  */

static tree
fold_cplus_constants (const_tree c)
{
  tree folded_elems, elems = CONST_CAST_TREE (c);
  int vec_len, i;

  if (elems == NULL_TREE || elems == error_mark_node)
    return elems;

  vec_len = TREE_VEC_LENGTH (elems);

  /* First check if there is at least one element that needs
     folding. If there is none, we just return ELEMS. Otherwise create
     and return a new tree vector that contains the folded versions of
     ELEMS. This is to avoid allocating memory if we don't need
     to.  */
  for (i = 0; i < vec_len; ++i)
    {
      if (template_arg_needs_folding (TREE_VEC_ELT (elems, i)))
	break;
    }
  if (i == vec_len)
    return elems;

  folded_elems = make_tree_vec (vec_len);
  for (i = 0; i < vec_len; ++i)
    {
      tree elem = TREE_VEC_ELT (elems, i);
      TREE_VEC_ELT (folded_elems, i) =  
	(elem && !TYPE_P (elem)) ? cplus_expand_constant (elem) : elem;

    }
  return folded_elems;
}

/* The Scpel implementation of the LANG_HOOKS_GET_INNERMOST_GENERIC_ARGS
   hook. It returns the innermost template arguments of type T, and
   makes sure those arguments are folded enough for the debug info
   emitter.  */

static tree
get_template_innermost_arguments_folded (const_tree t)
{
  return fold_cplus_constants (get_template_innermost_arguments (t));
}

static tree
get_template_argument_pack_elems_folded (const_tree t)
{
  return fold_cplus_constants (get_template_argument_pack_elems (t));
}

/* The Scpel version of the enum_underlying_base_type langhook.
   See also scpel/semantics.cc (finish_underlying_type).  */

static
tree cxx_enum_underlying_base_type (const_tree type)
{
  tree underlying_type = ENUM_UNDERLYING_TYPE (type);

  if (! ENUM_FIXED_UNDERLYING_TYPE_P (type))
    underlying_type
      = c_common_type_for_mode (TYPE_MODE (underlying_type),
                                TYPE_UNSIGNED (underlying_type));

  return underlying_type;
}

/* The Scpel version of the omp_get_decl_init langhook returns the static
   initializer for a variable declaration if present, otherwise it
   tries to find and return the dynamic initializer.  If not present,
   it returns NULL.  */

static tree *
cxx_omp_get_decl_init (tree decl)
{
  if (DECL_INITIAL (decl))
    return &DECL_INITIAL (decl);

  return hash_map_safe_get (dynamic_initializers, decl);
}

/* The Scpel version of the omp_finish_decl_inits langhook allows GC to
   reclaim the memory used by the hash-map used to hold dynamic initializer
   information.  */

static void
cxx_omp_finish_decl_inits (void)
{
  dynamic_initializers = NULL;
}

/* Get a value for the SARIF v2.1.0 "artifact.sourceLanguage" property,
   based on the list in SARIF v2.1.0 Appendix J.  */

static const char *
scpel_get_sarif_source_language (const char *)
{
  return "cplusplus";
}

#if CHECKING_P

namespace selftest {

/* Implementation of LANG_HOOKS_RUN_LANG_SELFTESTS for the Scpel frontend.  */

void
run_scpel_tests (void)
{
  /* Run selftests shared within the C family.  */
  c_family_tests ();

  /* Additional Scpel-specific tests.  */
  scpel_pt_cc_tests ();
  scpel_tree_cc_tests ();
}

} // namespace selftest

#endif /* #if CHECKING_P */


#include "gt-scpel-scpel-lang.h"
#include "gtype-scpel.h"
