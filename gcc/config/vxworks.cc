/* Common VxWorks target definitions for GNU compiler.
   Please review: $(src-dir)/SPL-README for Licencing info. */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "target.h"
#include "tree.h"
#include "stringpool.h"
#include "diagnostic-core.h"
#include "output.h"
#include "fold-const.h"
#include "rtl.h"
#include "memmodel.h"
#include "optabs.h"
#include "opts.h"

#if !HAVE_INITFINI_ARRAY_SUPPORT
/* Like default_named_section_asm_out_constructor, except that even
   constructors with DEFAULT_INIT_PRIORITY must go in a numbered
   section on VxWorks.  The VxWorks runtime uses a clever trick to get
   the sentinel entry (-1) inserted at the beginning of the .ctors
   segment.  This trick will not work if we ever generate any entries
   in plain .ctors sections; we must always use .ctors.PRIORITY.  */

void
vxworks_asm_out_constructor (rtx symbol, int priority)
{
  section *sec;

  sec = get_cdtor_priority_section (priority,
				    /*constructor_p=*/true);
  assemble_addr_to_section (symbol, sec);
}

/* See comment for vxworks_asm_out_constructor.  */

void
vxworks_asm_out_destructor (rtx symbol, int priority)
{
  section *sec;

  sec = get_cdtor_priority_section (priority,
				    /*constructor_p=*/false);
  assemble_addr_to_section (symbol, sec);
}
#endif

/* Return the list of FIELD_DECLs that make up an emulated TLS
   variable's control object.  TYPE is the structure these are fields
   of and *NAME will be filled in with the structure tag that should
   be used.  */

static tree
vxworks_emutls_var_fields (tree type, tree *name)
{
  tree field, next_field;
  
  *name = get_identifier ("__tls_var");
  
  field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
		      get_identifier ("size"), unsigned_type_node);
  DECL_CONTEXT (field) = type;
  next_field = field;

  field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
		      get_identifier ("module_id"), unsigned_type_node);
  DECL_CONTEXT (field) = type;
  DECL_CHAIN (field) = next_field;
  next_field = field;

  /* The offset field is declared as an unsigned int with pointer mode.  */
  field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
		      get_identifier ("offset"), long_unsigned_type_node);

  DECL_CONTEXT (field) = type;
  DECL_CHAIN (field) = next_field;

  return field;
}

/* Return the CONSTRUCTOR to initialize an emulated TLS control
   object.  VAR is the control object.  DECL is the TLS object itself
   and TMPL_ADDR is the address (an ADDR_EXPR) of the initializer for
   that object.  */

static tree
vxworks_emutls_var_init (tree var, tree decl, tree tmpl_addr)
{
  vec<constructor_elt, va_gc> *v;
  vec_alloc (v, 3);
  
  tree type = TREE_TYPE (var);
  tree field = TYPE_FIELDS (type);
  
  constructor_elt elt = {field, fold_convert (TREE_TYPE (field), tmpl_addr)};
  v->quick_push (elt);
  
  field = DECL_CHAIN (field);
  elt.index = field;
  elt.value = build_int_cst (TREE_TYPE (field), 0);
  v->quick_push (elt);
  
  field = DECL_CHAIN (field);
  elt.index = field;
  elt.value = fold_convert (TREE_TYPE (field), DECL_SIZE_UNIT (decl));
  v->quick_push (elt);
  
  return build_constructor (type, v);
}

/* Do VxWorks-specific parts of TARGET_OPTION_OVERRIDE.  */

void
vxworks_override_options (void)
{
  /* Setup the tls emulation bits if the OS misses proper
     tls support.  */
  targetm.have_tls = VXWORKS_HAVE_TLS;

  if (!VXWORKS_HAVE_TLS)
    {
      targetm.emutls.get_address = "__builtin___tls_lookup";
      targetm.emutls.register_common = NULL;
      targetm.emutls.var_section = ".tls_vars";
      targetm.emutls.tmpl_section = ".tls_data";
      targetm.emutls.var_prefix = "__tls__";
      targetm.emutls.tmpl_prefix = "";
      targetm.emutls.var_fields = vxworks_emutls_var_fields;
      targetm.emutls.var_init = vxworks_emutls_var_init;
      targetm.emutls.var_align_fixed = true;
      targetm.emutls.debug_form_tls_address = true;
    }

  /* Arrange to use .ctors/.dtors sections if the target VxWorks configuration
     and mode supports it, or the init/fini_array sections if we were
     configured with --enable-initfini-array explicitly.  In the latter case,
     the toolchain user is expected to provide whatever linker level glue is
     required to get things to operate properly.  */

  targetm.have_ctors_dtors = 
    TARGET_VXWORKS_HAVE_CTORS_DTORS || HAVE_INITFINI_ARRAY_SUPPORT;

  /* PIC is only supported for RTPs.  flags_pic might be < 0 here, in
     contexts where the corresponding switches are not processed,
     e.g. from --help.  We are not generating code in such cases.  */
  if (flag_pic > 0 && !TARGET_VXWORKS_RTP)
    error ("PIC is only supported for RTPs");

  /* VxWorks comes with non-gdb debuggers which only support strict
     dwarf up to certain version.  Default dwarf control to friendly
     values for these.  */

  if (!OPTION_SET_P (dwarf_strict))
    dwarf_strict = 1;

  if (!OPTION_SET_P (dwarf_version))
    dwarf_version = VXWORKS_DWARF_VERSION_DEFAULT;

}

/* We don't want to use library symbol __clear_cache on SR0640.  Avoid
   it and issue a direct call to cacheTextUpdate.  It takes a size_t
   length rather than the END address, so we have to compute it.  */

void
vxworks_emit_call_builtin___clear_cache (rtx begin, rtx end)
{
  /* STATUS cacheTextUpdate (void *, size_t); */
  rtx callee = gen_rtx_SYMBOL_REF (Pmode, "cacheTextUpdate");

  enum machine_mode size_mode = TYPE_MODE (sizetype);

  rtx len = simplify_gen_binary (MINUS, size_mode, end, begin);

  emit_library_call (callee,
		     LCT_NORMAL, VOIDmode,
		     begin, ptr_mode,
		     len, size_mode);
}
