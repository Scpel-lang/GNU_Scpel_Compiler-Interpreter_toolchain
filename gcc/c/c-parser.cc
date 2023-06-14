/* Parser for C and Objective-C. */

/* TODO:

   Make sure all relevant comments, and all relevant code from all
   actions, brought over from old parser.  Verify exact correspondence
   of syntax accepted.

   Add testcases covering every input symbol in every state in old and
   new parsers.

   Include full syntax for GNU C, including erroneous cases accepted
   with error messages, in syntax productions in comments.

   Make more diagnostics in the front end generally take an explicit
   location rather than implicitly using input_location.  */

#include "config.h"
#define INCLUDE_MEMORY
#include "system.h"
#include "coretypes.h"
#include "target.h"
#include "function.h"
#include "c-tree.h"
#include "timevar.h"
#include "stringpool.h"
#include "cgraph.h"
#include "attribs.h"
#include "stor-layout.h"
#include "varasm.h"
#include "trans-mem.h"
#include "c-family/c-pragma.h"
#include "c-lang.h"
#include "c-family/c-objc.h"
#include "plugin.h"
#include "omp-general.h"
#include "omp-offload.h"
#include "builtins.h"
#include "gomp-constants.h"
#include "c-family/c-indentation.h"
#include "gimple-expr.h"
#include "context.h"
#include "gcc-rich-location.h"
#include "c-parser.h"
#include "read-rtl-function.h"
#include "run-rtl-passes.h"
#include "intl.h"
#include "c-family/name-hint.h"
#include "tree-iterator.h"
#include "tree-pretty-print.h"
#include "memmodel.h"
#include "c-family/known-headers.h"
#include "bitmap.h"
#include "analyzer/analyzer-language.h"
#include "toplev.h"

/* We need to walk over decls with incomplete struct/union/enum types
   after parsing the whole translation unit.
   In finish_decl(), if the decl is static, has incomplete
   struct/union/enum type, it is appended to incomplete_record_decls.
   In c_parser_translation_unit(), we iterate over incomplete_record_decls
   and report error if any of the decls are still incomplete.  */ 

vec<tree> incomplete_record_decls;

void
set_c_expr_source_range (c_expr *expr,
			 location_t start, location_t finish)
{
  expr->src_range.m_start = start;
  expr->src_range.m_finish = finish;
  if (expr->value)
    set_source_range (expr->value, start, finish);
}

void
set_c_expr_source_range (c_expr *expr,
			 source_range src_range)
{
  expr->src_range = src_range;
  if (expr->value)
    set_source_range (expr->value, src_range);
}


/* Initialization routine for this file.  */

void
c_parse_init (void)
{
  /* The only initialization required is of the reserved word
     identifiers.  */
  unsigned int i;
  tree id;
  int mask = 0;

  /* Make sure RID_MAX hasn't grown past the 8 bits used to hold the keyword in
     the c_token structure.  */
  gcc_assert (RID_MAX <= 255);

  mask |= D_CXXONLY;
  if (!flag_isoc99)
    mask |= D_C99;
  if (!flag_isoc2x)
    mask |= D_C2X;
  if (flag_no_asm)
    {
      mask |= D_ASM | D_EXT;
      if (!flag_isoc99)
	mask |= D_EXT89;
      if (!flag_isoc2x)
	mask |= D_EXT11;
    }
  if (!c_dialect_objc ())
    mask |= D_OBJC | D_CXX_OBJC;

  ridpointers = ggc_cleared_vec_alloc<tree> ((int) RID_MAX);
  for (i = 0; i < num_c_common_reswords; i++)
    {
      /* If a keyword is disabled, do not enter it into the table
	 and so create a canonical spelling that isn't a keyword.  */
      if (c_common_reswords[i].disable & mask)
	{
	  if (warn_cxx_compat
	      && (c_common_reswords[i].disable & D_CXXWARN))
	    {
	      id = get_identifier (c_common_reswords[i].word);
	      C_SET_RID_CODE (id, RID_CXX_COMPAT_WARN);
	      C_IS_RESERVED_WORD (id) = 1;
	    }
	  continue;
	}

      id = get_identifier (c_common_reswords[i].word);
      C_SET_RID_CODE (id, c_common_reswords[i].rid);
      C_IS_RESERVED_WORD (id) = 1;
      ridpointers [(int) c_common_reswords[i].rid] = id;
    }

  for (i = 0; i < NUM_INT_N_ENTS; i++)
    {
      /* We always create the symbols but they aren't always supported.  */
      char name[50];
      sprintf (name, "__int%d", int_n_data[i].bitsize);
      id = get_identifier (name);
      C_SET_RID_CODE (id, RID_FIRST_INT_N + i);
      C_IS_RESERVED_WORD (id) = 1;

      sprintf (name, "__int%d__", int_n_data[i].bitsize);
      id = get_identifier (name);
      C_SET_RID_CODE (id, RID_FIRST_INT_N + i);
      C_IS_RESERVED_WORD (id) = 1;
    }

  if (flag_openmp)
    {
      id = get_identifier ("omp_all_memory");
      C_SET_RID_CODE (id, RID_OMP_ALL_MEMORY);
      C_IS_RESERVED_WORD (id) = 1;
      ridpointers [RID_OMP_ALL_MEMORY] = id;
    }
}

/* A parser structure recording information about the state and
   context of parsing.  Includes lexer information with up to two
   tokens of look-ahead; more are not needed for C.  */
struct GTY(()) c_parser {
  /* The look-ahead tokens.  */
  c_token * GTY((skip)) tokens;
  /* Buffer for look-ahead tokens.  */
  c_token tokens_buf[4];
  /* How many look-ahead tokens are available (0 - 4, or
     more if parsing from pre-lexed tokens).  */
  unsigned int tokens_avail;
  /* Raw look-ahead tokens, used only for checking in Objective-C
     whether '[[' starts attributes.  */
  vec<c_token, va_gc> *raw_tokens;
  /* The number of raw look-ahead tokens that have since been fully
     lexed.  */
  unsigned int raw_tokens_used;
  /* True if a syntax error is being recovered from; false otherwise.
     c_parser_error sets this flag.  It should clear this flag when
     enough tokens have been consumed to recover from the error.  */
  BOOL_BITFIELD error : 1;
  /* True if we're processing a pragma, and shouldn't automatically
     consume CPP_PRAGMA_EOL.  */
  BOOL_BITFIELD in_pragma : 1;
  /* True if we're parsing the outermost block of an if statement.  */
  BOOL_BITFIELD in_if_block : 1;
  /* True if we want to lex a translated, joined string (for an
     initial #pragma pch_preprocess).  Otherwise the parser is
     responsible for concatenating strings and translating to the
     execution character set as needed.  */
  BOOL_BITFIELD lex_joined_string : 1;
  /* True if, when the parser is concatenating string literals, it
     should translate them to the execution character set (false
     inside attributes).  */
  BOOL_BITFIELD translate_strings_p : 1;

  /* Objective-C specific parser/lexer information.  */

  /* True if we are in a context where the Objective-C "PQ" keywords
     are considered keywords.  */
  BOOL_BITFIELD objc_pq_context : 1;
  /* True if we are parsing a (potential) Objective-C foreach
     statement.  This is set to true after we parsed 'for (' and while
     we wait for 'in' or ';' to decide if it's a standard C for loop or an
     Objective-C foreach loop.  */
  BOOL_BITFIELD objc_could_be_foreach_context : 1;
  /* The following flag is needed to contextualize Objective-C lexical
     analysis.  In some cases (e.g., 'int NSObject;'), it is
     undesirable to bind an identifier to an Objective-C class, even
     if a class with that name exists.  */
  BOOL_BITFIELD objc_need_raw_identifier : 1;
  /* Nonzero if we're processing a __transaction statement.  The value
     is 1 | TM_STMT_ATTR_*.  */
  unsigned int in_transaction : 4;
  /* True if we are in a context where the Objective-C "Property attribute"
     keywords are valid.  */
  BOOL_BITFIELD objc_property_attr_context : 1;

  /* Whether we have just seen/constructed a string-literal.  Set when
     returning a string-literal from c_parser_string_literal.  Reset
     in consume_token.  Useful when we get a parse error and see an
     unknown token, which could have been a string-literal constant
     macro.  */
  BOOL_BITFIELD seen_string_literal : 1;

  /* Location of the last consumed token.  */
  location_t last_token_location;
};

/* Return a pointer to the Nth token in PARSERs tokens_buf.  */

c_token *
c_parser_tokens_buf (c_parser *parser, unsigned n)
{
  return &parser->tokens_buf[n];
}

/* Return the error state of PARSER.  */

bool
c_parser_error (c_parser *parser)
{
  return parser->error;
}

/* Set the error state of PARSER to ERR.  */

void
c_parser_set_error (c_parser *parser, bool err)
{
  parser->error = err;
}


/* The actual parser and external interface.  ??? Does this need to be
   garbage-collected?  */

static GTY (()) c_parser *the_parser;

/* Read in and lex a single token, storing it in *TOKEN.  If RAW,
   context-sensitive postprocessing of the token is not done.  */

static void
c_lex_one_token (c_parser *parser, c_token *token, bool raw = false)
{
  timevar_push (TV_LEX);

  if (raw || vec_safe_length (parser->raw_tokens) == 0)
    {
      token->type = c_lex_with_flags (&token->value, &token->location,
				      &token->flags,
				      (parser->lex_joined_string
				       ? 0 : C_LEX_STRING_NO_JOIN));
      token->id_kind = C_ID_NONE;
      token->keyword = RID_MAX;
      token->pragma_kind = PRAGMA_NONE;
    }
  else
    {
      /* Use a token previously lexed as a raw look-ahead token, and
	 complete the processing on it.  */
      *token = (*parser->raw_tokens)[parser->raw_tokens_used];
      ++parser->raw_tokens_used;
      if (parser->raw_tokens_used == vec_safe_length (parser->raw_tokens))
	{
	  vec_free (parser->raw_tokens);
	  parser->raw_tokens_used = 0;
	}
    }

  if (raw)
    goto out;

  switch (token->type)
    {
    case CPP_NAME:
      {
	tree decl;

	bool objc_force_identifier = parser->objc_need_raw_identifier;
	if (c_dialect_objc ())
	  parser->objc_need_raw_identifier = false;

	if (C_IS_RESERVED_WORD (token->value))
	  {
	    enum rid rid_code = C_RID_CODE (token->value);

	    if (rid_code == RID_CXX_COMPAT_WARN)
	      {
		warning_at (token->location,
			    OPT_Wc___compat,
			    "identifier %qE conflicts with C++ keyword",
			    token->value);
	      }
	    else if (rid_code >= RID_FIRST_ADDR_SPACE
		     && rid_code <= RID_LAST_ADDR_SPACE)
	      {
		addr_space_t as;
		as = (addr_space_t) (rid_code - RID_FIRST_ADDR_SPACE);
		targetm.addr_space.diagnose_usage (as, token->location);
		token->id_kind = C_ID_ADDRSPACE;
		token->keyword = rid_code;
		break;
	      }
	    else if (c_dialect_objc () && OBJC_IS_PQ_KEYWORD (rid_code))
	      {
		/* We found an Objective-C "pq" keyword (in, out,
		   inout, bycopy, byref, oneway).  They need special
		   care because the interpretation depends on the
		   context.  */
		if (parser->objc_pq_context)
		  {
		    token->type = CPP_KEYWORD;
		    token->keyword = rid_code;
		    break;
		  }
		else if (parser->objc_could_be_foreach_context
			 && rid_code == RID_IN)
		  {
		    /* We are in Objective-C, inside a (potential)
		       foreach context (which means after having
		       parsed 'for (', but before having parsed ';'),
		       and we found 'in'.  We consider it the keyword
		       which terminates the declaration at the
		       beginning of a foreach-statement.  Note that
		       this means you can't use 'in' for anything else
		       in that context; in particular, in Objective-C
		       you can't use 'in' as the name of the running
		       variable in a C for loop.  We could potentially
		       try to add code here to disambiguate, but it
		       seems a reasonable limitation.  */
		    token->type = CPP_KEYWORD;
		    token->keyword = rid_code;
		    break;
		  }
		/* Else, "pq" keywords outside of the "pq" context are
		   not keywords, and we fall through to the code for
		   normal tokens.  */
	      }
	    else if (c_dialect_objc () && OBJC_IS_PATTR_KEYWORD (rid_code))
	      {
		/* We found an Objective-C "property attribute"
		   keyword (getter, setter, readonly, etc). These are
		   only valid in the property context.  */
		if (parser->objc_property_attr_context)
		  {
		    token->type = CPP_KEYWORD;
		    token->keyword = rid_code;
		    break;
		  }
		/* Else they are not special keywords.
		*/
	      }
	    else if (c_dialect_objc () 
		     && (OBJC_IS_AT_KEYWORD (rid_code)
			 || OBJC_IS_CXX_KEYWORD (rid_code)))
	      {
		/* We found one of the Objective-C "@" keywords (defs,
		   selector, synchronized, etc) or one of the
		   Objective-C "cxx" keywords (class, private,
		   protected, public, try, catch, throw) without a
		   preceding '@' sign.  Do nothing and fall through to
		   the code for normal tokens (in C++ we would still
		   consider the CXX ones keywords, but not in C).  */
		;
	      }
	    else
	      {
		token->type = CPP_KEYWORD;
		token->keyword = rid_code;
		break;
	      }
	  }

	decl = lookup_name (token->value);
	if (decl)
	  {
	    if (TREE_CODE (decl) == TYPE_DECL)
	      {
		token->id_kind = C_ID_TYPENAME;
		break;
	      }
	  }
	else if (c_dialect_objc ())
	  {
	    tree objc_interface_decl = objc_is_class_name (token->value);
	    /* Objective-C class names are in the same namespace as
	       variables and typedefs, and hence are shadowed by local
	       declarations.  */
	    if (objc_interface_decl
                && (!objc_force_identifier || global_bindings_p ()))
	      {
		token->value = objc_interface_decl;
		token->id_kind = C_ID_CLASSNAME;
		break;
	      }
	  }
        token->id_kind = C_ID_ID;
      }
      break;
    case CPP_AT_NAME:
      /* This only happens in Objective-C; it must be a keyword.  */
      break;
    case CPP_COLON:
    case CPP_COMMA:
    case CPP_CLOSE_PAREN:
    case CPP_SEMICOLON:
      /* These tokens may affect the interpretation of any identifiers
	 following, if doing Objective-C.  */
      if (c_dialect_objc ())
	parser->objc_need_raw_identifier = false;
      break;
    case CPP_PRAGMA:
      /* We smuggled the cpp_token->u.pragma value in an INTEGER_CST.  */
      token->pragma_kind = (enum pragma_kind) TREE_INT_CST_LOW (token->value);
      token->value = NULL;
      break;
    default:
      break;
    }
 out:
  timevar_pop (TV_LEX);
}

/* Return a pointer to the next token from PARSER, reading it in if
   necessary.  */

c_token *
c_parser_peek_token (c_parser *parser)
{
  if (parser->tokens_avail == 0)
    {
      c_lex_one_token (parser, &parser->tokens[0]);
      parser->tokens_avail = 1;
    }
  return &parser->tokens[0];
}

/* Return a pointer to the next-but-one token from PARSER, reading it
   in if necessary.  The next token is already read in.  */

c_token *
c_parser_peek_2nd_token (c_parser *parser)
{
  if (parser->tokens_avail >= 2)
    return &parser->tokens[1];
  gcc_assert (parser->tokens_avail == 1);
  gcc_assert (parser->tokens[0].type != CPP_EOF);
  gcc_assert (parser->tokens[0].type != CPP_PRAGMA_EOL);
  c_lex_one_token (parser, &parser->tokens[1]);
  parser->tokens_avail = 2;
  return &parser->tokens[1];
}

/* Return a pointer to the Nth token from PARSER, reading it
   in if necessary.  The N-1th token is already read in.  */

c_token *
c_parser_peek_nth_token (c_parser *parser, unsigned int n)
{
  /* N is 1-based, not zero-based.  */
  gcc_assert (n > 0);

  if (parser->tokens_avail >= n)
    return &parser->tokens[n - 1];
  gcc_assert (parser->tokens_avail == n - 1);
  c_lex_one_token (parser, &parser->tokens[n - 1]);
  parser->tokens_avail = n;
  return &parser->tokens[n - 1];
}

/* Return a pointer to the Nth token from PARSER, reading it in as a
   raw look-ahead token if necessary.  The N-1th token is already read
   in.  Raw look-ahead tokens remain available for when the non-raw
   functions above are called.  */

c_token *
c_parser_peek_nth_token_raw (c_parser *parser, unsigned int n)
{
  /* N is 1-based, not zero-based.  */
  gcc_assert (n > 0);

  if (parser->tokens_avail >= n)
    return &parser->tokens[n - 1];
  unsigned int raw_len = vec_safe_length (parser->raw_tokens);
  unsigned int raw_avail
    = parser->tokens_avail + raw_len - parser->raw_tokens_used;
  gcc_assert (raw_avail >= n - 1);
  if (raw_avail >= n)
    return &(*parser->raw_tokens)[parser->raw_tokens_used
				  + n - 1 - parser->tokens_avail];
  vec_safe_reserve (parser->raw_tokens, 1);
  parser->raw_tokens->quick_grow (raw_len + 1);
  c_lex_one_token (parser, &(*parser->raw_tokens)[raw_len], true);
  return &(*parser->raw_tokens)[raw_len];
}

bool
c_keyword_starts_typename (enum rid keyword)
{
  switch (keyword)
    {
    case RID_UNSIGNED:
    case RID_LONG:
    case RID_SHORT:
    case RID_SIGNED:
    case RID_COMPLEX:
    case RID_INT:
    case RID_CHAR:
    case RID_FLOAT:
    case RID_DOUBLE:
    case RID_VOID:
    case RID_DFLOAT32:
    case RID_DFLOAT64:
    case RID_DFLOAT128:
    CASE_RID_FLOATN_NX:
    case RID_BOOL:
    case RID_ENUM:
    case RID_STRUCT:
    case RID_UNION:
    case RID_TYPEOF:
    case RID_TYPEOF_UNQUAL:
    case RID_CONST:
    case RID_ATOMIC:
    case RID_VOLATILE:
    case RID_RESTRICT:
    case RID_ATTRIBUTE:
    case RID_FRACT:
    case RID_ACCUM:
    case RID_SAT:
    case RID_AUTO_TYPE:
    case RID_ALIGNAS:
      return true;
    default:
      if (keyword >= RID_FIRST_INT_N
	  && keyword < RID_FIRST_INT_N + NUM_INT_N_ENTS
	  && int_n_enabled_p[keyword - RID_FIRST_INT_N])
	return true;
      return false;
    }
}

/* Return true if TOKEN can start a type name,
   false otherwise.  */
bool
c_token_starts_typename (c_token *token)
{
  switch (token->type)
    {
    case CPP_NAME:
      switch (token->id_kind)
	{
	case C_ID_ID:
	  return false;
	case C_ID_ADDRSPACE:
	  return true;
	case C_ID_TYPENAME:
	  return true;
	case C_ID_CLASSNAME:
	  gcc_assert (c_dialect_objc ());
	  return true;
	default:
	  gcc_unreachable ();
	}
    case CPP_KEYWORD:
      return c_keyword_starts_typename (token->keyword);
    case CPP_LESS:
      if (c_dialect_objc ())
	return true;
      return false;
    default:
      return false;
    }
}

/* Return true if the next token from PARSER can start a type name,
   false otherwise.  LA specifies how to do lookahead in order to
   detect unknown type names.  If unsure, pick CLA_PREFER_ID.  */

static inline bool
c_parser_next_tokens_start_typename (c_parser *parser, enum c_lookahead_kind la)
{
  c_token *token = c_parser_peek_token (parser);
  if (c_token_starts_typename (token))
    return true;

  /* Try a bit harder to detect an unknown typename.  */
  if (la != cla_prefer_id
      && token->type == CPP_NAME
      && token->id_kind == C_ID_ID

      /* Do not try too hard when we could have "object in array".  */
      && !parser->objc_could_be_foreach_context

      && (la == cla_prefer_type
	  || c_parser_peek_2nd_token (parser)->type == CPP_NAME
	  || c_parser_peek_2nd_token (parser)->type == CPP_MULT)

      /* Only unknown identifiers.  */
      && !lookup_name (token->value))
    return true;

  return false;
}

/* Return true if TOKEN, after an open parenthesis, can start a
   compound literal (either a storage class specifier allowed in that
   context, or a type name), false otherwise.  */
static bool
c_token_starts_compound_literal (c_token *token)
{
  switch (token->type)
    {
    case CPP_KEYWORD:
      switch (token->keyword)
	{
	case RID_CONSTEXPR:
	case RID_REGISTER:
	case RID_STATIC:
	case RID_THREAD:
	  return true;
	default:
	  break;
	}
      /* Fall through.  */
    default:
      return c_token_starts_typename (token);
    }
}

/* Return true if TOKEN is a type qualifier, false otherwise.  */
static bool
c_token_is_qualifier (c_token *token)
{
  switch (token->type)
    {
    case CPP_NAME:
      switch (token->id_kind)
	{
	case C_ID_ADDRSPACE:
	  return true;
	default:
	  return false;
	}
    case CPP_KEYWORD:
      switch (token->keyword)
	{
	case RID_CONST:
	case RID_VOLATILE:
	case RID_RESTRICT:
	case RID_ATTRIBUTE:
	case RID_ATOMIC:
	  return true;
	default:
	  return false;
	}
    case CPP_LESS:
      return false;
    default:
      gcc_unreachable ();
    }
}

/* Return true if the next token from PARSER is a type qualifier,
   false otherwise.  */
static inline bool
c_parser_next_token_is_qualifier (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);
  return c_token_is_qualifier (token);
}

/* Return true if TOKEN can start declaration specifiers (not
   including standard attributes), false otherwise.  */
static bool
c_token_starts_declspecs (c_token *token)
{
  switch (token->type)
    {
    case CPP_NAME:
      switch (token->id_kind)
	{
	case C_ID_ID:
	  return false;
	case C_ID_ADDRSPACE:
	  return true;
	case C_ID_TYPENAME:
	  return true;
	case C_ID_CLASSNAME:
	  gcc_assert (c_dialect_objc ());
	  return true;
	default:
	  gcc_unreachable ();
	}
    case CPP_KEYWORD:
      switch (token->keyword)
	{
	case RID_STATIC:
	case RID_EXTERN:
	case RID_REGISTER:
	case RID_TYPEDEF:
	case RID_INLINE:
	case RID_NORETURN:
	case RID_AUTO:
	case RID_THREAD:
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_SHORT:
	case RID_SIGNED:
	case RID_COMPLEX:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	CASE_RID_FLOATN_NX:
	case RID_BOOL:
	case RID_ENUM:
	case RID_STRUCT:
	case RID_UNION:
	case RID_TYPEOF:
	case RID_TYPEOF_UNQUAL:
	case RID_CONST:
	case RID_VOLATILE:
	case RID_RESTRICT:
	case RID_ATTRIBUTE:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_ALIGNAS:
	case RID_ATOMIC:
	case RID_AUTO_TYPE:
	case RID_CONSTEXPR:
	  return true;
	default:
	  if (token->keyword >= RID_FIRST_INT_N
	      && token->keyword < RID_FIRST_INT_N + NUM_INT_N_ENTS
	      && int_n_enabled_p[token->keyword - RID_FIRST_INT_N])
	    return true;
	  return false;
	}
    case CPP_LESS:
      if (c_dialect_objc ())
	return true;
      return false;
    default:
      return false;
    }
}


/* Return true if TOKEN can start declaration specifiers (not
   including standard attributes) or a static assertion, false
   otherwise.  */
static bool
c_token_starts_declaration (c_token *token)
{
  if (c_token_starts_declspecs (token)
      || token->keyword == RID_STATIC_ASSERT)
    return true;
  else
    return false;
}

/* Return true if the next token from PARSER can start declaration
   specifiers (not including standard attributes), false
   otherwise.  */
bool
c_parser_next_token_starts_declspecs (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);

  /* In Objective-C, a classname normally starts a declspecs unless it
     is immediately followed by a dot.  In that case, it is the
     Objective-C 2.0 "dot-syntax" for class objects, ie, calls the
     setter/getter on the class.  c_token_starts_declspecs() can't
     differentiate between the two cases because it only checks the
     current token, so we have a special check here.  */
  if (c_dialect_objc () 
      && token->type == CPP_NAME
      && token->id_kind == C_ID_CLASSNAME 
      && c_parser_peek_2nd_token (parser)->type == CPP_DOT)
    return false;

  return c_token_starts_declspecs (token);
}

/* Return true if the next tokens from PARSER can start declaration
   specifiers (not including standard attributes) or a static
   assertion, false otherwise.  */
bool
c_parser_next_tokens_start_declaration (c_parser *parser)
{
  c_token *token = c_parser_peek_token (parser);

  /* Same as above.  */
  if (c_dialect_objc () 
      && token->type == CPP_NAME
      && token->id_kind == C_ID_CLASSNAME 
      && c_parser_peek_2nd_token (parser)->type == CPP_DOT)
    return false;

  /* Labels do not start declarations.  */
  if (token->type == CPP_NAME
      && c_parser_peek_2nd_token (parser)->type == CPP_COLON)
    return false;

  if (c_token_starts_declaration (token))
    return true;

  if (c_parser_next_tokens_start_typename (parser, cla_nonabstract_decl))
    return true;

  return false;
}

/* Consume the next token from PARSER.  */

void
c_parser_consume_token (c_parser *parser)
{
  gcc_assert (parser->tokens_avail >= 1);
  gcc_assert (parser->tokens[0].type != CPP_EOF);
  gcc_assert (!parser->in_pragma || parser->tokens[0].type != CPP_PRAGMA_EOL);
  gcc_assert (parser->error || parser->tokens[0].type != CPP_PRAGMA);
  parser->last_token_location = parser->tokens[0].location;
  if (parser->tokens != &parser->tokens_buf[0])
    parser->tokens++;
  else if (parser->tokens_avail >= 2)
    {
      parser->tokens[0] = parser->tokens[1];
      if (parser->tokens_avail >= 3)
        {
          parser->tokens[1] = parser->tokens[2];
          if (parser->tokens_avail >= 4)
            parser->tokens[2] = parser->tokens[3];
        }
    }
  parser->tokens_avail--;
  parser->seen_string_literal = false;
}

/* Expect the current token to be a #pragma.  Consume it and remember
   that we've begun parsing a pragma.  */

static void
c_parser_consume_pragma (c_parser *parser)
{
  gcc_assert (!parser->in_pragma);
  gcc_assert (parser->tokens_avail >= 1);
  gcc_assert (parser->tokens[0].type == CPP_PRAGMA);
  if (parser->tokens != &parser->tokens_buf[0])
    parser->tokens++;
  else if (parser->tokens_avail >= 2)
    {
      parser->tokens[0] = parser->tokens[1];
      if (parser->tokens_avail >= 3)
	parser->tokens[1] = parser->tokens[2];
    }
  parser->tokens_avail--;
  parser->in_pragma = true;
}

/* Update the global input_location from TOKEN.  */
static inline void
c_parser_set_source_position_from_token (c_token *token)
{
  if (token->type != CPP_EOF)
    {
      input_location = token->location;
    }
}

/* Helper function for c_parser_error.
   Having peeked a token of kind TOK1_KIND that might signify
   a conflict marker, peek successor tokens to determine
   if we actually do have a conflict marker.
   Specifically, we consider a run of 7 '<', '=' or '>' characters
   at the start of a line as a conflict marker.
   These come through the lexer as three pairs and a single,
   e.g. three CPP_LSHIFT ("<<") and a CPP_LESS ('<').
   If it returns true, *OUT_LOC is written to with the location/range
   of the marker.  */

static bool
c_parser_peek_conflict_marker (c_parser *parser, enum cpp_ttype tok1_kind,
			       location_t *out_loc)
{
  c_token *token2 = c_parser_peek_2nd_token (parser);
  if (token2->type != tok1_kind)
    return false;
  c_token *token3 = c_parser_peek_nth_token (parser, 3);
  if (token3->type != tok1_kind)
    return false;
  c_token *token4 = c_parser_peek_nth_token (parser, 4);
  if (token4->type != conflict_marker_get_final_tok_kind (tok1_kind))
    return false;

  /* It must be at the start of the line.  */
  location_t start_loc = c_parser_peek_token (parser)->location;
  if (LOCATION_COLUMN (start_loc) != 1)
    return false;

  /* We have a conflict marker.  Construct a location of the form:
       <<<<<<<
       ^~~~~~~
     with start == caret, finishing at the end of the marker.  */
  location_t finish_loc = get_finish (token4->location);
  *out_loc = make_location (start_loc, start_loc, finish_loc);

  return true;
}

/* Issue a diagnostic of the form
      FILE:LINE: MESSAGE before TOKEN
   where TOKEN is the next token in the input stream of PARSER.
   MESSAGE (specified by the caller) is usually of the form "expected
   OTHER-TOKEN".

   Use RICHLOC as the location of the diagnostic.

   Do not issue a diagnostic if still recovering from an error.

   Return true iff an error was actually emitted.

   ??? This is taken from the C++ parser, but building up messages in
   this way is not i18n-friendly and some other approach should be
   used.  */

static bool
c_parser_error_richloc (c_parser *parser, const char *gmsgid,
			rich_location *richloc)
{
  c_token *token = c_parser_peek_token (parser);
  if (parser->error)
    return false;
  parser->error = true;
  if (!gmsgid)
    return false;

  /* If this is actually a conflict marker, report it as such.  */
  if (token->type == CPP_LSHIFT
      || token->type == CPP_RSHIFT
      || token->type == CPP_EQ_EQ)
    {
      location_t loc;
      if (c_parser_peek_conflict_marker (parser, token->type, &loc))
	{
	  error_at (loc, "version control conflict marker in file");
	  return true;
	}
    }

  /* If we were parsing a string-literal and there is an unknown name
     token right after, then check to see if that could also have been
     a literal string by checking the name against a list of known
     standard string literal constants defined in header files. If
     there is one, then add that as an hint to the error message. */
  auto_diagnostic_group d;
  name_hint h;
  if (parser->seen_string_literal && token->type == CPP_NAME)
    {
      tree name = token->value;
      const char *token_name = IDENTIFIER_POINTER (name);
      const char *header_hint
	= get_c_stdlib_header_for_string_macro_name (token_name);
      if (header_hint != NULL)
	h = name_hint (NULL, new suggest_missing_header (token->location,
							 token_name,
							 header_hint));
    }

  c_parse_error (gmsgid,
		 /* Because c_parse_error does not understand
		    CPP_KEYWORD, keywords are treated like
		    identifiers.  */
		 (token->type == CPP_KEYWORD ? CPP_NAME : token->type),
		 /* ??? The C parser does not save the cpp flags of a
		    token, we need to pass 0 here and we will not get
		    the source spelling of some tokens but rather the
		    canonical spelling.  */
		 token->value, /*flags=*/0, richloc);
  return true;
}

/* As c_parser_error_richloc, but issue the message at the
   location of PARSER's next token, or at input_location
   if the next token is EOF.  */

bool
c_parser_error (c_parser *parser, const char *gmsgid)
{
  c_token *token = c_parser_peek_token (parser);
  c_parser_set_source_position_from_token (token);
  rich_location richloc (line_table, input_location);
  return c_parser_error_richloc (parser, gmsgid, &richloc);
}

/* Some tokens naturally come in pairs e.g.'(' and ')'.
   This class is for tracking such a matching pair of symbols.
   In particular, it tracks the location of the first token,
   so that if the second token is missing, we can highlight the
   location of the first token when notifying the user about the
   problem.  */

template <typename traits_t>
class token_pair
{
 public:
  /* token_pair's ctor.  */
  token_pair () : m_open_loc (UNKNOWN_LOCATION) {}

  /* If the next token is the opening symbol for this pair, consume it and
     return true.
     Otherwise, issue an error and return false.
     In either case, record the location of the opening token.  */

  bool require_open (c_parser *parser)
  {
    c_token *token = c_parser_peek_token (parser);
    if (token)
      m_open_loc = token->location;

    return c_parser_require (parser, traits_t::open_token_type,
			     traits_t::open_gmsgid);
  }

  /* Consume the next token from PARSER, recording its location as
     that of the opening token within the pair.  */

  void consume_open (c_parser *parser)
  {
    c_token *token = c_parser_peek_token (parser);
    gcc_assert (token->type == traits_t::open_token_type);
    m_open_loc = token->location;
    c_parser_consume_token (parser);
  }

  /* If the next token is the closing symbol for this pair, consume it
     and return true.
     Otherwise, issue an error, highlighting the location of the
     corresponding opening token, and return false.  */

  bool require_close (c_parser *parser) const
  {
    return c_parser_require (parser, traits_t::close_token_type,
			     traits_t::close_gmsgid, m_open_loc);
  }

  /* Like token_pair::require_close, except that tokens will be skipped
     until the desired token is found.  An error message is still produced
     if the next token is not as expected.  */

  void skip_until_found_close (c_parser *parser) const
  {
    c_parser_skip_until_found (parser, traits_t::close_token_type,
			       traits_t::close_gmsgid, m_open_loc);
  }

 private:
  location_t m_open_loc;
};

/* Traits for token_pair<T> for tracking matching pairs of parentheses.  */

struct matching_paren_traits
{
  static const enum cpp_ttype open_token_type = CPP_OPEN_PAREN;
  static const char * const open_gmsgid;
  static const enum cpp_ttype close_token_type = CPP_CLOSE_PAREN;
  static const char * const close_gmsgid;
};

const char * const matching_paren_traits::open_gmsgid = "expected %<(%>";
const char * const matching_paren_traits::close_gmsgid = "expected %<)%>";

/* "matching_parens" is a token_pair<T> class for tracking matching
   pairs of parentheses.  */

typedef token_pair<matching_paren_traits> matching_parens;

/* Traits for token_pair<T> for tracking matching pairs of braces.  */

struct matching_brace_traits
{
  static const enum cpp_ttype open_token_type = CPP_OPEN_BRACE;
  static const char * const open_gmsgid;
  static const enum cpp_ttype close_token_type = CPP_CLOSE_BRACE;
  static const char * const close_gmsgid;
};

const char * const matching_brace_traits::open_gmsgid = "expected %<{%>";
const char * const matching_brace_traits::close_gmsgid = "expected %<}%>";

/* "matching_braces" is a token_pair<T> class for tracking matching
   pairs of braces.  */

typedef token_pair<matching_brace_traits> matching_braces;

/* Get a description of the matching symbol to TYPE e.g. "(" for
   CPP_CLOSE_PAREN.  */

static const char *
get_matching_symbol (enum cpp_ttype type)
{
  switch (type)
    {
    default:
      gcc_unreachable ();
    case CPP_CLOSE_PAREN:
      return "(";
    case CPP_CLOSE_BRACE:
      return "{";
    }
}

/* If the next token is of the indicated TYPE, consume it.  Otherwise,
   issue the error MSGID.  If MSGID is NULL then a message has already
   been produced and no message will be produced this time.  Returns
   true if found, false otherwise.

   If MATCHING_LOCATION is not UNKNOWN_LOCATION, then highlight it
   within any error as the location of an "opening" token matching
   the close token TYPE (e.g. the location of the '(' when TYPE is
   CPP_CLOSE_PAREN).

   If TYPE_IS_UNIQUE is true (the default) then msgid describes exactly
   one type (e.g. "expected %<)%>") and thus it may be reasonable to
   attempt to generate a fix-it hint for the problem.
   Otherwise msgid describes multiple token types (e.g.
   "expected %<;%>, %<,%> or %<)%>"), and thus we shouldn't attempt to
   generate a fix-it hint.  */

bool
c_parser_require (c_parser *parser,
		  enum cpp_ttype type,
		  const char *msgid,
		  location_t matching_location,
		  bool type_is_unique)
{
  if (c_parser_next_token_is (parser, type))
    {
      c_parser_consume_token (parser);
      return true;
    }
  else
    {
      location_t next_token_loc = c_parser_peek_token (parser)->location;
      gcc_rich_location richloc (next_token_loc);

      /* Potentially supply a fix-it hint, suggesting to add the
	 missing token immediately after the *previous* token.
	 This may move the primary location within richloc.  */
      if (!parser->error && type_is_unique)
	maybe_suggest_missing_token_insertion (&richloc, type,
					       parser->last_token_location);

      /* If matching_location != UNKNOWN_LOCATION, highlight it.
	 Attempt to consolidate diagnostics by printing it as a
	 secondary range within the main diagnostic.  */
      bool added_matching_location = false;
      if (matching_location != UNKNOWN_LOCATION)
	added_matching_location
	  = richloc.add_location_if_nearby (matching_location);

      if (c_parser_error_richloc (parser, msgid, &richloc))
	/* If we weren't able to consolidate matching_location, then
	   print it as a secondary diagnostic.  */
	if (matching_location != UNKNOWN_LOCATION && !added_matching_location)
	  inform (matching_location, "to match this %qs",
		  get_matching_symbol (type));

      return false;
    }
}

/* If the next token is the indicated keyword, consume it.  Otherwise,
   issue the error MSGID.  Returns true if found, false otherwise.  */

static bool
c_parser_require_keyword (c_parser *parser,
			  enum rid keyword,
			  const char *msgid)
{
  if (c_parser_next_token_is_keyword (parser, keyword))
    {
      c_parser_consume_token (parser);
      return true;
    }
  else
    {
      c_parser_error (parser, msgid);
      return false;
    }
}

/* Like c_parser_require, except that tokens will be skipped until the
   desired token is found.  An error message is still produced if the
   next token is not as expected.  If MSGID is NULL then a message has
   already been produced and no message will be produced this
   time.

   If MATCHING_LOCATION is not UNKNOWN_LOCATION, then highlight it
   within any error as the location of an "opening" token matching
   the close token TYPE (e.g. the location of the '(' when TYPE is
   CPP_CLOSE_PAREN).  */

void
c_parser_skip_until_found (c_parser *parser,
			   enum cpp_ttype type,
			   const char *msgid,
			   location_t matching_location)
{
  unsigned nesting_depth = 0;

  if (c_parser_require (parser, type, msgid, matching_location))
    return;

  /* Skip tokens until the desired token is found.  */
  while (true)
    {
      /* Peek at the next token.  */
      c_token *token = c_parser_peek_token (parser);
      /* If we've reached the token we want, consume it and stop.  */
      if (token->type == type && !nesting_depth)
	{
	  c_parser_consume_token (parser);
	  break;
	}

      /* If we've run out of tokens, stop.  */
      if (token->type == CPP_EOF)
	return;
      if (token->type == CPP_PRAGMA_EOL && parser->in_pragma)
	return;
      if (token->type == CPP_OPEN_BRACE
	  || token->type == CPP_OPEN_PAREN
	  || token->type == CPP_OPEN_SQUARE)
	++nesting_depth;
      else if (token->type == CPP_CLOSE_BRACE
	       || token->type == CPP_CLOSE_PAREN
	       || token->type == CPP_CLOSE_SQUARE)
	{
	  if (nesting_depth-- == 0)
	    break;
	}
      /* Consume this token.  */
      c_parser_consume_token (parser);
    }
  parser->error = false;
}

/* Skip tokens until the end of a parameter is found, but do not
   consume the comma, semicolon or closing delimiter.  */

static void
c_parser_skip_to_end_of_parameter (c_parser *parser)
{
  unsigned nesting_depth = 0;

  while (true)
    {
      c_token *token = c_parser_peek_token (parser);
      if ((token->type == CPP_COMMA || token->type == CPP_SEMICOLON)
	  && !nesting_depth)
	break;
      /* If we've run out of tokens, stop.  */
      if (token->type == CPP_EOF)
	return;
      if (token->type == CPP_PRAGMA_EOL && parser->in_pragma)
	return;
      if (token->type == CPP_OPEN_BRACE
	  || token->type == CPP_OPEN_PAREN
	  || token->type == CPP_OPEN_SQUARE)
	++nesting_depth;
      else if (token->type == CPP_CLOSE_BRACE
	       || token->type == CPP_CLOSE_PAREN
	       || token->type == CPP_CLOSE_SQUARE)
	{
	  if (nesting_depth-- == 0)
	    break;
	}
      /* Consume this token.  */
      c_parser_consume_token (parser);
    }
  parser->error = false;
}

/* Expect to be at the end of the pragma directive and consume an
   end of line marker.  */

static void
c_parser_skip_to_pragma_eol (c_parser *parser, bool error_if_not_eol = true)
{
  gcc_assert (parser->in_pragma);
  parser->in_pragma = false;

  if (error_if_not_eol && c_parser_peek_token (parser)->type != CPP_PRAGMA_EOL)
    c_parser_error (parser, "expected end of line");

  cpp_ttype token_type;
  do
    {
      c_token *token = c_parser_peek_token (parser);
      token_type = token->type;
      if (token_type == CPP_EOF)
	break;
      c_parser_consume_token (parser);
    }
  while (token_type != CPP_PRAGMA_EOL);

  parser->error = false;
}

/* Skip tokens until we have consumed an entire block, or until we
   have consumed a non-nested ';'.  */

static void
c_parser_skip_to_end_of_block_or_statement (c_parser *parser)
{
  unsigned nesting_depth = 0;
  bool save_error = parser->error;

  while (true)
    {
      c_token *token;

      /* Peek at the next token.  */
      token = c_parser_peek_token (parser);

      switch (token->type)
	{
	case CPP_EOF:
	  return;

	case CPP_PRAGMA_EOL:
	  if (parser->in_pragma)
	    return;
	  break;

	case CPP_SEMICOLON:
	  /* If the next token is a ';', we have reached the
	     end of the statement.  */
	  if (!nesting_depth)
	    {
	      /* Consume the ';'.  */
	      c_parser_consume_token (parser);
	      goto finished;
	    }
	  break;

	case CPP_CLOSE_BRACE:
	  /* If the next token is a non-nested '}', then we have
	     reached the end of the current block.  */
	  if (nesting_depth == 0 || --nesting_depth == 0)
	    {
	      c_parser_consume_token (parser);
	      goto finished;
	    }
	  break;

	case CPP_OPEN_BRACE:
	  /* If it the next token is a '{', then we are entering a new
	     block.  Consume the entire block.  */
	  ++nesting_depth;
	  break;

	case CPP_PRAGMA:
	  /* If we see a pragma, consume the whole thing at once.  We
	     have some safeguards against consuming pragmas willy-nilly.
	     Normally, we'd expect to be here with parser->error set,
	     which disables these safeguards.  But it's possible to get
	     here for secondary error recovery, after parser->error has
	     been cleared.  */
	  c_parser_consume_pragma (parser);
	  c_parser_skip_to_pragma_eol (parser);
	  parser->error = save_error;
	  continue;

	default:
	  break;
	}

      c_parser_consume_token (parser);
    }

 finished:
  parser->error = false;
}

/* CPP's options (initialized by c-opts.cc).  */
extern cpp_options *cpp_opts;

/* Save the warning flags which are controlled by __extension__.  */

static inline int
disable_extension_diagnostics (void)
{
  int ret = (pedantic
	     | (warn_pointer_arith << 1)
	     | (warn_traditional << 2)
	     | (flag_iso << 3)
	     | (warn_long_long << 4)
	     | (warn_cxx_compat << 5)
	     | (warn_overlength_strings << 6)
	     /* warn_c90_c99_compat has three states: -1/0/1, so we must
		play tricks to properly restore it.  */
	     | ((warn_c90_c99_compat == 1) << 7)
	     | ((warn_c90_c99_compat == -1) << 8)
	     /* Similarly for warn_c99_c11_compat.  */
	     | ((warn_c99_c11_compat == 1) << 9)
	     | ((warn_c99_c11_compat == -1) << 10)
	     /* Similarly for warn_c11_c2x_compat.  */
	     | ((warn_c11_c2x_compat == 1) << 11)
	     | ((warn_c11_c2x_compat == -1) << 12)
	     );
  cpp_opts->cpp_pedantic = pedantic = 0;
  warn_pointer_arith = 0;
  cpp_opts->cpp_warn_traditional = warn_traditional = 0;
  flag_iso = 0;
  cpp_opts->cpp_warn_long_long = warn_long_long = 0;
  warn_cxx_compat = 0;
  warn_overlength_strings = 0;
  warn_c90_c99_compat = 0;
  warn_c99_c11_compat = 0;
  warn_c11_c2x_compat = 0;
  return ret;
}

/* Restore the warning flags which are controlled by __extension__.
   FLAGS is the return value from disable_extension_diagnostics.  */

static inline void
restore_extension_diagnostics (int flags)
{
  cpp_opts->cpp_pedantic = pedantic = flags & 1;
  warn_pointer_arith = (flags >> 1) & 1;
  cpp_opts->cpp_warn_traditional = warn_traditional = (flags >> 2) & 1;
  flag_iso = (flags >> 3) & 1;
  cpp_opts->cpp_warn_long_long = warn_long_long = (flags >> 4) & 1;
  warn_cxx_compat = (flags >> 5) & 1;
  warn_overlength_strings = (flags >> 6) & 1;
  /* See above for why is this needed.  */
  warn_c90_c99_compat = (flags >> 7) & 1 ? 1 : ((flags >> 8) & 1 ? -1 : 0);
  warn_c99_c11_compat = (flags >> 9) & 1 ? 1 : ((flags >> 10) & 1 ? -1 : 0);
  warn_c11_c2x_compat = (flags >> 11) & 1 ? 1 : ((flags >> 12) & 1 ? -1 : 0);
}

/* Helper data structure for parsing #pragma acc routine.  */
struct oacc_routine_data {
  bool error_seen; /* Set if error has been reported.  */
  bool fndecl_seen; /* Set if one fn decl/definition has been seen already.  */
  tree clauses;
  location_t loc;
};

/* Used for parsing objc foreach statements.  */
static tree objc_foreach_break_label, objc_foreach_continue_label;

static bool c_parser_nth_token_starts_std_attributes (c_parser *,
						      unsigned int);
static tree c_parser_std_attribute_specifier_sequence (c_parser *);
static void c_parser_external_declaration (c_parser *);
static void c_parser_asm_definition (c_parser *);
static void c_parser_declaration_or_fndef (c_parser *, bool, bool, bool,
					   bool, bool, tree * = NULL,
					   vec<c_token> * = NULL,
					   bool have_attrs = false,
					   tree attrs = NULL,
					   struct oacc_routine_data * = NULL,
					   bool * = NULL);
static void c_parser_static_assert_declaration_no_semi (c_parser *);
static void c_parser_static_assert_declaration (c_parser *);
static struct c_typespec c_parser_enum_specifier (c_parser *);
static struct c_typespec c_parser_struct_or_union_specifier (c_parser *);
static tree c_parser_struct_declaration (c_parser *, tree *);
static struct c_typespec c_parser_typeof_specifier (c_parser *);
static tree c_parser_alignas_specifier (c_parser *);
static struct c_declarator *c_parser_direct_declarator (c_parser *, bool,
							c_dtr_syn, bool *);
static struct c_declarator *c_parser_direct_declarator_inner (c_parser *,
							      bool,
							      struct c_declarator *);
static struct c_arg_info *c_parser_parms_declarator (c_parser *, bool, tree,
						     bool);
static struct c_arg_info *c_parser_parms_list_declarator (c_parser *, tree,
							  tree, bool);
static struct c_parm *c_parser_parameter_declaration (c_parser *, tree, bool);
static tree c_parser_simple_asm_expr (c_parser *);
static tree c_parser_gnu_attributes (c_parser *);
static struct c_expr c_parser_initializer (c_parser *, tree);
static struct c_expr c_parser_braced_init (c_parser *, tree, bool,
					   struct obstack *, tree);
static void c_parser_initelt (c_parser *, struct obstack *);
static void c_parser_initval (c_parser *, struct c_expr *,
			      struct obstack *);
static tree c_parser_compound_statement (c_parser *, location_t * = NULL);
static location_t c_parser_compound_statement_nostart (c_parser *);
static void c_parser_label (c_parser *, tree);
static void c_parser_statement (c_parser *, bool *, location_t * = NULL);
static void c_parser_statement_after_labels (c_parser *, bool *,
					     vec<tree> * = NULL);
static tree c_parser_c99_block_statement (c_parser *, bool *,
					  location_t * = NULL);
static void c_parser_if_statement (c_parser *, bool *, vec<tree> *);
static void c_parser_switch_statement (c_parser *, bool *);
static void c_parser_while_statement (c_parser *, bool, unsigned short, bool *);
static void c_parser_do_statement (c_parser *, bool, unsigned short);
static void c_parser_for_statement (c_parser *, bool, unsigned short, bool *);
static tree c_parser_asm_statement (c_parser *);
static tree c_parser_asm_operands (c_parser *);
static tree c_parser_asm_goto_operands (c_parser *);
static tree c_parser_asm_clobbers (c_parser *);
static struct c_expr c_parser_expr_no_commas (c_parser *, struct c_expr *,
					      tree = NULL_TREE);
static struct c_expr c_parser_conditional_expression (c_parser *,
						      struct c_expr *, tree);
static struct c_expr c_parser_binary_expression (c_parser *, struct c_expr *,
						 tree);
static struct c_expr c_parser_cast_expression (c_parser *, struct c_expr *);
static struct c_expr c_parser_unary_expression (c_parser *);
static struct c_expr c_parser_sizeof_expression (c_parser *);
static struct c_expr c_parser_alignof_expression (c_parser *);
static struct c_expr c_parser_postfix_expression (c_parser *);
static struct c_expr c_parser_postfix_expression_after_paren_type (c_parser *,
								   struct c_declspecs *,
								   struct c_type_name *,
								   location_t);
static struct c_expr c_parser_postfix_expression_after_primary (c_parser *,
								location_t loc,
								struct c_expr);
static struct c_expr c_parser_expression (c_parser *);
static struct c_expr c_parser_expression_conv (c_parser *);
static vec<tree, va_gc> *c_parser_expr_list (c_parser *, bool, bool,
					     vec<tree, va_gc> **, location_t *,
					     tree *, vec<location_t> *,
					     unsigned int * = NULL);
static struct c_expr c_parser_has_attribute_expression (c_parser *);


enum pragma_context { pragma_external, pragma_struct, pragma_param,
		      pragma_stmt, pragma_compound };
static bool c_parser_pragma (c_parser *, enum pragma_context, bool *);


#if ENABLE_ANALYZER

namespace ana {

/* Concrete implementation of ana::translation_unit for the C frontend.  */

class c_translation_unit : public translation_unit
{
public:
  /* Implementation of translation_unit::lookup_constant_by_id for use by the
     analyzer to look up named constants in the user's source code.  */
  tree lookup_constant_by_id (tree id) const final override
  {
    /* Consider decls.  */
    if (tree decl = lookup_name (id))
      if (TREE_CODE (decl) == CONST_DECL)
	if (tree value = DECL_INITIAL (decl))
	  if (TREE_CODE (value) == INTEGER_CST)
	    return value;

    /* Consider macros.  */
    cpp_hashnode *hashnode = C_CPP_HASHNODE (id);
    if (cpp_macro_p (hashnode))
      if (tree value = consider_macro (hashnode->value.macro))
	return value;

    return NULL_TREE;
  }

private:
  /* Attempt to get an INTEGER_CST from MACRO.
     Only handle the simplest cases: where MACRO's definition is a single
     token containing a number, by lexing the number again.
     This will handle e.g.
       #define NAME 42
     and other bases but not negative numbers, parentheses or e.g.
       #define NAME 1 << 7
     as doing so would require a parser.  */
  tree consider_macro (cpp_macro *macro) const
  {
    if (macro->paramc > 0)
      return NULL_TREE;
    if (macro->kind != cmk_macro)
      return NULL_TREE;
    if (macro->count != 1)
      return NULL_TREE;
    const cpp_token &tok = macro->exp.tokens[0];
    if (tok.type != CPP_NUMBER)
      return NULL_TREE;

    cpp_reader *old_parse_in = parse_in;
    parse_in = cpp_create_reader (CLK_GNUC89, NULL, line_table);

    pretty_printer pp;
    pp_string (&pp, (const char *) tok.val.str.text);
    pp_newline (&pp);
    cpp_push_buffer (parse_in,
		     (const unsigned char *) pp_formatted_text (&pp),
		     strlen (pp_formatted_text (&pp)),
		     0);

    tree value;
    location_t loc;
    unsigned char cpp_flags;
    c_lex_with_flags (&value, &loc, &cpp_flags, 0);

    cpp_destroy (parse_in);
    parse_in = old_parse_in;

    if (value && TREE_CODE (value) == INTEGER_CST)
      return value;

    return NULL_TREE;
  }
};

} // namespace ana

#endif /* #if ENABLE_ANALYZER */

/* Parse a translation unit (C90 6.7, C99 6.9, C11 6.9).

   translation-unit:
     external-declarations

   external-declarations:
     external-declaration
     external-declarations external-declaration

   GNU extensions:

   translation-unit:
     empty
*/

static void
c_parser_translation_unit (c_parser *parser)
{
  if (c_parser_next_token_is (parser, CPP_EOF))
    {
      pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
	       "ISO C forbids an empty translation unit");
    }
  else
    {
      void *obstack_position = obstack_alloc (&parser_obstack, 0);
      mark_valid_location_for_stdc_pragma (false);
      do
	{
	  ggc_collect ();
	  c_parser_external_declaration (parser);
	  obstack_free (&parser_obstack, obstack_position);
	}
      while (c_parser_next_token_is_not (parser, CPP_EOF));
    }

  unsigned int i;
  tree decl;
  FOR_EACH_VEC_ELT (incomplete_record_decls, i, decl)
    if (DECL_SIZE (decl) == NULL_TREE && TREE_TYPE (decl) != error_mark_node)
      error ("storage size of %q+D isn%'t known", decl);

  if (vec_safe_length (current_omp_declare_target_attribute))
    {
      c_omp_declare_target_attr
	a = current_omp_declare_target_attribute->pop ();
      if (!errorcount)
	error ("%qs without corresponding %qs",
	       a.device_type >= 0 ? "#pragma omp begin declare target"
				  : "#pragma omp declare target",
	       "#pragma omp end declare target");
      vec_safe_truncate (current_omp_declare_target_attribute, 0);
    }
  if (current_omp_begin_assumes)
    {
      if (!errorcount)
	error ("%qs without corresponding %qs",
	       "#pragma omp begin assumes", "#pragma omp end assumes");
      current_omp_begin_assumes = 0;
    }

#if ENABLE_ANALYZER
  if (flag_analyzer)
    {
      ana::c_translation_unit tu;
      ana::on_finish_translation_unit (tu);
    }
#endif
}

/* Parse an external declaration (C90 6.7, C99 6.9, C11 6.9).

   external-declaration:
     function-definition
     declaration

   GNU extensions:

   external-declaration:
     asm-definition
     ;
     __extension__ external-declaration

   Objective-C:

   external-declaration:
     objc-class-definition
     objc-class-declaration
     objc-alias-declaration
     objc-protocol-definition
     objc-method-definition
     @end
*/

static void
c_parser_external_declaration (c_parser *parser)
{
  int ext;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_EXTENSION:
	  ext = disable_extension_diagnostics ();
	  c_parser_consume_token (parser);
	  c_parser_external_declaration (parser);
	  restore_extension_diagnostics (ext);
	  break;
	case RID_ASM:
	  c_parser_asm_definition (parser);
	  break;
	default:
	  goto decl_or_fndef;
	}
      break;
    case CPP_SEMICOLON:
      pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
	       "ISO C does not allow extra %<;%> outside of a function");
      c_parser_consume_token (parser);
      break;
    case CPP_PRAGMA:
      mark_valid_location_for_stdc_pragma (true);
      c_parser_pragma (parser, pragma_external, NULL);
      mark_valid_location_for_stdc_pragma (false);
      break;
    case CPP_PLUS:
    case CPP_MINUS:
      /* Else fall through, and yield a syntax error trying to parse
	 as a declaration or function definition.  */
      /* FALLTHRU */
    default:
    decl_or_fndef:
      /* A declaration or a function definition (or, in Objective-C,
	 an @interface or @protocol with prefix attributes).  We can
	 only tell which after parsing the declaration specifiers, if
	 any, and the first declarator.  */
      c_parser_declaration_or_fndef (parser, true, true, true, false, true);
      break;
    }
}

static void c_finish_omp_declare_simd (c_parser *, tree, tree, vec<c_token> *);
static void c_finish_oacc_routine (struct oacc_routine_data *, tree, bool);

/* Build and add a DEBUG_BEGIN_STMT statement with location LOC.  */

static void
add_debug_begin_stmt (location_t loc)
{
  /* Don't add DEBUG_BEGIN_STMTs outside of functions, see PR84721.  */
  if (!MAY_HAVE_DEBUG_MARKER_STMTS || !building_stmt_list_p ())
    return;

  tree stmt = build0 (DEBUG_BEGIN_STMT, void_type_node);
  SET_EXPR_LOCATION (stmt, loc);
  add_stmt (stmt);
}

/* Helper function for c_parser_declaration_or_fndef and
   Handle assume attribute(s).  */

static tree
handle_assume_attribute (location_t here, tree attrs, bool nested)
{
  if (nested)
    for (tree attr = lookup_attribute ("gnu", "assume", attrs); attr;
	 attr = lookup_attribute ("gnu", "assume", TREE_CHAIN (attr)))
      {
	tree args = TREE_VALUE (attr);
	int nargs = list_length (args);
	if (nargs != 1)
	  {
	    error_at (here, "wrong number of arguments specified "
			    "for %qE attribute",
		      get_attribute_name (attr));
	    inform (here, "expected %i, found %i", 1, nargs);
	  }
	else
	  {
	    tree arg = TREE_VALUE (args);
	    arg = c_objc_common_truthvalue_conversion (here, arg);
	    arg = c_fully_fold (arg, false, NULL);
	    if (arg != error_mark_node)
	      {
		tree fn = build_call_expr_internal_loc (here, IFN_ASSUME,
							void_type_node, 1,
							arg);
		add_stmt (fn);
	      }
	  }
      }
  else
    pedwarn (here, OPT_Wattributes,
	     "%<assume%> attribute at top level");

  return remove_attribute ("gnu", "assume", attrs);
}

/* Parse a declaration or function definition (C90 6.5, 6.7.1, C99
   6.7, 6.9.1, C11 6.7, 6.9.1).  If FNDEF_OK is true, a function definition
   is accepted; otherwise (old-style parameter declarations) only other
   declarations are accepted.  If STATIC_ASSERT_OK is true, a static
   assertion is accepted; otherwise (old-style parameter declarations)
   it is not.  If NESTED is true, we are inside a function or parsing
   old-style parameter declarations; any functions encountered are
   nested functions and declaration specifiers are required; otherwise
   we are at top level and functions are normal functions and
   declaration specifiers may be optional.  If EMPTY_OK is true, empty
   declarations are OK (subject to all other constraints); otherwise
   (old-style parameter declarations) they are diagnosed.  If
   START_ATTR_OK is true, the declaration specifiers may start with
   attributes (GNU or standard); otherwise they may not.
   OBJC_FOREACH_OBJECT_DECLARATION can be used to get back the parsed
   declaration when parsing an Objective-C foreach statement.
   FALLTHRU_ATTR_P is used to signal whether this function parsed
   "__attribute__((fallthrough));".  ATTRS are any standard attributes
   parsed in the caller (in contexts where such attributes had to be
   parsed to determine whether what follows is a declaration or a
   statement); HAVE_ATTRS says whether there were any such attributes
   (even empty).

   declaration:
     declaration-specifiers init-declarator-list[opt] ;
     static_assert-declaration

   function-definition:
     declaration-specifiers[opt] declarator declaration-list[opt]
       compound-statement

   declaration-list:
     declaration
     declaration-list declaration

   init-declarator-list:
     init-declarator
     init-declarator-list , init-declarator

   init-declarator:
     declarator simple-asm-expr[opt] gnu-attributes[opt]
     declarator simple-asm-expr[opt] gnu-attributes[opt] = initializer

   GNU extensions:

   nested-function-definition:
     declaration-specifiers declarator declaration-list[opt]
       compound-statement

   attribute ;

   Objective-C:
     gnu-attributes objc-class-definition
     gnu-attributes objc-category-definition
     gnu-attributes objc-protocol-definition

   The simple-asm-expr and gnu-attributes are GNU extensions.

   This function does not handle __extension__; that is handled in its
   callers.  ??? Following the old parser, __extension__ may start
   external declarations, declarations in functions and declarations
   at the start of "for" loops, but not old-style parameter
   declarations.

   C99 requires declaration specifiers in a function definition; the
   absence is diagnosed through the diagnosis of implicit int.  In GNU
   C we also allow but diagnose declarations without declaration
   specifiers, but only at top level (elsewhere they conflict with
   other syntax).

   In Objective-C, declarations of the looping variable in a foreach
   statement are exceptionally terminated by 'in' (for example, 'for
   (NSObject *object in array) { ... }').

   OpenMP:

   declaration:
     threadprivate-directive

   GIMPLE:

   gimple-function-definition:
     declaration-specifiers[opt] __GIMPLE (gimple-or-rtl-pass-list) declarator
       declaration-list[opt] compound-statement

   rtl-function-definition:
     declaration-specifiers[opt] __RTL (gimple-or-rtl-pass-list) declarator
       declaration-list[opt] compound-statement  */

static void
c_parser_declaration_or_fndef (c_parser *parser, bool fndef_ok,
			       bool static_assert_ok, bool empty_ok,
			       bool nested, bool start_attr_ok,
			       tree *objc_foreach_object_declaration
			       /* = NULL */,
			       vec<c_token> *omp_declare_simd_clauses
			       /* = NULL */,
			       bool have_attrs /* = false */,
			       tree attrs /* = NULL_TREE */,
			       struct oacc_routine_data *oacc_routine_data
			       /* = NULL */,
			       bool *fallthru_attr_p /* = NULL */)
{
  struct c_declspecs *specs;
  tree prefix_attrs;
  tree all_prefix_attrs;
  bool diagnosed_no_specs = false;
  location_t here = c_parser_peek_token (parser)->location;

  add_debug_begin_stmt (c_parser_peek_token (parser)->location);

  if (static_assert_ok
      && c_parser_next_token_is_keyword (parser, RID_STATIC_ASSERT))
    {
      c_parser_static_assert_declaration (parser);
      return;
    }
  specs = build_null_declspecs ();

  /* Handle any standard attributes parsed in the caller.  */
  if (have_attrs)
    {
      declspecs_add_attrs (here, specs, attrs);
      specs->non_std_attrs_seen_p = false;
    }

  /* Try to detect an unknown type name when we have "A B" or "A *B".  */
  if (c_parser_peek_token (parser)->type == CPP_NAME
      && c_parser_peek_token (parser)->id_kind == C_ID_ID
      && (c_parser_peek_2nd_token (parser)->type == CPP_NAME
          || c_parser_peek_2nd_token (parser)->type == CPP_MULT)
      && (!nested || !lookup_name (c_parser_peek_token (parser)->value)))
    {
      tree name = c_parser_peek_token (parser)->value;

      /* Issue a warning about NAME being an unknown type name, perhaps
	 with some kind of hint.
	 If the user forgot a "struct" etc, suggest inserting
	 it.  Otherwise, attempt to look for misspellings.  */
      gcc_rich_location richloc (here);
      if (tag_exists_p (RECORD_TYPE, name))
	{
	  /* This is not C++ with its implicit typedef.  */
	  richloc.add_fixit_insert_before ("struct ");
	  error_at (&richloc,
		    "unknown type name %qE;"
		    " use %<struct%> keyword to refer to the type",
		    name);
	}
      else if (tag_exists_p (UNION_TYPE, name))
	{
	  richloc.add_fixit_insert_before ("union ");
	  error_at (&richloc,
		    "unknown type name %qE;"
		    " use %<union%> keyword to refer to the type",
		    name);
	}
      else if (tag_exists_p (ENUMERAL_TYPE, name))
	{
	  richloc.add_fixit_insert_before ("enum ");
	  error_at (&richloc,
		    "unknown type name %qE;"
		    " use %<enum%> keyword to refer to the type",
		    name);
	}
      else
	{
	  auto_diagnostic_group d;
	  name_hint hint = lookup_name_fuzzy (name, FUZZY_LOOKUP_TYPENAME,
					      here);
	  if (const char *suggestion = hint.suggestion ())
	    {
	      richloc.add_fixit_replace (suggestion);
	      error_at (&richloc,
			"unknown type name %qE; did you mean %qs?",
			name, suggestion);
	    }
	  else
	    error_at (here, "unknown type name %qE", name);
	}

      /* Parse declspecs normally to get a correct pointer type, but avoid
         a further "fails to be a type name" error.  Refuse nested functions
         since it is not how the user likely wants us to recover.  */
      c_parser_peek_token (parser)->type = CPP_KEYWORD;
      c_parser_peek_token (parser)->keyword = RID_VOID;
      c_parser_peek_token (parser)->value = error_mark_node;
      fndef_ok = !nested;
    }

  /* When there are standard attributes at the start of the
     declaration (to apply to the entity being declared), an
     init-declarator-list or function definition must be present.  */
  if (c_parser_nth_token_starts_std_attributes (parser, 1))
    have_attrs = true;

  c_parser_declspecs (parser, specs, true, true, start_attr_ok,
		      true, true, start_attr_ok, true, cla_nonabstract_decl);
  if (parser->error)
    {
      c_parser_skip_to_end_of_block_or_statement (parser);
      return;
    }
  if (nested && !specs->declspecs_seen_p)
    {
      c_parser_error (parser, "expected declaration specifiers");
      c_parser_skip_to_end_of_block_or_statement (parser);
      return;
    }

  finish_declspecs (specs);
  bool gnu_auto_type_p = specs->typespec_word == cts_auto_type;
  bool std_auto_type_p = specs->c2x_auto_p;
  bool any_auto_type_p = gnu_auto_type_p || std_auto_type_p;
  gcc_assert (!(gnu_auto_type_p && std_auto_type_p));
  const char *auto_type_keyword = gnu_auto_type_p ? "__auto_type" : "auto";
  if (specs->constexpr_p)
    {
      /* An underspecified declaration may not declare tags or members
	 or structures or unions; it is undefined behavior to declare
	 the members of an enumeration.  Where the structure, union or
	 enumeration type is declared within an initializer, this is
	 diagnosed elsewhere.  Diagnose here the case of declaring
	 such a type in the type specifiers of a constexpr
	 declaration.  */
      switch (specs->typespec_kind)
	{
	case ctsk_tagfirstref:
	case ctsk_tagfirstref_attrs:
	  error_at (here, "%qT declared in underspecified object declaration",
		    specs->type);
	  break;

	case ctsk_tagdef:
	  error_at (here, "%qT defined in underspecified object declaration",
		    specs->type);
	  break;

	default:
	  break;
	}
    }
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      bool handled_assume = false;
      if (specs->typespec_kind == ctsk_none
	  && lookup_attribute ("gnu", "assume", specs->attrs))
	{
	  handled_assume = true;
	  specs->attrs
	    = handle_assume_attribute (here, specs->attrs, nested);
	}
      if (any_auto_type_p)
	error_at (here, "%qs in empty declaration", auto_type_keyword);
      else if (specs->typespec_kind == ctsk_none
	       && attribute_fallthrough_p (specs->attrs))
	{
	  if (fallthru_attr_p != NULL)
	    *fallthru_attr_p = true;
	  if (nested)
	    {
	      tree fn = build_call_expr_internal_loc (here, IFN_FALLTHROUGH,
						      void_type_node, 0);
	      add_stmt (fn);
	    }
	  else
	    pedwarn (here, OPT_Wattributes,
		     "%<fallthrough%> attribute at top level");
	}
      else if (empty_ok
	       && !(have_attrs && specs->non_std_attrs_seen_p)
	       && !handled_assume)
	shadow_tag (specs);
      else
	{
	  shadow_tag_warned (specs, 1);
	  if (!handled_assume)
	    pedwarn (here, 0, "empty declaration");
	}
      /* We still have to evaluate size expressions.  */
      if (specs->expr)
	add_stmt (fold_convert (void_type_node, specs->expr));
      c_parser_consume_token (parser);
      if (oacc_routine_data)
	c_finish_oacc_routine (oacc_routine_data, NULL_TREE, false);
      return;
    }

  /* Provide better error recovery.  Note that a type name here is usually
     better diagnosed as a redeclaration.  */
  if (empty_ok
      && specs->typespec_kind == ctsk_tagdef
      && c_parser_next_token_starts_declspecs (parser)
      && !c_parser_next_token_is (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected %<;%>, identifier or %<(%>");
      parser->error = false;
      shadow_tag_warned (specs, 1);
      return;
    }
  else if (attribute_fallthrough_p (specs->attrs))
    warning_at (here, OPT_Wattributes,
		"%<fallthrough%> attribute not followed by %<;%>");
  else if (lookup_attribute ("gnu", "assume", specs->attrs))
    warning_at (here, OPT_Wattributes,
		"%<assume%> attribute not followed by %<;%>");

  pending_xref_error ();
  prefix_attrs = specs->attrs;
  all_prefix_attrs = prefix_attrs;
  specs->attrs = NULL_TREE;
  while (true)
    {
      struct c_declarator *declarator;
      bool dummy = false;
      timevar_id_t tv;
      tree fnbody = NULL_TREE;
      tree underspec_name = NULL_TREE;
      /* Declaring either one or more declarators (in which case we
	 should diagnose if there were no declaration specifiers) or a
	 function definition (in which case the diagnostic for
	 implicit int suffices).  */
      declarator = c_parser_declarator (parser, 
					specs->typespec_kind != ctsk_none,
					C_DTR_NORMAL, &dummy);
      if (declarator == NULL)
	{
	  if (omp_declare_simd_clauses)
	    c_finish_omp_declare_simd (parser, NULL_TREE, NULL_TREE,
				       omp_declare_simd_clauses);
	  if (oacc_routine_data)
	    c_finish_oacc_routine (oacc_routine_data, NULL_TREE, false);
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      if (gnu_auto_type_p && declarator->kind != cdk_id)
	{
	  error_at (here,
		    "%<__auto_type%> requires a plain identifier"
		    " as declarator");
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      if (std_auto_type_p)
	{
	  struct c_declarator *d = declarator;
	  while (d->kind == cdk_attrs)
	    d = d->declarator;
	  if (d->kind != cdk_id)
	    {
	      error_at (here,
			"%<auto%> requires a plain identifier, possibly with"
			" attributes, as declarator");
	      c_parser_skip_to_end_of_block_or_statement (parser);
	      return;
	    }
	  underspec_name = d->u.id.id;
	}
      else if (specs->constexpr_p)
	{
	  struct c_declarator *d = declarator;
	  while (d->kind != cdk_id)
	    d = d->declarator;
	  underspec_name = d->u.id.id;
	}
      if (c_parser_next_token_is (parser, CPP_EQ)
	  || c_parser_next_token_is (parser, CPP_COMMA)
	  || c_parser_next_token_is (parser, CPP_SEMICOLON)
	  || c_parser_next_token_is_keyword (parser, RID_ASM)
	  || c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE)
	  || c_parser_next_token_is_keyword (parser, RID_IN))
	{
	  tree asm_name = NULL_TREE;
	  tree postfix_attrs = NULL_TREE;
	  if (!diagnosed_no_specs && !specs->declspecs_seen_p)
	    {
	      diagnosed_no_specs = true;
	      pedwarn (here, 0, "data definition has no type or storage class");
	    }
	  /* Having seen a data definition, there cannot now be a
	     function definition.  */
	  fndef_ok = false;
	  if (c_parser_next_token_is_keyword (parser, RID_ASM))
	    asm_name = c_parser_simple_asm_expr (parser);
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    {
	      postfix_attrs = c_parser_gnu_attributes (parser);
	      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
		{
		  /* This means there is an attribute specifier after
		     the declarator in a function definition.  Provide
		     some more information for the user.  */
		  error_at (here, "attributes should be specified before the "
			    "declarator in a function definition");
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return;
		}
	    }
	  if (c_parser_next_token_is (parser, CPP_EQ))
	    {
	      tree d;
	      struct c_expr init;
	      location_t init_loc;
	      c_parser_consume_token (parser);
	      if (any_auto_type_p)
		{
		  init_loc = c_parser_peek_token (parser)->location;
		  rich_location richloc (line_table, init_loc);
		  unsigned int underspec_state = 0;
		  if (std_auto_type_p)
		    underspec_state =
		      start_underspecified_init (init_loc, underspec_name);
		  start_init (NULL_TREE, asm_name,
			      (global_bindings_p ()
			       || specs->storage_class == csc_static
			       || specs->constexpr_p),
			      specs->constexpr_p, &richloc);
		  /* A parameter is initialized, which is invalid.  Don't
		     attempt to instrument the initializer.  */
		  int flag_sanitize_save = flag_sanitize;
		  if (nested && !empty_ok)
		    flag_sanitize = 0;
		  init = c_parser_expr_no_commas (parser, NULL);
		  if (std_auto_type_p)
		    finish_underspecified_init (underspec_name,
						underspec_state);
		  flag_sanitize = flag_sanitize_save;
		  if (gnu_auto_type_p
		      && TREE_CODE (init.value) == COMPONENT_REF
		      && DECL_C_BIT_FIELD (TREE_OPERAND (init.value, 1)))
		    error_at (here,
			      "%<__auto_type%> used with a bit-field"
			      " initializer");
		  init = convert_lvalue_to_rvalue (init_loc, init, true, true,
						   true);
		  tree init_type = TREE_TYPE (init.value);
		  bool vm_type = c_type_variably_modified_p (init_type);
		  if (vm_type)
		    init.value = save_expr (init.value);
		  finish_init ();
		  specs->typespec_kind = ctsk_typeof;
		  specs->locations[cdw_typedef] = init_loc;
		  specs->typedef_p = true;
		  specs->type = init_type;
		  if (specs->postfix_attrs)
		    {
		      /* Postfix [[]] attributes are valid with C2X
			 auto, although not with __auto_type, and
			 modify the type given by the initializer.  */
		      specs->postfix_attrs =
			c_warn_type_attributes (specs->postfix_attrs);
		      decl_attributes (&specs->type, specs->postfix_attrs, 0);
		      specs->postfix_attrs = NULL_TREE;
		    }
		  if (vm_type)
		    {
		      bool maybe_const = true;
		      tree type_expr = c_fully_fold (init.value, false,
						     &maybe_const);
		      specs->expr_const_operands &= maybe_const;
		      if (specs->expr)
			specs->expr = build2 (COMPOUND_EXPR,
					      TREE_TYPE (type_expr),
					      specs->expr, type_expr);
		      else
			specs->expr = type_expr;
		    }
		  d = start_decl (declarator, specs, true,
				  chainon (postfix_attrs, all_prefix_attrs));
		  if (!d)
		    d = error_mark_node;
		  if (omp_declare_simd_clauses)
		    c_finish_omp_declare_simd (parser, d, NULL_TREE,
					       omp_declare_simd_clauses);
		}
	      else
		{
		  /* The declaration of the variable is in effect while
		     its initializer is parsed, except for a constexpr
		     variable.  */
		  init_loc = c_parser_peek_token (parser)->location;
		  rich_location richloc (line_table, init_loc);
		  unsigned int underspec_state = 0;
		  if (specs->constexpr_p)
		    underspec_state =
		      start_underspecified_init (init_loc, underspec_name);
		  d = start_decl (declarator, specs, true,
				  chainon (postfix_attrs,
					   all_prefix_attrs),
				  !specs->constexpr_p);
		  if (!d)
		    d = error_mark_node;
		  if (!specs->constexpr_p && omp_declare_simd_clauses)
		    c_finish_omp_declare_simd (parser, d, NULL_TREE,
					       omp_declare_simd_clauses);
		  start_init (d, asm_name,
			      TREE_STATIC (d) || specs->constexpr_p,
			      specs->constexpr_p, &richloc);
		  /* A parameter is initialized, which is invalid.  Don't
		     attempt to instrument the initializer.  */
		  int flag_sanitize_save = flag_sanitize;
		  if (TREE_CODE (d) == PARM_DECL)
		    flag_sanitize = 0;
		  init = c_parser_initializer (parser, d);
		  flag_sanitize = flag_sanitize_save;
		  if (specs->constexpr_p)
		    {
		      finish_underspecified_init (underspec_name,
						  underspec_state);
		      d = pushdecl (d);
		      if (omp_declare_simd_clauses)
			c_finish_omp_declare_simd (parser, d, NULL_TREE,
						   omp_declare_simd_clauses);
		    }
		  finish_init ();
		}
	      if (oacc_routine_data)
		c_finish_oacc_routine (oacc_routine_data, d, false);
	      if (d != error_mark_node)
		{
		  maybe_warn_string_init (init_loc, TREE_TYPE (d), init);
		  finish_decl (d, init_loc, init.value,
			       init.original_type, asm_name);
		}
	    }
	  else
	    {
	      if (any_auto_type_p || specs->constexpr_p)
		{
		  error_at (here,
			    "%qs requires an initialized data declaration",
			    any_auto_type_p ? auto_type_keyword : "constexpr");
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return;
		}

	      location_t lastloc = UNKNOWN_LOCATION;
	      tree attrs = chainon (postfix_attrs, all_prefix_attrs);
	      tree d = start_decl (declarator, specs, false, attrs, true,
				   &lastloc);
	      if (d && TREE_CODE (d) == FUNCTION_DECL)
		{
		  /* Find the innermost declarator that is neither cdk_id
		     nor cdk_attrs.  */
		  const struct c_declarator *decl = declarator;
		  const struct c_declarator *last_non_id_attrs = NULL;

		  while (decl)
		    switch (decl->kind)
		      {
		      case cdk_array:
		      case cdk_function:
		      case cdk_pointer:
			last_non_id_attrs = decl;
			decl = decl->declarator;
			break;

		      case cdk_attrs:
			decl = decl->declarator;
			break;

		      case cdk_id:
			decl = 0;
			break;

		      default:
			gcc_unreachable ();
		      }

		  /* If it exists and is cdk_function declaration whose
		     arguments have not been set yet, use its arguments.  */
		  if (last_non_id_attrs
		      && last_non_id_attrs->kind == cdk_function)
		    {
		      tree parms = last_non_id_attrs->u.arg_info->parms;
		      if (DECL_ARGUMENTS (d) == NULL_TREE
			  && DECL_INITIAL (d) == NULL_TREE)
			DECL_ARGUMENTS (d) = parms;

		      warn_parm_array_mismatch (lastloc, d, parms);
		    }
		}
	      if (omp_declare_simd_clauses)
		{
		  tree parms = NULL_TREE;
		  if (d && TREE_CODE (d) == FUNCTION_DECL)
		    {
		      struct c_declarator *ce = declarator;
		      while (ce != NULL)
			if (ce->kind == cdk_function)
			  {
			    parms = ce->u.arg_info->parms;
			    break;
			  }
			else
			  ce = ce->declarator;
		    }
		  if (parms)
		    temp_store_parm_decls (d, parms);
		  c_finish_omp_declare_simd (parser, d, parms,
					     omp_declare_simd_clauses);
		  if (parms)
		    temp_pop_parm_decls ();
		}
	      if (oacc_routine_data)
		c_finish_oacc_routine (oacc_routine_data, d, false);
	      if (d)
		finish_decl (d, UNKNOWN_LOCATION, NULL_TREE,
			     NULL_TREE, asm_name);

	      if (c_parser_next_token_is_keyword (parser, RID_IN))
		{
		  if (d)
		    *objc_foreach_object_declaration = d;
		  else
		    *objc_foreach_object_declaration = error_mark_node;		    
		}
	    }
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    {
	      if (any_auto_type_p || specs->constexpr_p)
		{
		  error_at (here,
			    "%qs may only be used with a single declarator",
			    any_auto_type_p ? auto_type_keyword : "constexpr");
		  c_parser_skip_to_end_of_block_or_statement (parser);
		  return;
		}
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
		all_prefix_attrs = chainon (c_parser_gnu_attributes (parser),
					    prefix_attrs);
	      else
		all_prefix_attrs = prefix_attrs;
	      continue;
	    }
	  else if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      c_parser_consume_token (parser);
	      return;
	    }
	  else if (c_parser_next_token_is_keyword (parser, RID_IN))
	    {
	      /* This can only happen in Objective-C: we found the
		 'in' that terminates the declaration inside an
		 Objective-C foreach statement.  Do not consume the
		 token, so that the caller can use it to determine
		 that this indeed is a foreach context.  */
	      return;
	    }
	  else
	    {
	      c_parser_error (parser, "expected %<,%> or %<;%>");
	      c_parser_skip_to_end_of_block_or_statement (parser);
	      return;
	    }
	}
      else if (any_auto_type_p || specs->constexpr_p)
	{
	  error_at (here,
		    "%qs requires an initialized data declaration",
		    any_auto_type_p ? auto_type_keyword : "constexpr");
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      else if (!fndef_ok)
	{
	  c_parser_error (parser, "expected %<=%>, %<,%>, %<;%>, "
			  "%<asm%> or %<__attribute__%>");
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  return;
	}
      /* Function definition (nested or otherwise).  */
      if (nested)
	{
	  pedwarn (here, OPT_Wpedantic, "ISO C forbids nested functions");
	  c_push_function_context ();
	}
      if (!start_function (specs, declarator, all_prefix_attrs))
	{
	  /* At this point we've consumed:
	       declaration-specifiers declarator
	     and the next token isn't CPP_EQ, CPP_COMMA, CPP_SEMICOLON,
	     RID_ASM, RID_ATTRIBUTE, or RID_IN,
	     but the
	       declaration-specifiers declarator
	     aren't grokkable as a function definition, so we have
	     an error.  */
	  gcc_assert (!c_parser_next_token_is (parser, CPP_SEMICOLON));
	  if (c_parser_next_token_starts_declspecs (parser))
	    {
	      /* If we have
		   declaration-specifiers declarator decl-specs
		 then assume we have a missing semicolon, which would
		 give us:
		   declaration-specifiers declarator  decl-specs
						    ^
						    ;
		   <~~~~~~~~~ declaration ~~~~~~~~~~>
		 Use c_parser_require to get an error with a fix-it hint.  */
	      c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>");
	      parser->error = false;
	    }
	  else
	    {
	      /* This can appear in many cases looking nothing like a
		 function definition, so we don't give a more specific
		 error suggesting there was one.  */
	      c_parser_error (parser, "expected %<=%>, %<,%>, %<;%>, %<asm%> "
			      "or %<__attribute__%>");
	    }
	  if (nested)
	    c_pop_function_context ();
	  break;
	}

      if (DECL_DECLARED_INLINE_P (current_function_decl))
        tv = TV_PARSE_INLINE;
      else
        tv = TV_PARSE_FUNC;
      auto_timevar at (g_timer, tv);

      /* Parse old-style parameter declarations.  ??? Attributes are
	 not allowed to start declaration specifiers here because of a
	 syntax conflict between a function declaration with attribute
	 suffix and a function definition with an attribute prefix on
	 first old-style parameter declaration.  Following the old
	 parser, they are not accepted on subsequent old-style
	 parameter declarations either.  However, there is no
	 ambiguity after the first declaration, nor indeed on the
	 first as long as we don't allow postfix attributes after a
	 declarator with a nonempty identifier list in a definition;
	 and postfix attributes have never been accepted here in
	 function definitions either.  */
      int save_debug_nonbind_markers_p = debug_nonbind_markers_p;
      debug_nonbind_markers_p = 0;
      while (c_parser_next_token_is_not (parser, CPP_EOF)
	     && c_parser_next_token_is_not (parser, CPP_OPEN_BRACE))
	c_parser_declaration_or_fndef (parser, false, false, false,
				       true, false);
      debug_nonbind_markers_p = save_debug_nonbind_markers_p;
      store_parm_decls ();
      if (omp_declare_simd_clauses)
	c_finish_omp_declare_simd (parser, current_function_decl, NULL_TREE,
				   omp_declare_simd_clauses);
      if (oacc_routine_data)
	c_finish_oacc_routine (oacc_routine_data, current_function_decl, true);
      location_t startloc = c_parser_peek_token (parser)->location;
      DECL_STRUCT_FUNCTION (current_function_decl)->function_start_locus
	= startloc;
      location_t endloc = startloc;

// Modified
// Possible BUG

	fnbody = c_parser_compound_statement (parser, &endloc);
      tree fndecl = current_function_decl;
      if (nested)
	{
	  tree decl = current_function_decl;
	  /* Mark nested functions as needing static-chain initially.
	     lower_nested_functions will recompute it but the
	     DECL_STATIC_CHAIN flag is also used before that happens,
	     by initializer_constant_valid_p.  See gcc.dg/nested-fn-2.c.  */
	  DECL_STATIC_CHAIN (decl) = 1;
	  add_stmt (fnbody);
	  finish_function (endloc);
	  c_pop_function_context ();
	  add_stmt (build_stmt (DECL_SOURCE_LOCATION (decl), DECL_EXPR, decl));
	}
      else
	{
	  if (fnbody)
	    add_stmt (fnbody);
	  finish_function (endloc);
	}
      /* Get rid of the empty stmt list for GIMPLE/RTL.  */
      if (specs->declspec_il != cdil_none)
	DECL_SAVED_TREE (fndecl) = NULL_TREE;

      break;
    }
}

/* Parse an asm-definition (asm() outside a function body).  This is a
   GNU extension.

   asm-definition:
     simple-asm-expr ;
*/

static void
c_parser_asm_definition (c_parser *parser)
{
  tree asm_str = c_parser_simple_asm_expr (parser);
  if (asm_str)
    symtab->finalize_toplevel_asm (asm_str);
  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
}

/* Parse a static assertion (C11 6.7.10).

   static_assert-declaration:
     static_assert-declaration-no-semi ;
*/

static void
c_parser_static_assert_declaration (c_parser *parser)
{
  c_parser_static_assert_declaration_no_semi (parser);
  if (parser->error
      || !c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
    c_parser_skip_to_end_of_block_or_statement (parser);
}

/* Parse a static assertion (C11 6.7.10), without the trailing
   semicolon.

   static_assert-declaration-no-semi:
     _Static_assert ( constant-expression , string-literal )

   C2X:
   static_assert-declaration-no-semi:
     _Static_assert ( constant-expression )
*/

static void
c_parser_static_assert_declaration_no_semi (c_parser *parser)
{
  location_t assert_loc, value_loc;
  tree value;
  tree string = NULL_TREE;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_STATIC_ASSERT));
  tree spelling = c_parser_peek_token (parser)->value;
  assert_loc = c_parser_peek_token (parser)->location;
  if (flag_isoc99)
    pedwarn_c99 (assert_loc, OPT_Wpedantic,
		 "ISO C99 does not support %qE", spelling);
  else
    pedwarn_c99 (assert_loc, OPT_Wpedantic,
		 "ISO C90 does not support %qE", spelling);
  c_parser_consume_token (parser);
  matching_parens parens;
  if (!parens.require_open (parser))
    return;
  location_t value_tok_loc = c_parser_peek_token (parser)->location;
  value = convert_lvalue_to_rvalue (value_tok_loc,
				    c_parser_expr_no_commas (parser, NULL),
				    true, true).value;
  value_loc = EXPR_LOC_OR_LOC (value, value_tok_loc);
  if (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_STRING:
	case CPP_STRING16:
	case CPP_STRING32:
	case CPP_WSTRING:
	case CPP_UTF8STRING:
	  string = c_parser_string_literal (parser, false, true).value;
	  break;
	default:
	  c_parser_error (parser, "expected string literal");
	  return;
	}
    }
  else if (flag_isoc11)
    /* If pedantic for pre-C11, the use of _Static_assert itself will
       have been diagnosed, so do not also diagnose the use of this
       new C2X feature of _Static_assert.  */
    pedwarn_c11 (assert_loc, OPT_Wpedantic,
		 "ISO C11 does not support omitting the string in "
		 "%qE", spelling);
  parens.require_close (parser);

  if (!INTEGRAL_TYPE_P (TREE_TYPE (value)))
    {
      error_at (value_loc, "expression in static assertion is not an integer");
      return;
    }
  if (TREE_CODE (value) != INTEGER_CST)
    {
      value = c_fully_fold (value, false, NULL);
      /* Strip no-op conversions.  */
      STRIP_TYPE_NOPS (value);
      if (TREE_CODE (value) == INTEGER_CST)
	pedwarn (value_loc, OPT_Wpedantic, "expression in static assertion "
		 "is not an integer constant expression");
    }
  if (TREE_CODE (value) != INTEGER_CST)
    {
      error_at (value_loc, "expression in static assertion is not constant");
      return;
    }
  constant_expression_warning (value);
  if (integer_zerop (value))
    {
      if (string)
	error_at (assert_loc, "static assertion failed: %E", string);
      else
	error_at (assert_loc, "static assertion failed");
    }
}

/* Parse some declaration specifiers (possibly none) (C90 6.5, C99
   6.7, C11 6.7), adding them to SPECS (which may already include some).
   Storage class specifiers are accepted iff SCSPEC_OK; type
   specifiers are accepted iff TYPESPEC_OK; alignment specifiers are
   accepted iff ALIGNSPEC_OK; gnu-attributes are accepted at the start
   iff START_ATTR_OK; __auto_type is accepted iff AUTO_TYPE_OK.  In
   addition to the syntax shown, standard attributes are accepted at
   the start iff START_STD_ATTR_OK and at the end iff END_STD_ATTR_OK;
   unlike gnu-attributes, they are not accepted in the middle of the
   list.  (This combines various different syntax productions in the C
   standard, and in some cases gnu-attributes and standard attributes
   at the start may already have been parsed before this function is
   called.)

   declaration-specifiers:
     storage-class-specifier declaration-specifiers[opt]
     type-specifier declaration-specifiers[opt]
     type-qualifier declaration-specifiers[opt]
     function-specifier declaration-specifiers[opt]
     alignment-specifier declaration-specifiers[opt]

   Function specifiers (inline) are from C99, and are currently
   handled as storage class specifiers, as is __thread.  Alignment
   specifiers are from C11.

   C90 6.5.1, C99 6.7.1, C11 6.7.1:
   storage-class-specifier:
     typedef
     extern
     static
     auto
     register
     _Thread_local

   (_Thread_local is new in C11.)

   C99 6.7.4, C11 6.7.4:
   function-specifier:
     inline
     _Noreturn

   (_Noreturn is new in C11.)

   C90 6.5.2, C99 6.7.2, C11 6.7.2:
   type-specifier:
     void
     char
     short
     int
     long
     float
     double
     signed
     unsigned
     _Bool
     _Complex
     [_Imaginary removed in C99 TC2]
     struct-or-union-specifier
     enum-specifier
     typedef-name
     atomic-type-specifier

   (_Bool and _Complex are new in C99.)
   (atomic-type-specifier is new in C11.)

   C90 6.5.3, C99 6.7.3, C11 6.7.3:

   type-qualifier:
     const
     restrict
     volatile
     address-space-qualifier
     _Atomic

   (restrict is new in C99.)
   (_Atomic is new in C11.)

   GNU extensions:

   declaration-specifiers:
     gnu-attributes declaration-specifiers[opt]

   type-qualifier:
     address-space

   address-space:
     identifier recognized by the target

   storage-class-specifier:
     __thread

   type-specifier:
     typeof-specifier
     __auto_type
     __intN
     _Decimal32
     _Decimal64
     _Decimal128
     _Fract
     _Accum
     _Sat

  (_Fract, _Accum, and _Sat are new from ISO/IEC DTR 18037:
   http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1169.pdf)

   atomic-type-specifier
    _Atomic ( type-name )

   Objective-C:

   type-specifier:
     class-name objc-protocol-refs[opt]
     typedef-name objc-protocol-refs
     objc-protocol-refs
*/

void
c_parser_declspecs (c_parser *parser, struct c_declspecs *specs,
		    bool scspec_ok, bool typespec_ok, bool start_attr_ok,
		    bool alignspec_ok, bool auto_type_ok,
		    bool start_std_attr_ok, bool end_std_attr_ok,
		    enum c_lookahead_kind la)
{
  bool attrs_ok = start_attr_ok;
  bool seen_type = specs->typespec_kind != ctsk_none;

  if (!typespec_ok)
    gcc_assert (la == cla_prefer_id);

  if (start_std_attr_ok
      && c_parser_nth_token_starts_std_attributes (parser, 1))
    {
      gcc_assert (!specs->non_std_attrs_seen_p);
      location_t loc = c_parser_peek_token (parser)->location;
      tree attrs = c_parser_std_attribute_specifier_sequence (parser);
      declspecs_add_attrs (loc, specs, attrs);
      specs->non_std_attrs_seen_p = false;
    }

  while (c_parser_next_token_is (parser, CPP_NAME)
	 || c_parser_next_token_is (parser, CPP_KEYWORD)
	 || (c_dialect_objc () && c_parser_next_token_is (parser, CPP_LESS)))
    {
      struct c_typespec t;
      tree attrs;
      tree align;
      location_t loc = c_parser_peek_token (parser)->location;

      /* If we cannot accept a type, exit if the next token must start
	 one.  Also, if we already have seen a tagged definition,
	 a typename would be an error anyway and likely the user
	 has simply forgotten a semicolon, so we exit.  */
      if ((!typespec_ok || specs->typespec_kind == ctsk_tagdef)
	  && c_parser_next_tokens_start_typename (parser, la)
	  && !c_parser_next_token_is_qualifier (parser)
	  && !c_parser_next_token_is_keyword (parser, RID_ALIGNAS))
	break;

      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  c_token *name_token = c_parser_peek_token (parser);
	  tree value = name_token->value;
	  c_id_kind kind = name_token->id_kind;

	  if (kind == C_ID_ADDRSPACE)
	    {
	      addr_space_t as
		= name_token->keyword - RID_FIRST_ADDR_SPACE;
	      declspecs_add_addrspace (name_token->location, specs, as);
	      c_parser_consume_token (parser);
	      attrs_ok = true;
	      continue;
	    }

	  gcc_assert (!c_parser_next_token_is_qualifier (parser));

	  /* If we cannot accept a type, and the next token must start one,
	     exit.  Do the same if we already have seen a tagged definition,
	     since it would be an error anyway and likely the user has simply
	     forgotten a semicolon.  */
	  if (seen_type || !c_parser_next_tokens_start_typename (parser, la))
	    break;

	  /* Now at an unknown typename (C_ID_ID), a C_ID_TYPENAME or
	     a C_ID_CLASSNAME.  */
	  c_parser_consume_token (parser);
	  seen_type = true;
	  attrs_ok = true;
	  if (kind == C_ID_ID)
	    {
	      error_at (loc, "unknown type name %qE", value);
	      t.kind = ctsk_typedef;
	      t.spec = error_mark_node;
	    }
	  else if (kind == C_ID_TYPENAME
	           && (!c_dialect_objc ()
	               || c_parser_next_token_is_not (parser, CPP_LESS)))
	    {
	      t.kind = ctsk_typedef;
	      /* For a typedef name, record the meaning, not the name.
		 In case of 'foo foo, bar;'.  */
	      t.spec = lookup_name (value);
	    }
// Modified
// Possible BUG

	  t.expr = NULL_TREE;
	  t.expr_const_operands = true;
	  t.has_enum_type_specifier = false;
	  declspecs_add_type (name_token->location, specs, t);
	  continue;
	}
// Modified
// Possible Bug

      gcc_assert (c_parser_next_token_is (parser, CPP_KEYWORD));
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_STATIC:
	case RID_EXTERN:
	case RID_REGISTER:
	case RID_TYPEDEF:
	case RID_INLINE:
	case RID_NORETURN:
	case RID_AUTO:
	case RID_THREAD:
	case RID_CONSTEXPR:
	  if (!scspec_ok)
	    goto out;
	  attrs_ok = true;
	  /* TODO: Distinguish between function specifiers (inline, noreturn)
	     and storage class specifiers, either here or in
	     declspecs_add_scspec.  */
	  declspecs_add_scspec (loc, specs,
				c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_AUTO_TYPE:
	  if (!auto_type_ok)
	    goto out;
	  /* Fall through.  */
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_SHORT:
	case RID_SIGNED:
	case RID_COMPLEX:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	CASE_RID_FLOATN_NX:
	case RID_BOOL:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_INT_N_0:
	case RID_INT_N_1:
	case RID_INT_N_2:
	case RID_INT_N_3:
	  if (!typespec_ok)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  if (c_dialect_objc ())
	    parser->objc_need_raw_identifier = true;
	  t.kind = ctsk_resword;
	  t.spec = c_parser_peek_token (parser)->value;
	  t.expr = NULL_TREE;
	  t.expr_const_operands = true;
	  t.has_enum_type_specifier = false;
	  declspecs_add_type (loc, specs, t);
	  c_parser_consume_token (parser);
	  break;
	case RID_ENUM:
	  if (!typespec_ok)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  t = c_parser_enum_specifier (parser);
          invoke_plugin_callbacks (PLUGIN_FINISH_TYPE, t.spec);
	  declspecs_add_type (loc, specs, t);
	  break;
	case RID_STRUCT:
	case RID_UNION:
	  if (!typespec_ok)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  t = c_parser_struct_or_union_specifier (parser);
          invoke_plugin_callbacks (PLUGIN_FINISH_TYPE, t.spec);
	  declspecs_add_type (loc, specs, t);
	  break;
	case RID_TYPEOF:
	case RID_TYPEOF_UNQUAL:
	  /* ??? The old parser rejected typeof after other type
	     specifiers, but is a syntax error the best way of
	     handling this?  */
	  if (!typespec_ok || seen_type)
	    goto out;
	  attrs_ok = true;
	  seen_type = true;
	  t = c_parser_typeof_specifier (parser);
	  declspecs_add_type (loc, specs, t);
	  break;
	case RID_ATOMIC:
	  /* C parser handling of Objective-C constructs needs
	     checking for correct lvalue-to-rvalue conversions, and
	     the code in build_modify_expr handling various
	     Objective-C cases, and that in build_unary_op handling
	     Objective-C cases for increment / decrement, also needs
	     updating; uses of TYPE_MAIN_VARIANT in objc_compare_types
	     and objc_types_are_equivalent may also need updates.  */
	  if (c_dialect_objc ())
	    sorry ("%<_Atomic%> in Objective-C");
	  if (flag_isoc99)
	    pedwarn_c99 (loc, OPT_Wpedantic,
			 "ISO C99 does not support the %<_Atomic%> qualifier");
	  else
	    pedwarn_c99 (loc, OPT_Wpedantic,
			 "ISO C90 does not support the %<_Atomic%> qualifier");
	  attrs_ok = true;
	  tree value;
	  value = c_parser_peek_token (parser)->value;
	  c_parser_consume_token (parser);
	  if (typespec_ok && c_parser_next_token_is (parser, CPP_OPEN_PAREN))
	    {
	      /* _Atomic ( type-name ).  */
	      seen_type = true;
	      c_parser_consume_token (parser);
	      struct c_type_name *type = c_parser_type_name (parser);
	      t.kind = ctsk_typeof;
	      t.spec = error_mark_node;
	      t.expr = NULL_TREE;
	      t.expr_const_operands = true;
	      t.has_enum_type_specifier = false;
	      if (type != NULL)
		t.spec = groktypename (type, &t.expr,
				       &t.expr_const_operands);
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
					 "expected %<)%>");
	      if (t.spec != error_mark_node)
		{
		  if (TREE_CODE (t.spec) == ARRAY_TYPE)
		    error_at (loc, "%<_Atomic%>-qualified array type");
		  else if (TREE_CODE (t.spec) == FUNCTION_TYPE)
		    error_at (loc, "%<_Atomic%>-qualified function type");
		  else if (TYPE_QUALS (t.spec) != TYPE_UNQUALIFIED)
		    error_at (loc, "%<_Atomic%> applied to a qualified type");
		  else
		    t.spec = c_build_qualified_type (t.spec, TYPE_QUAL_ATOMIC);
		}
	      declspecs_add_type (loc, specs, t);
	    }
	  else
	    declspecs_add_qual (loc, specs, value);
	  break;
	case RID_CONST:
	case RID_VOLATILE:
	case RID_RESTRICT:
	  attrs_ok = true;
	  declspecs_add_qual (loc, specs, c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	case RID_ATTRIBUTE:
	  if (!attrs_ok)
	    goto out;
	  attrs = c_parser_gnu_attributes (parser);
	  declspecs_add_attrs (loc, specs, attrs);
	  break;
	case RID_ALIGNAS:
	  if (!alignspec_ok)
	    goto out;
	  align = c_parser_alignas_specifier (parser);
	  declspecs_add_alignas (loc, specs, align);
	  break;
	default:
	  goto out;
	}
    }
 out:
  if (end_std_attr_ok
      && c_parser_nth_token_starts_std_attributes (parser, 1))
    specs->postfix_attrs = c_parser_std_attribute_specifier_sequence (parser);
}

/* Parse an enum specifier (C90 6.5.2.2, C99 6.7.2.2, C11 6.7.2.2).

   enum-specifier:
     enum gnu-attributes[opt] identifier[opt] enum-type-specifier[opt]
       { enumerator-list } gnu-attributes[opt]
     enum gnu-attributes[opt] identifier[opt] enum-type-specifier[opt]
       { enumerator-list , } gnu-attributes[opt] enum-type-specifier[opt]
     enum gnu-attributes[opt] identifier

   The form with trailing comma is new in C99; enum-type-specifiers
   are new in C2x.  The forms with gnu-attributes are GNU extensions.
   In GNU C, we accept any expression without commas in the syntax
   (assignment expressions, not just conditional expressions);
   assignment expressions will be diagnosed as non-constant.

   enum-type-specifier:
     : specifier-qualifier-list

   enumerator-list:
     enumerator
     enumerator-list , enumerator

   enumerator:
     enumeration-constant attribute-specifier-sequence[opt]
     enumeration-constant attribute-specifier-sequence[opt]
       = constant-expression

   GNU Extensions:

   enumerator:
     enumeration-constant attribute-specifier-sequence[opt] gnu-attributes[opt]
     enumeration-constant attribute-specifier-sequence[opt] gnu-attributes[opt]
       = constant-expression

*/

static struct c_typespec
c_parser_enum_specifier (c_parser *parser)
{
  struct c_typespec ret;
  bool have_std_attrs;
  tree std_attrs = NULL_TREE;
  tree attrs;
  tree ident = NULL_TREE;
  tree fixed_underlying_type = NULL_TREE;
  location_t enum_loc;
  location_t ident_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ENUM));
  c_parser_consume_token (parser);
  have_std_attrs = c_parser_nth_token_starts_std_attributes (parser, 1);
  if (have_std_attrs)
    std_attrs = c_parser_std_attribute_specifier_sequence (parser);
  attrs = c_parser_gnu_attributes (parser);
  enum_loc = c_parser_peek_token (parser)->location;
  /* Set the location in case we create a decl now.  */
  c_parser_set_source_position_from_token (c_parser_peek_token (parser));
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      ident = c_parser_peek_token (parser)->value;
      ident_loc = c_parser_peek_token (parser)->location;
      enum_loc = ident_loc;
      c_parser_consume_token (parser);
    }
  if (c_parser_next_token_is (parser, CPP_COLON)
      /* Distinguish an enum-type-specifier from a bit-field
	 declaration of the form "enum e : constant-expression;".  */
      && c_token_starts_typename (c_parser_peek_2nd_token (parser)))
    {
      pedwarn_c11 (enum_loc, OPT_Wpedantic,
		   "ISO C does not support specifying %<enum%> underlying "
		   "types before C2X");
      if (ident)
	{
	  /* The tag is in scope during the enum-type-specifier (which
	     may refer to the tag inside typeof).  */
	  ret = parser_xref_tag (ident_loc, ENUMERAL_TYPE, ident,
				 have_std_attrs, std_attrs, true);
	  if (!ENUM_FIXED_UNDERLYING_TYPE_P (ret.spec))
	    error_at (enum_loc, "%<enum%> declared both with and without "
		      "fixed underlying type");
	}
      else
	{
	  /* There must be an enum definition, so this initialization
	     (to avoid possible warnings about uninitialized data)
	     will be replaced later (either with the results of that
	     definition, or with the results of error handling for the
	     case of no tag and no definition).  */
	  ret.spec = NULL_TREE;
	  ret.kind = ctsk_tagdef;
	  ret.expr = NULL_TREE;
	  ret.expr_const_operands = true;
	  ret.has_enum_type_specifier = true;
	}
      c_parser_consume_token (parser);
      struct c_declspecs *specs = build_null_declspecs ();
      c_parser_declspecs (parser, specs, false, true, false, false, false,
			  false, true, cla_prefer_id);
      finish_declspecs (specs);
      if (specs->default_int_p)
	error_at (enum_loc, "no %<enum%> underlying type specified");
      else if (TREE_CODE (specs->type) != INTEGER_TYPE
	       && TREE_CODE (specs->type) != BOOLEAN_TYPE)
	{
	  error_at (enum_loc, "invalid %<enum%> underlying type");
	  specs->type = integer_type_node;
	}
      else if (specs->restrict_p)
	error_at (enum_loc, "invalid use of %<restrict%>");
      fixed_underlying_type = TYPE_MAIN_VARIANT (specs->type);
      if (ident)
	{
	  /* The type specified must be consistent with any previously
	     specified underlying type.  If this is a newly declared
	     type, it is now a complete type.  */
	  if (ENUM_FIXED_UNDERLYING_TYPE_P (ret.spec)
	      && ENUM_UNDERLYING_TYPE (ret.spec) == NULL_TREE)
	    {
	      TYPE_MIN_VALUE (ret.spec) =
		TYPE_MIN_VALUE (fixed_underlying_type);
	      TYPE_MAX_VALUE (ret.spec) =
		TYPE_MAX_VALUE (fixed_underlying_type);
	      TYPE_UNSIGNED (ret.spec) = TYPE_UNSIGNED (fixed_underlying_type);
	      SET_TYPE_ALIGN (ret.spec, TYPE_ALIGN (fixed_underlying_type));
	      TYPE_SIZE (ret.spec) = NULL_TREE;
	      TYPE_PRECISION (ret.spec) =
		TYPE_PRECISION (fixed_underlying_type);
	      ENUM_UNDERLYING_TYPE (ret.spec) = fixed_underlying_type;
	      layout_type (ret.spec);
	    }
	  else if (ENUM_FIXED_UNDERLYING_TYPE_P (ret.spec)
		   && !comptypes (fixed_underlying_type,
				  ENUM_UNDERLYING_TYPE (ret.spec)))
	    {
	      error_at (enum_loc, "%<enum%> underlying type incompatible with "
			"previous declaration");
	      fixed_underlying_type = ENUM_UNDERLYING_TYPE (ret.spec);
	    }
	}
    }
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    {
      /* Parse an enum definition.  */
      struct c_enum_contents the_enum;
      tree type;
      tree postfix_attrs;
      /* We chain the enumerators in reverse order, then put them in
	 forward order at the end.  */
      tree values;
      timevar_push (TV_PARSE_ENUM);
      type = start_enum (enum_loc, &the_enum, ident, fixed_underlying_type);
      values = NULL_TREE;
      c_parser_consume_token (parser);
      while (true)
	{
	  tree enum_id;
	  tree enum_value;
	  tree enum_decl;
	  bool seen_comma;
	  c_token *token;
	  location_t comma_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
	  location_t decl_loc, value_loc;
	  if (c_parser_next_token_is_not (parser, CPP_NAME))
	    {
	      /* Give a nicer error for "enum {}".  */
	      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE)
		  && !parser->error)
		{
		  error_at (c_parser_peek_token (parser)->location,
			    "empty enum is invalid");
		  parser->error = true;
		}
	      else
		c_parser_error (parser, "expected identifier");
	      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
	      values = error_mark_node;
	      break;
	    }
	  token = c_parser_peek_token (parser);
	  enum_id = token->value;
	  /* Set the location in case we create a decl now.  */
	  c_parser_set_source_position_from_token (token);
	  decl_loc = value_loc = token->location;
	  c_parser_consume_token (parser);
	  /* Parse any specified attributes.  */
	  tree std_attrs = NULL_TREE;
	  if (c_parser_nth_token_starts_std_attributes (parser, 1))
	    std_attrs = c_parser_std_attribute_specifier_sequence (parser);
	  tree enum_attrs = chainon (std_attrs,
				     c_parser_gnu_attributes (parser));
	  if (c_parser_next_token_is (parser, CPP_EQ))
	    {
	      c_parser_consume_token (parser);
	      value_loc = c_parser_peek_token (parser)->location;
	      enum_value = convert_lvalue_to_rvalue (value_loc,
						     (c_parser_expr_no_commas
						      (parser, NULL)),
						     true, true).value;
	    }
	  else
	    enum_value = NULL_TREE;
	  enum_decl = build_enumerator (decl_loc, value_loc,
					&the_enum, enum_id, enum_value);
	  if (enum_attrs)
	    decl_attributes (&TREE_PURPOSE (enum_decl), enum_attrs, 0);
	  TREE_CHAIN (enum_decl) = values;
	  values = enum_decl;
	  seen_comma = false;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    {
	      comma_loc = c_parser_peek_token (parser)->location;
	      seen_comma = true;
	      c_parser_consume_token (parser);
	    }
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    {
	      if (seen_comma)
		pedwarn_c90 (comma_loc, OPT_Wpedantic,
			     "comma at end of enumerator list");
	      c_parser_consume_token (parser);
	      break;
	    }
	  if (!seen_comma)
	    {
	      c_parser_error (parser, "expected %<,%> or %<}%>");
	      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
	      values = error_mark_node;
	      break;
	    }
	}
      postfix_attrs = c_parser_gnu_attributes (parser);
      ret.spec = finish_enum (type, nreverse (values),
			      chainon (std_attrs,
				       chainon (attrs, postfix_attrs)));
      ret.kind = ctsk_tagdef;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      ret.has_enum_type_specifier = fixed_underlying_type != NULL_TREE;
      timevar_pop (TV_PARSE_ENUM);
      return ret;
    }
  else if (!ident)
    {
      c_parser_error (parser, "expected %<{%>");
      ret.spec = error_mark_node;
      ret.kind = ctsk_tagref;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      ret.has_enum_type_specifier = false;
      return ret;
    }
  /* Attributes may only appear when the members are defined or in
     certain forward declarations (treat enum forward declarations in
     GNU C analogously to struct and union forward declarations in
     standard C).  */
  if (have_std_attrs && c_parser_next_token_is_not (parser, CPP_SEMICOLON))
    c_parser_error (parser, "expected %<;%>");
  if (fixed_underlying_type == NULL_TREE)
    {
      ret = parser_xref_tag (ident_loc, ENUMERAL_TYPE, ident, have_std_attrs,
			     std_attrs, false);
      /* In ISO C, enumerated types without a fixed underlying type
	 can be referred to only if already defined.  */
      if (pedantic && !COMPLETE_TYPE_P (ret.spec))
	{
	  gcc_assert (ident);
	  pedwarn (enum_loc, OPT_Wpedantic,
		   "ISO C forbids forward references to %<enum%> types");
	}
    }
  return ret;
}

/* Parse a struct or union specifier (C90 6.5.2.1, C99 6.7.2.1, C11 6.7.2.1).

   struct-or-union-specifier:
     struct-or-union attribute-specifier-sequence[opt] gnu-attributes[opt]
       identifier[opt] { struct-contents } gnu-attributes[opt]
     struct-or-union attribute-specifier-sequence[opt] gnu-attributes[opt]
       identifier

   struct-contents:
     struct-declaration-list

   struct-declaration-list:
     struct-declaration ;
     struct-declaration-list struct-declaration ;

   GNU extensions:

   struct-contents:
     empty
     struct-declaration
     struct-declaration-list struct-declaration

   struct-declaration-list:
     struct-declaration-list ;
     ;

   (Note that in the syntax here, unlike that in ISO C, the semicolons
   are included here rather than in struct-declaration, in order to
   describe the syntax with extra semicolons and missing semicolon at
   end.)

   Objective-C:

   struct-declaration-list:
     @defs ( class-name )

   (Note this does not include a trailing semicolon, but can be
   followed by further declarations, and gets a pedwarn-if-pedantic
   when followed by a semicolon.)  */

static struct c_typespec
c_parser_struct_or_union_specifier (c_parser *parser)
{
  struct c_typespec ret;
  bool have_std_attrs;
  tree std_attrs = NULL_TREE;
  tree attrs;
  tree ident = NULL_TREE;
  location_t struct_loc;
  location_t ident_loc = UNKNOWN_LOCATION;
  enum tree_code code;
  switch (c_parser_peek_token (parser)->keyword)
    {
    case RID_STRUCT:
      code = RECORD_TYPE;
      break;
    case RID_UNION:
      code = UNION_TYPE;
      break;
    default:
      gcc_unreachable ();
    }
  struct_loc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);
  have_std_attrs = c_parser_nth_token_starts_std_attributes (parser, 1);
  if (have_std_attrs)
    std_attrs = c_parser_std_attribute_specifier_sequence (parser);
  attrs = c_parser_gnu_attributes (parser);

  /* Set the location in case we create a decl now.  */
  c_parser_set_source_position_from_token (c_parser_peek_token (parser));

  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      ident = c_parser_peek_token (parser)->value;
      ident_loc = c_parser_peek_token (parser)->location;
      struct_loc = ident_loc;
      c_parser_consume_token (parser);
    }
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    {
      /* Parse a struct or union definition.  Start the scope of the
	 tag before parsing components.  */
      class c_struct_parse_info *struct_info;
      tree type = start_struct (struct_loc, code, ident, &struct_info);
      tree postfix_attrs;
      /* We chain the components in reverse order, then put them in
	 forward order at the end.  Each struct-declaration may
	 declare multiple components (comma-separated), so we must use
	 chainon to join them, although when parsing each
	 struct-declaration we can use TREE_CHAIN directly.

	 The theory behind all this is that there will be more
	 semicolon separated fields than comma separated fields, and
	 so we'll be minimizing the number of node traversals required
	 by chainon.  */
      tree contents;
      tree expr = NULL;
      timevar_push (TV_PARSE_STRUCT);
      contents = NULL_TREE;
      c_parser_consume_token (parser);
// Modified for Scpel
// possible BUG!
    end_at_defs:
      /* Parse the struct-declarations and semicolons.  Problems with
	 semicolons are diagnosed here; empty structures are diagnosed
	 elsewhere.  */
      while (true)
	{
	  tree decls;
	  /* Parse any stray semicolon.  */
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      location_t semicolon_loc
		= c_parser_peek_token (parser)->location;
	      gcc_rich_location richloc (semicolon_loc);
	      richloc.add_fixit_remove ();
	      pedwarn (&richloc, OPT_Wpedantic,
		       "extra semicolon in struct or union specified");
	      c_parser_consume_token (parser);
	      continue;
	    }
	  /* Stop if at the end of the struct or union contents.  */
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    {
	      c_parser_consume_token (parser);
	      break;
	    }
	  /* Accept #pragmas at struct scope.  */
	  if (c_parser_next_token_is (parser, CPP_PRAGMA))
	    {
	      c_parser_pragma (parser, pragma_struct, NULL);
	      continue;
	    }
	  /* Parse some comma-separated declarations, but not the
	     trailing semicolon if any.  */
	  decls = c_parser_struct_declaration (parser, &expr);
	  contents = chainon (decls, contents);
	  /* If no semicolon follows, either we have a parse error or
	     are at the end of the struct or union and should
	     pedwarn.  */
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    c_parser_consume_token (parser);
	  else
	    {
	      if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
		pedwarn (c_parser_peek_token (parser)->location, 0,
			 "no semicolon at end of struct or union");
	      else if (parser->error
		       || !c_parser_next_token_starts_declspecs (parser))
		{
		  c_parser_error (parser, "expected %<;%>");
		  c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
		  break;
		}

	      /* If we come here, we have already emitted an error
		 for an expected `;', identifier or `(', and we also
	         recovered already.  Go on with the next field. */
	    }
	}
      postfix_attrs = c_parser_gnu_attributes (parser);
      ret.spec = finish_struct (struct_loc, type, nreverse (contents),
				chainon (std_attrs,
					 chainon (attrs, postfix_attrs)),
				struct_info);
      ret.kind = ctsk_tagdef;
      ret.expr = expr;
      ret.expr_const_operands = true;
      ret.has_enum_type_specifier = false;
      timevar_pop (TV_PARSE_STRUCT);
      return ret;
    }
  else if (!ident)
    {
      c_parser_error (parser, "expected %<{%>");
      ret.spec = error_mark_node;
      ret.kind = ctsk_tagref;
      ret.expr = NULL_TREE;
      ret.expr_const_operands = true;
      ret.has_enum_type_specifier = false;
      return ret;
    }
  /* Attributes may only appear when the members are defined or in
     certain forward declarations.  */
  if (have_std_attrs && c_parser_next_token_is_not (parser, CPP_SEMICOLON))
    c_parser_error (parser, "expected %<;%>");
  /* ??? Existing practice is that GNU attributes are ignored after
     the struct or union keyword when not defining the members.  */
  ret = parser_xref_tag (ident_loc, code, ident, have_std_attrs, std_attrs,
			 false);
  return ret;
}

/* Parse a struct-declaration (C90 6.5.2.1, C99 6.7.2.1, C11 6.7.2.1),
   *without* the trailing semicolon.

   struct-declaration:
     attribute-specifier-sequence[opt] specifier-qualifier-list
       attribute-specifier-sequence[opt] struct-declarator-list
     static_assert-declaration-no-semi

   specifier-qualifier-list:
     type-specifier specifier-qualifier-list[opt]
     type-qualifier specifier-qualifier-list[opt]
     alignment-specifier specifier-qualifier-list[opt]
     gnu-attributes specifier-qualifier-list[opt]

   struct-declarator-list:
     struct-declarator
     struct-declarator-list , gnu-attributes[opt] struct-declarator

   struct-declarator:
     declarator gnu-attributes[opt]
     declarator[opt] : constant-expression gnu-attributes[opt]

   GNU extensions:

   struct-declaration:
     __extension__ struct-declaration
     specifier-qualifier-list

   Unlike the ISO C syntax, semicolons are handled elsewhere.  The use
   of gnu-attributes where shown is a GNU extension.  In GNU C, we accept
   any expression without commas in the syntax (assignment
   expressions, not just conditional expressions); assignment
   expressions will be diagnosed as non-constant.  */

static tree
c_parser_struct_declaration (c_parser *parser, tree *expr)
{
  struct c_declspecs *specs;
  tree prefix_attrs;
  tree all_prefix_attrs;
  tree decls;
  location_t decl_loc;
  if (c_parser_next_token_is_keyword (parser, RID_EXTENSION))
    {
      int ext;
      tree decl;
      ext = disable_extension_diagnostics ();
      c_parser_consume_token (parser);
      decl = c_parser_struct_declaration (parser, expr);
      restore_extension_diagnostics (ext);
      return decl;
    }
  if (c_parser_next_token_is_keyword (parser, RID_STATIC_ASSERT))
    {
      c_parser_static_assert_declaration_no_semi (parser);
      return NULL_TREE;
    }
  specs = build_null_declspecs ();
  decl_loc = c_parser_peek_token (parser)->location;
  /* Strictly by the standard, we shouldn't allow _Alignas here,
     but it appears to have been intended to allow it there, so
     we're keeping it as it is until WG14 reaches a conclusion
     of N1731.
     <http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1731.pdf>  */
  c_parser_declspecs (parser, specs, false, true, true,
		      true, false, true, true, cla_nonabstract_decl);
  if (parser->error)
    return NULL_TREE;
  if (!specs->declspecs_seen_p)
    {
      c_parser_error (parser, "expected specifier-qualifier-list");
      return NULL_TREE;
    }
  finish_declspecs (specs);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON)
      || c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      tree ret;
      if (specs->typespec_kind == ctsk_none)
	{
	  pedwarn (decl_loc, OPT_Wpedantic,
		   "ISO C forbids member declarations with no members");
	  shadow_tag_warned (specs, pedantic);
	  ret = NULL_TREE;
	}
      else
	{
	  /* Support for unnamed structs or unions as members of
	     structs or unions (which is [a] useful and [b] supports
	     MS P-SDK).  */
	  tree attrs = NULL;

	  ret = grokfield (c_parser_peek_token (parser)->location,
			   build_id_declarator (NULL_TREE), specs,
			   NULL_TREE, &attrs, expr);
	  if (ret)
	    decl_attributes (&ret, attrs, 0);
	}
      return ret;
    }

  /* Provide better error recovery.  Note that a type name here is valid,
     and will be treated as a field name.  */
  if (specs->typespec_kind == ctsk_tagdef
      && TREE_CODE (specs->type) != ENUMERAL_TYPE
      && c_parser_next_token_starts_declspecs (parser)
      && !c_parser_next_token_is (parser, CPP_NAME))
    {
      c_parser_error (parser, "expected %<;%>, identifier or %<(%>");
      parser->error = false;
      return NULL_TREE;
    }

  pending_xref_error ();
  prefix_attrs = specs->attrs;
  all_prefix_attrs = prefix_attrs;
  specs->attrs = NULL_TREE;
  decls = NULL_TREE;
  while (true)
    {
      /* Declaring one or more declarators or un-named bit-fields.  */
      struct c_declarator *declarator;
      bool dummy = false;
      if (c_parser_next_token_is (parser, CPP_COLON))
	declarator = build_id_declarator (NULL_TREE);
      else
	declarator = c_parser_declarator (parser,
					  specs->typespec_kind != ctsk_none,
					  C_DTR_NORMAL, &dummy);
      if (declarator == NULL)
	{
	  c_parser_skip_to_end_of_block_or_statement (parser);
	  break;
	}
      if (c_parser_next_token_is (parser, CPP_COLON)
	  || c_parser_next_token_is (parser, CPP_COMMA)
	  || c_parser_next_token_is (parser, CPP_SEMICOLON)
	  || c_parser_next_token_is (parser, CPP_CLOSE_BRACE)
	  || c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	{
	  tree postfix_attrs = NULL_TREE;
	  tree width = NULL_TREE;
	  tree d;
	  if (c_parser_next_token_is (parser, CPP_COLON))
	    {
	      c_parser_consume_token (parser);
	      location_t loc = c_parser_peek_token (parser)->location;
	      width = convert_lvalue_to_rvalue (loc,
						(c_parser_expr_no_commas
						 (parser, NULL)),
						true, true).value;
	    }
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    postfix_attrs = c_parser_gnu_attributes (parser);
	  d = grokfield (c_parser_peek_token (parser)->location,
			 declarator, specs, width, &all_prefix_attrs, expr);
	  decl_attributes (&d, chainon (postfix_attrs,
					all_prefix_attrs), 0);
	  DECL_CHAIN (d) = decls;
	  decls = d;
	  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
	    all_prefix_attrs = chainon (c_parser_gnu_attributes (parser),
					prefix_attrs);
	  else
	    all_prefix_attrs = prefix_attrs;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    c_parser_consume_token (parser);
	  else if (c_parser_next_token_is (parser, CPP_SEMICOLON)
		   || c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    {
	      /* Semicolon consumed in caller.  */
	      break;
	    }
	  else
	    {
	      c_parser_error (parser, "expected %<,%>, %<;%> or %<}%>");
	      break;
	    }
	}
      else
	{
	  c_parser_error (parser,
			  "expected %<:%>, %<,%>, %<;%>, %<}%> or "
			  "%<__attribute__%>");
	  break;
	}
    }
  return decls;
}

/* Parse a typeof specifier (a GNU extension adopted in C2X).

   typeof-specifier:
     typeof ( expression )
     typeof ( type-name )
     typeof_unqual ( expression )
     typeof_unqual ( type-name )
*/

static struct c_typespec
c_parser_typeof_specifier (c_parser *parser)
{
  bool is_unqual;
  bool is_std;
  struct c_typespec ret;
  ret.kind = ctsk_typeof;
  ret.spec = error_mark_node;
  ret.expr = NULL_TREE;
  ret.expr_const_operands = true;
  ret.has_enum_type_specifier = false;
  if (c_parser_next_token_is_keyword (parser, RID_TYPEOF))
    {
      is_unqual = false;
      tree spelling = c_parser_peek_token (parser)->value;
      is_std = (flag_isoc2x
		&& strcmp (IDENTIFIER_POINTER (spelling), "typeof") == 0);
    }
  else
    {
      gcc_assert (c_parser_next_token_is_keyword (parser, RID_TYPEOF_UNQUAL));
      is_unqual = true;
      is_std = true;
    }
  c_parser_consume_token (parser);
  c_inhibit_evaluation_warnings++;
  in_typeof++;
  matching_parens parens;
  if (!parens.require_open (parser))
    {
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      return ret;
    }
  if (c_parser_next_tokens_start_typename (parser, cla_prefer_id))
    {
      struct c_type_name *type = c_parser_type_name (parser);
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      if (type != NULL)
	{
	  ret.spec = groktypename (type, &ret.expr, &ret.expr_const_operands);
	  pop_maybe_used (c_type_variably_modified_p (ret.spec));
	}
    }
  else
    {
      bool was_vm;
      location_t here = c_parser_peek_token (parser)->location;
      struct c_expr expr = c_parser_expression (parser);
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      if (TREE_CODE (expr.value) == COMPONENT_REF
	  && DECL_C_BIT_FIELD (TREE_OPERAND (expr.value, 1)))
	error_at (here, "%<typeof%> applied to a bit-field");
      mark_exp_read (expr.value);
      ret.spec = TREE_TYPE (expr.value);
      was_vm = c_type_variably_modified_p (ret.spec);
      /* This is returned with the type so that when the type is
	 evaluated, this can be evaluated.  */
      if (was_vm)
	ret.expr = c_fully_fold (expr.value, false, &ret.expr_const_operands);
      pop_maybe_used (was_vm);
    }
  parens.skip_until_found_close (parser);
  if (ret.spec != error_mark_node)
    {
      if (is_unqual && TYPE_QUALS (ret.spec) != TYPE_UNQUALIFIED)
	ret.spec = TYPE_MAIN_VARIANT (ret.spec);
      if (is_std)
	{
	  /* In ISO C terms, _Noreturn is not part of the type of
	     expressions such as &abort, but in GCC it is represented
	     internally as a type qualifier.  */
	  if (TREE_CODE (ret.spec) == FUNCTION_TYPE
	      && TYPE_QUALS (ret.spec) != TYPE_UNQUALIFIED)
	    ret.spec = TYPE_MAIN_VARIANT (ret.spec);
	  else if (FUNCTION_POINTER_TYPE_P (ret.spec)
		   && TYPE_QUALS (TREE_TYPE (ret.spec)) != TYPE_UNQUALIFIED)
	    ret.spec
	      = build_pointer_type (TYPE_MAIN_VARIANT (TREE_TYPE (ret.spec)));
	}
    }
  return ret;
}

/* Parse an alignment-specifier.

   C11 6.7.5:

   alignment-specifier:
     _Alignas ( type-name )
     _Alignas ( constant-expression )
*/

static tree
c_parser_alignas_specifier (c_parser * parser)
{
  tree ret = error_mark_node;
  location_t loc = c_parser_peek_token (parser)->location;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ALIGNAS));
  tree spelling = c_parser_peek_token (parser)->value;
  c_parser_consume_token (parser);
  if (flag_isoc99)
    pedwarn_c99 (loc, OPT_Wpedantic,
		 "ISO C99 does not support %qE", spelling);
  else
    pedwarn_c99 (loc, OPT_Wpedantic,
		 "ISO C90 does not support %qE", spelling);
  matching_parens parens;
  if (!parens.require_open (parser))
    return ret;
  if (c_parser_next_tokens_start_typename (parser, cla_prefer_id))
    {
      struct c_type_name *type = c_parser_type_name (parser);
      if (type != NULL)
	ret = c_sizeof_or_alignof_type (loc, groktypename (type, NULL, NULL),
					false, true, 1);
    }
  else
    ret = convert_lvalue_to_rvalue (loc,
				    c_parser_expr_no_commas (parser, NULL),
				    true, true).value;
  parens.skip_until_found_close (parser);
  return ret;
}

/* Parse a declarator, possibly an abstract declarator (C90 6.5.4,
   6.5.5, C99 6.7.5, 6.7.6, C11 6.7.6, 6.7.7).  If TYPE_SEEN_P then
   a typedef name may be redeclared; otherwise it may not.  KIND
   indicates which kind of declarator is wanted.  Returns a valid
   declarator except in the case of a syntax error in which case NULL is
   returned.  *SEEN_ID is set to true if an identifier being declared is
   seen; this is used to diagnose bad forms of abstract array declarators
   and to determine whether an identifier list is syntactically permitted.

   declarator:
     pointer[opt] direct-declarator

   direct-declarator:
     identifier
     ( gnu-attributes[opt] declarator )
     direct-declarator array-declarator
     direct-declarator ( parameter-type-list )
     direct-declarator ( identifier-list[opt] )

   pointer:
     * type-qualifier-list[opt]
     * type-qualifier-list[opt] pointer

   type-qualifier-list:
     type-qualifier
     gnu-attributes
     type-qualifier-list type-qualifier
     type-qualifier-list gnu-attributes

   array-declarator:
     [ type-qualifier-list[opt] assignment-expression[opt] ]
     [ static type-qualifier-list[opt] assignment-expression ]
     [ type-qualifier-list static assignment-expression ]
     [ type-qualifier-list[opt] * ]

   parameter-type-list:
     parameter-list
     parameter-list , ...

   parameter-list:
     parameter-declaration
     parameter-list , parameter-declaration

   parameter-declaration:
     declaration-specifiers declarator gnu-attributes[opt]
     declaration-specifiers abstract-declarator[opt] gnu-attributes[opt]

   identifier-list:
     identifier
     identifier-list , identifier

   abstract-declarator:
     pointer
     pointer[opt] direct-abstract-declarator

   direct-abstract-declarator:
     ( gnu-attributes[opt] abstract-declarator )
     direct-abstract-declarator[opt] array-declarator
     direct-abstract-declarator[opt] ( parameter-type-list[opt] )

   GNU extensions:

   direct-declarator:
     direct-declarator ( parameter-forward-declarations
			 parameter-type-list[opt] )

   direct-abstract-declarator:
     direct-abstract-declarator[opt] ( parameter-forward-declarations
				       parameter-type-list[opt] )

   parameter-forward-declarations:
     parameter-list ;
     parameter-forward-declarations parameter-list ;

   The uses of gnu-attributes shown above are GNU extensions.

   Some forms of array declarator are not included in C99 in the
   syntax for abstract declarators; these are disallowed elsewhere.
   This may be a defect (DR#289).

   This function also accepts an omitted abstract declarator as being
   an abstract declarator, although not part of the formal syntax.  */

struct c_declarator *
c_parser_declarator (c_parser *parser, bool type_seen_p, c_dtr_syn kind,
		     bool *seen_id)
{
  /* Parse any initial pointer part.  */
  if (c_parser_next_token_is (parser, CPP_MULT))
    {
      struct c_declspecs *quals_attrs = build_null_declspecs ();
      struct c_declarator *inner;
      c_parser_consume_token (parser);
      c_parser_declspecs (parser, quals_attrs, false, false, true,
			  false, false, true, false, cla_prefer_id);
      inner = c_parser_declarator (parser, type_seen_p, kind, seen_id);
      if (inner == NULL)
	return NULL;
      else
	return make_pointer_declarator (quals_attrs, inner);
    }
  /* Now we have a direct declarator, direct abstract declarator or
     nothing (which counts as a direct abstract declarator here).  */
  return c_parser_direct_declarator (parser, type_seen_p, kind, seen_id);
}

/* Parse a direct declarator or direct abstract declarator; arguments
   as c_parser_declarator.  */

static struct c_declarator *
c_parser_direct_declarator (c_parser *parser, bool type_seen_p, c_dtr_syn kind,
			    bool *seen_id)
{
  /* The direct declarator must start with an identifier (possibly
     omitted) or a parenthesized declarator (possibly abstract).  In
     an ordinary declarator, initial parentheses must start a
     parenthesized declarator.  In an abstract declarator or parameter
     declarator, they could start a parenthesized declarator or a
     parameter list.  To tell which, the open parenthesis and any
     following gnu-attributes must be read.  If a declaration
     specifier or standard attributes follow, then it is a parameter
     list; if the specifier is a typedef name, there might be an
     ambiguity about redeclaring it, which is resolved in the
     direction of treating it as a typedef name.  If a close
     parenthesis follows, it is also an empty parameter list, as the
     syntax does not permit empty abstract declarators.  Otherwise, it
     is a parenthesized declarator (in which case the analysis may be
     repeated inside it, recursively).

     ??? There is an ambiguity in a parameter declaration "int
     (__attribute__((foo)) x)", where x is not a typedef name: it
     could be an abstract declarator for a function, or declare x with
     parentheses.  The proper resolution of this ambiguity needs
     documenting.  At present we follow an accident of the old
     parser's implementation, whereby the first parameter must have
     some declaration specifiers other than just gnu-attributes.  Thus as
     a parameter declaration it is treated as a parenthesized
     parameter named x, and as an abstract declarator it is
     rejected.

     ??? Also following the old parser, gnu-attributes inside an empty
     parameter list are ignored, making it a list not yielding a
     prototype, rather than giving an error or making it have one
     parameter with implicit type int.

     ??? Also following the old parser, typedef names may be
     redeclared in declarators, but not Objective-C class names.  */

  if (kind != C_DTR_ABSTRACT
      && c_parser_next_token_is (parser, CPP_NAME)
      && ((type_seen_p
	   && (c_parser_peek_token (parser)->id_kind == C_ID_TYPENAME
	       || c_parser_peek_token (parser)->id_kind == C_ID_CLASSNAME))
	  || c_parser_peek_token (parser)->id_kind == C_ID_ID))
    {
      struct c_declarator *inner
	= build_id_declarator (c_parser_peek_token (parser)->value);
      *seen_id = true;
      inner->id_loc = c_parser_peek_token (parser)->location;
      c_parser_consume_token (parser);
      if (c_parser_nth_token_starts_std_attributes (parser, 1))
	inner->u.id.attrs = c_parser_std_attribute_specifier_sequence (parser);
      return c_parser_direct_declarator_inner (parser, *seen_id, inner);
    }

  if (kind != C_DTR_NORMAL
      && c_parser_next_token_is (parser, CPP_OPEN_SQUARE)
      && !c_parser_nth_token_starts_std_attributes (parser, 1))
    {
      struct c_declarator *inner = build_id_declarator (NULL_TREE);
      inner->id_loc = c_parser_peek_token (parser)->location;
      return c_parser_direct_declarator_inner (parser, *seen_id, inner);
    }

  /* Either we are at the end of an abstract declarator, or we have
     parentheses.  */

  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      tree attrs;
      struct c_declarator *inner;
      c_parser_consume_token (parser);
      bool have_gnu_attrs = c_parser_next_token_is_keyword (parser,
							    RID_ATTRIBUTE);
      attrs = c_parser_gnu_attributes (parser);
      if (kind != C_DTR_NORMAL
	  && (c_parser_next_token_starts_declspecs (parser)
	      || (!have_gnu_attrs
		  && (c_parser_nth_token_starts_std_attributes (parser, 1)
		      || c_parser_next_token_is (parser, CPP_ELLIPSIS)))
	      || c_parser_next_token_is (parser, CPP_CLOSE_PAREN)))
	{
	  struct c_arg_info *args
	    = c_parser_parms_declarator (parser, kind == C_DTR_NORMAL,
					 attrs, have_gnu_attrs);
	  if (args == NULL)
	    return NULL;
	  else
	    {
	      inner = build_id_declarator (NULL_TREE);
	      if (!(args->types
		    && args->types != error_mark_node
		    && TREE_CODE (TREE_VALUE (args->types)) == IDENTIFIER_NODE)
		  && c_parser_nth_token_starts_std_attributes (parser, 1))
		{
		  tree std_attrs
		    = c_parser_std_attribute_specifier_sequence (parser);
		  if (std_attrs)
		    inner = build_attrs_declarator (std_attrs, inner);
		}
	      inner = build_function_declarator (args, inner);
	      return c_parser_direct_declarator_inner (parser, *seen_id,
						       inner);
	    }
	}
      /* A parenthesized declarator.  */
      inner = c_parser_declarator (parser, type_seen_p, kind, seen_id);
      if (inner != NULL && attrs != NULL)
	inner = build_attrs_declarator (attrs, inner);
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  c_parser_consume_token (parser);
	  if (inner == NULL)
	    return NULL;
	  else
	    return c_parser_direct_declarator_inner (parser, *seen_id, inner);
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return NULL;
	}
    }
  else
    {
      if (kind == C_DTR_NORMAL)
	{
	  c_parser_error (parser, "expected identifier or %<(%>");
	  return NULL;
	}
      else
	return build_id_declarator (NULL_TREE);
    }
}

/* Parse part of a direct declarator or direct abstract declarator,
   given that some (in INNER) has already been parsed; ID_PRESENT is
   true if an identifier is present, false for an abstract
   declarator.  */

static struct c_declarator *
c_parser_direct_declarator_inner (c_parser *parser, bool id_present,
				  struct c_declarator *inner)
{
  /* Parse a sequence of array declarators and parameter lists.  */
  if (c_parser_next_token_is (parser, CPP_OPEN_SQUARE)
      && !c_parser_nth_token_starts_std_attributes (parser, 1))
    {
      location_t brace_loc = c_parser_peek_token (parser)->location;
      struct c_declarator *declarator;
      struct c_declspecs *quals_attrs = build_null_declspecs ();
      bool static_seen;
      bool star_seen;
      struct c_expr dimen;
      dimen.value = NULL_TREE;
      dimen.original_code = ERROR_MARK;
      dimen.original_type = NULL_TREE;
      c_parser_consume_token (parser);
      c_parser_declspecs (parser, quals_attrs, false, false, true,
			  false, false, false, false, cla_prefer_id);
      static_seen = c_parser_next_token_is_keyword (parser, RID_STATIC);
      if (static_seen)
	c_parser_consume_token (parser);
      if (static_seen && !quals_attrs->declspecs_seen_p)
	c_parser_declspecs (parser, quals_attrs, false, false, true,
			    false, false, false, false, cla_prefer_id);
      if (!quals_attrs->declspecs_seen_p)
	quals_attrs = NULL;
      /* If "static" is present, there must be an array dimension.
	 Otherwise, there may be a dimension, "*", or no
	 dimension.  */
      if (static_seen)
	{
	  star_seen = false;
	  dimen = c_parser_expr_no_commas (parser, NULL);
	}
      else
	{
	  if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
	    {
	      dimen.value = NULL_TREE;
	      star_seen = false;
	    }
	  else if (c_parser_next_token_is (parser, CPP_MULT))
	    {
	      if (c_parser_peek_2nd_token (parser)->type == CPP_CLOSE_SQUARE)
		{
		  dimen.value = NULL_TREE;
		  star_seen = true;
		  c_parser_consume_token (parser);
		}
	      else
		{
		  star_seen = false;
		  dimen = c_parser_expr_no_commas (parser, NULL);
		}
	    }
	  else
	    {
	      star_seen = false;
	      dimen = c_parser_expr_no_commas (parser, NULL);
	    }
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
	c_parser_consume_token (parser);
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
				     "expected %<]%>");
	  return NULL;
	}
      if (dimen.value)
	dimen = convert_lvalue_to_rvalue (brace_loc, dimen, true, true);
      declarator = build_array_declarator (brace_loc, dimen.value, quals_attrs,
					   static_seen, star_seen);
      if (declarator == NULL)
	return NULL;
      if (c_parser_nth_token_starts_std_attributes (parser, 1))
	{
	  tree std_attrs
	    = c_parser_std_attribute_specifier_sequence (parser);
	  if (std_attrs)
	    inner = build_attrs_declarator (std_attrs, inner);
	}
      inner = set_array_declarator_inner (declarator, inner);
      return c_parser_direct_declarator_inner (parser, id_present, inner);
    }
  else if (c_parser_next_token_is (parser, CPP_OPEN_PAREN))
    {
      tree attrs;
      struct c_arg_info *args;
      c_parser_consume_token (parser);
      bool have_gnu_attrs = c_parser_next_token_is_keyword (parser,
							    RID_ATTRIBUTE);
      attrs = c_parser_gnu_attributes (parser);
      args = c_parser_parms_declarator (parser, id_present, attrs,
					have_gnu_attrs);
      if (args == NULL)
	return NULL;
      else
	{
	  if (!(args->types
		&& args->types != error_mark_node
		&& TREE_CODE (TREE_VALUE (args->types)) == IDENTIFIER_NODE)
	      && c_parser_nth_token_starts_std_attributes (parser, 1))
	    {
	      tree std_attrs
		= c_parser_std_attribute_specifier_sequence (parser);
	      if (std_attrs)
		inner = build_attrs_declarator (std_attrs, inner);
	    }
	  inner = build_function_declarator (args, inner);
	  return c_parser_direct_declarator_inner (parser, id_present, inner);
	}
    }
  return inner;
}

/* Parse a parameter list or identifier list, including the closing
   parenthesis but not the opening one.  ATTRS are the gnu-attributes
   at the start of the list.  ID_LIST_OK is true if an identifier list
   is acceptable; such a list must not have attributes at the start.
   HAVE_GNU_ATTRS says whether any gnu-attributes (including empty
   attributes) were present (in which case standard attributes cannot
   occur).  */

static struct c_arg_info *
c_parser_parms_declarator (c_parser *parser, bool id_list_ok, tree attrs,
			   bool have_gnu_attrs)
{
  push_scope ();
  declare_parm_level ();
  /* If the list starts with an identifier, it is an identifier list.
     Otherwise, it is either a prototype list or an empty list.  */
  if (id_list_ok
      && !attrs
      && c_parser_next_token_is (parser, CPP_NAME)
      && c_parser_peek_token (parser)->id_kind == C_ID_ID
      
      /* Look ahead to detect typos in type names.  */
      && c_parser_peek_2nd_token (parser)->type != CPP_NAME
      && c_parser_peek_2nd_token (parser)->type != CPP_MULT
      && c_parser_peek_2nd_token (parser)->type != CPP_OPEN_PAREN
      && c_parser_peek_2nd_token (parser)->type != CPP_OPEN_SQUARE
      && c_parser_peek_2nd_token (parser)->type != CPP_KEYWORD)
    {
      tree list = NULL_TREE, *nextp = &list;
      while (c_parser_next_token_is (parser, CPP_NAME)
	     && c_parser_peek_token (parser)->id_kind == C_ID_ID)
	{
	  *nextp = build_tree_list (NULL_TREE,
				    c_parser_peek_token (parser)->value);
	  nextp = & TREE_CHAIN (*nextp);
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is_not (parser, CPP_COMMA))
	    break;
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    {
	      c_parser_error (parser, "expected identifier");
	      break;
	    }
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  struct c_arg_info *ret = build_arg_info ();
	  ret->types = list;
	  c_parser_consume_token (parser);
	  pop_scope ();
	  return ret;
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  pop_scope ();
	  return NULL;
	}
    }
  else
    {
      struct c_arg_info *ret
	= c_parser_parms_list_declarator (parser, attrs, NULL, have_gnu_attrs);
      pop_scope ();
      return ret;
    }
}

/* Parse a parameter list (possibly empty), including the closing
   parenthesis but not the opening one.  ATTRS are the gnu-attributes
   at the start of the list; if HAVE_GNU_ATTRS, there were some such
   attributes (possibly empty, in which case ATTRS is NULL_TREE),
   which means standard attributes cannot start the list.  EXPR is
   NULL or an expression that needs to be evaluated for the side
   effects of array size expressions in the parameters.  */

static struct c_arg_info *
c_parser_parms_list_declarator (c_parser *parser, tree attrs, tree expr,
				bool have_gnu_attrs)
{
  bool bad_parm = false;

  /* ??? Following the old parser, forward parameter declarations may
     use abstract declarators, and if no real parameter declarations
     follow the forward declarations then this is not diagnosed.  Also
     note as above that gnu-attributes are ignored as the only contents of
     the parentheses, or as the only contents after forward
     declarations.  */
  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
    {
      struct c_arg_info *ret = build_arg_info ();
      c_parser_consume_token (parser);
      return ret;
    }
  if (c_parser_next_token_is (parser, CPP_ELLIPSIS) && !have_gnu_attrs)
    {
      struct c_arg_info *ret = build_arg_info ();

      ret->types = NULL_TREE;
      pedwarn_c11 (c_parser_peek_token (parser)->location, OPT_Wpedantic,
		   "ISO C requires a named argument before %<...%> "
		   "before C2X");
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  ret->no_named_args_stdarg_p = true;
	  c_parser_consume_token (parser);
	  return ret;
	}
      else
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return NULL;
	}
    }
  /* Nonempty list of parameters, either terminated with semicolon
     (forward declarations; recurse) or with close parenthesis (normal
     function) or with ", ... )" (variadic function).  */
  while (true)
    {
      /* Parse a parameter.  */
      struct c_parm *parm = c_parser_parameter_declaration (parser, attrs,
							    have_gnu_attrs);
      attrs = NULL_TREE;
      have_gnu_attrs = false;
      if (parm == NULL)
	bad_parm = true;
      else
	push_parm_decl (parm, &expr);
      if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	{
	  tree new_attrs;
	  c_parser_consume_token (parser);
	  mark_forward_parm_decls ();
	  bool new_have_gnu_attrs
	    = c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE);
	  new_attrs = c_parser_gnu_attributes (parser);
	  return c_parser_parms_list_declarator (parser, new_attrs, expr,
						 new_have_gnu_attrs);
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  c_parser_consume_token (parser);
	  if (bad_parm)
	    return NULL;
	  else
	    return get_parm_info (false, expr);
	}
      if (!c_parser_require (parser, CPP_COMMA,
			     "expected %<;%>, %<,%> or %<)%>",
			     UNKNOWN_LOCATION, false))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return NULL;
	}
      if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
	{
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	    {
	      c_parser_consume_token (parser);
	      if (bad_parm)
		return NULL;
	      else
		return get_parm_info (true, expr);
	    }
	  else
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
					 "expected %<)%>");
	      return NULL;
	    }
	}
    }
}

/* Parse a parameter declaration.  ATTRS are the gnu-attributes at the
   start of the declaration if it is the first parameter;
   HAVE_GNU_ATTRS is true if there were any gnu-attributes there (even
   empty) there.  */

static struct c_parm *
c_parser_parameter_declaration (c_parser *parser, tree attrs,
				bool have_gnu_attrs)
{
  struct c_declspecs *specs;
  struct c_declarator *declarator;
  tree prefix_attrs;
  tree postfix_attrs = NULL_TREE;
  bool dummy = false;

  /* Accept #pragmas between parameter declarations.  */
  while (c_parser_next_token_is (parser, CPP_PRAGMA))
    c_parser_pragma (parser, pragma_param, NULL);

  if (!c_parser_next_token_starts_declspecs (parser)
      && !c_parser_nth_token_starts_std_attributes (parser, 1))
    {
      c_token *token = c_parser_peek_token (parser);
      if (parser->error)
	return NULL;
      c_parser_set_source_position_from_token (token);
      if (c_parser_next_tokens_start_typename (parser, cla_prefer_type))
	{
	  auto_diagnostic_group d;
	  name_hint hint = lookup_name_fuzzy (token->value,
					      FUZZY_LOOKUP_TYPENAME,
					      token->location);
	  if (const char *suggestion = hint.suggestion ())
	    {
	      gcc_rich_location richloc (token->location);
	      richloc.add_fixit_replace (suggestion);
	      error_at (&richloc,
			"unknown type name %qE; did you mean %qs?",
			token->value, suggestion);
	    }
	  else
	    error_at (token->location, "unknown type name %qE", token->value);
	  parser->error = true;
	}
      /* ??? In some Objective-C cases '...' isn't applicable so there
	 should be a different message.  */
      else
	c_parser_error (parser,
			"expected declaration specifiers or %<...%>");
      c_parser_skip_to_end_of_parameter (parser);
      return NULL;
    }

  location_t start_loc = c_parser_peek_token (parser)->location;

  specs = build_null_declspecs ();
  if (attrs)
    {
      declspecs_add_attrs (input_location, specs, attrs);
      attrs = NULL_TREE;
    }
  c_parser_declspecs (parser, specs, true, true, true, true, false,
		      !have_gnu_attrs, true, cla_nonabstract_decl);
  finish_declspecs (specs);
  pending_xref_error ();
  prefix_attrs = specs->attrs;
  specs->attrs = NULL_TREE;
  declarator = c_parser_declarator (parser,
				    specs->typespec_kind != ctsk_none,
				    C_DTR_PARM, &dummy);
  if (declarator == NULL)
    {
      c_parser_skip_until_found (parser, CPP_COMMA, NULL);
      return NULL;
    }
  if (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
    postfix_attrs = c_parser_gnu_attributes (parser);

  /* Generate a location for the parameter, ranging from the start of the
     initial token to the end of the final token.

     If we have a identifier, then use it for the caret location, e.g.

       extern int callee (int one, int (*two)(int, int), float three);
                                   ~~~~~~^~~~~~~~~~~~~~

     otherwise, reuse the start location for the caret location e.g.:

       extern int callee (int one, int (*)(int, int), float three);
                                   ^~~~~~~~~~~~~~~~~
  */
  location_t end_loc = parser->last_token_location;

  /* Find any cdk_id declarator; determine if we have an identifier.  */
  c_declarator *id_declarator = declarator;
  while (id_declarator && id_declarator->kind != cdk_id)
    id_declarator = id_declarator->declarator;
  location_t caret_loc = (id_declarator->u.id.id
			  ? id_declarator->id_loc
			  : start_loc);
  location_t param_loc = make_location (caret_loc, start_loc, end_loc);

  return build_c_parm (specs, chainon (postfix_attrs, prefix_attrs),
		       declarator, param_loc);
}

/* Parse a string literal in an asm expression.  It should not be
   translated, and wide string literals are an error although
   permitted by the syntax.  This is a GNU extension.

   asm-string-literal:
     string-literal
*/

static tree
c_parser_asm_string_literal (c_parser *parser)
{
  tree str;
  int save_flag = warn_overlength_strings;
  warn_overlength_strings = 0;
  str = c_parser_string_literal (parser, false, false).value;
  warn_overlength_strings = save_flag;
  return str;
}

/* Parse a simple asm expression.  This is used in restricted
   contexts, where a full expression with inputs and outputs does not
   make sense.  This is a GNU extension.

   simple-asm-expr:
     asm ( asm-string-literal )
*/

static tree
c_parser_simple_asm_expr (c_parser *parser)
{
  tree str;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ASM));
  c_parser_consume_token (parser);
  matching_parens parens;
  if (!parens.require_open (parser))
    return NULL_TREE;
  str = c_parser_asm_string_literal (parser);
  if (!parens.require_close (parser))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return NULL_TREE;
    }
  return str;
}

static tree
c_parser_gnu_attribute_any_word (c_parser *parser)
{
  tree attr_name = NULL_TREE;

  if (c_parser_next_token_is (parser, CPP_KEYWORD))
    {
      /* ??? See comment above about what keywords are accepted here.  */
      bool ok;
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_STATIC:
	case RID_UNSIGNED:
	case RID_LONG:
	case RID_CONST:
	case RID_EXTERN:
	case RID_REGISTER:
	case RID_TYPEDEF:
	case RID_SHORT:
	case RID_INLINE:
	case RID_NORETURN:
	case RID_VOLATILE:
	case RID_SIGNED:
	case RID_AUTO:
	case RID_RESTRICT:
	case RID_COMPLEX:
	case RID_THREAD:
	case RID_INT:
	case RID_CHAR:
	case RID_FLOAT:
	case RID_DOUBLE:
	case RID_VOID:
	case RID_DFLOAT32:
	case RID_DFLOAT64:
	case RID_DFLOAT128:
	CASE_RID_FLOATN_NX:
	case RID_BOOL:
	case RID_FRACT:
	case RID_ACCUM:
	case RID_SAT:
	case RID_ATOMIC:
	case RID_AUTO_TYPE:
	case RID_CONSTEXPR:
	case RID_INT_N_0:
	case RID_INT_N_1:
	case RID_INT_N_2:
	case RID_INT_N_3:
	  ok = true;
	  break;
	default:
	  ok = false;
	  break;
	}
      if (!ok)
	return NULL_TREE;

      /* Accept __attribute__((__const)) as __attribute__((const)) etc.  */
      attr_name = ridpointers[(int) c_parser_peek_token (parser)->keyword];
    }
  else if (c_parser_next_token_is (parser, CPP_NAME))
    attr_name = c_parser_peek_token (parser)->value;

  return attr_name;
}

/* Parse attribute arguments.  This is a common form of syntax
   covering all currently valid GNU and standard attributes.

   gnu-attribute-arguments:
     identifier
     identifier , nonempty-expr-list
     expr-list

   where the "identifier" must not be declared as a type.  ??? Why not
   allow identifiers declared as types to start the arguments?  */

static tree
c_parser_attribute_arguments (c_parser *parser, bool takes_identifier,
			      bool require_string, bool assume_attr,
			      bool allow_empty_args)
{
  vec<tree, va_gc> *expr_list;
  tree attr_args;
  /* Parse the attribute contents.  If they start with an
     identifier which is followed by a comma or close
     parenthesis, then the arguments start with that
     identifier; otherwise they are an expression list.
     In objective-c the identifier may be a classname.  */
  if (c_parser_next_token_is (parser, CPP_NAME)
      && (c_parser_peek_token (parser)->id_kind == C_ID_ID
	  || (c_dialect_objc ()
	      && c_parser_peek_token (parser)->id_kind
	      == C_ID_CLASSNAME))
      && ((c_parser_peek_2nd_token (parser)->type == CPP_COMMA)
	  || (c_parser_peek_2nd_token (parser)->type
	      == CPP_CLOSE_PAREN))
      && (takes_identifier
	  || (c_dialect_objc ()
	      && !assume_attr
	      && c_parser_peek_token (parser)->id_kind
	      == C_ID_CLASSNAME)))
    {
      tree arg1 = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	attr_args = build_tree_list (NULL_TREE, arg1);
      else
	{
	  tree tree_list;
	  c_parser_consume_token (parser);
	  expr_list = c_parser_expr_list (parser, false, true,
					  NULL, NULL, NULL, NULL);
	  tree_list = build_tree_list_vec (expr_list);
	  attr_args = tree_cons (NULL_TREE, arg1, tree_list);
	  release_tree_vector (expr_list);
	}
    }
  else
    {
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  if (!allow_empty_args)
	    error_at (c_parser_peek_token (parser)->location,
		      "parentheses must be omitted if "
		      "attribute argument list is empty");
	  attr_args = NULL_TREE;
	}
      else if (require_string)
	{
	  /* The only valid argument for this attribute is a string
	     literal.  Handle this specially here to avoid accepting
	     string literals with excess parentheses.  */
	  tree string = c_parser_string_literal (parser, false, true).value;
	  attr_args = build_tree_list (NULL_TREE, string);
	}
      else if (assume_attr)
	{
	  tree cond
	    = c_parser_conditional_expression (parser, NULL, NULL_TREE).value;
	  if (!c_parser_next_token_is (parser, CPP_COMMA))
	    attr_args = build_tree_list (NULL_TREE, cond);
	  else
	    {
	      tree tree_list;
	      c_parser_consume_token (parser);
	      expr_list = c_parser_expr_list (parser, false, true,
					      NULL, NULL, NULL, NULL);
	      tree_list = build_tree_list_vec (expr_list);
	      attr_args = tree_cons (NULL_TREE, cond, tree_list);
	      release_tree_vector (expr_list);
	    }
	}
      else
	{
	  expr_list = c_parser_expr_list (parser, false, true,
					  NULL, NULL, NULL, NULL);
	  attr_args = build_tree_list_vec (expr_list);
	  release_tree_vector (expr_list);
	}
    }
  return attr_args;
}

/* Parse (possibly empty) gnu-attributes.  This is a GNU extension.

   gnu-attributes:
     empty
     gnu-attributes gnu-attribute

   gnu-attribute:
     __attribute__ ( ( gnu-attribute-list ) )

   gnu-attribute-list:
     gnu-attrib
     gnu-attribute_list , gnu-attrib

   gnu-attrib:
     empty
     any-word
     any-word ( gnu-attribute-arguments )

   where "any-word" may be any identifier (including one declared as a
   type), a reserved word storage class specifier, type specifier or
   type qualifier.  ??? This still leaves out most reserved keywords
   (following the old parser), shouldn't we include them?
   When EXPECT_COMMA is true, expect the attribute to be preceded
   by a comma and fail if it isn't.
   When EMPTY_OK is true, allow and consume any number of consecutive
   commas with no attributes in between.  */

static tree
c_parser_gnu_attribute (c_parser *parser, tree attrs,
			bool expect_comma = false, bool empty_ok = true)
{
  bool comma_first = c_parser_next_token_is (parser, CPP_COMMA);
  if (!comma_first
      && !c_parser_next_token_is (parser, CPP_NAME)
      && !c_parser_next_token_is (parser, CPP_KEYWORD))
    return NULL_TREE;

  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);
      if (!empty_ok)
	return attrs;
    }

  tree attr_name = c_parser_gnu_attribute_any_word (parser);
  if (attr_name == NULL_TREE)
    return NULL_TREE;

  attr_name = canonicalize_attr_name (attr_name);
  c_parser_consume_token (parser);

  tree attr;
  if (c_parser_next_token_is_not (parser, CPP_OPEN_PAREN))
    {
      if (expect_comma && !comma_first)
	{
	  /* A comma is missing between the last attribute on the chain
	     and this one.  */
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return error_mark_node;
	}
      attr = build_tree_list (attr_name, NULL_TREE);
      /* Add this attribute to the list.  */
      attrs = chainon (attrs, attr);
      return attrs;
    }
  c_parser_consume_token (parser);

  tree attr_args
    = c_parser_attribute_arguments (parser,
				    attribute_takes_identifier_p (attr_name),
				    false,
				    is_attribute_p ("assume", attr_name),
				    true);

  attr = build_tree_list (attr_name, attr_args);
  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
    c_parser_consume_token (parser);
  else
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				 "expected %<)%>");
      return error_mark_node;
    }

  if (expect_comma && !comma_first)
    {
      /* A comma is missing between the last attribute on the chain
	 and this one.  */
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				 "expected %<)%>");
      return error_mark_node;
    }

  /* Add this attribute to the list.  */
  attrs = chainon (attrs, attr);
  return attrs;
}

static tree
c_parser_gnu_attributes (c_parser *parser)
{
  tree attrs = NULL_TREE;
  while (c_parser_next_token_is_keyword (parser, RID_ATTRIBUTE))
    {
      bool save_translate_strings_p = parser->translate_strings_p;
      parser->translate_strings_p = false;
      /* Consume the `__attribute__' keyword.  */
      c_parser_consume_token (parser);
      /* Look for the two `(' tokens.  */
      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	{
	  parser->translate_strings_p = save_translate_strings_p;
	  return attrs;
	}
      if (!c_parser_require (parser, CPP_OPEN_PAREN, "expected %<(%>"))
	{
	  parser->translate_strings_p = save_translate_strings_p;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return attrs;
	}
      /* Parse the attribute list.  Require a comma between successive
	 (possibly empty) attributes.  */
      for (bool expect_comma = false; ; expect_comma = true)
	{
	  /* Parse a single attribute.  */
	  tree attr = c_parser_gnu_attribute (parser, attrs, expect_comma);
	  if (attr == error_mark_node)
	    return attrs;
	  if (!attr)
	    break;
	  attrs = attr;
      }

      /* Look for the two `)' tokens.  */
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	c_parser_consume_token (parser);
      else
	{
	  parser->translate_strings_p = save_translate_strings_p;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return attrs;
	}
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	c_parser_consume_token (parser);
      else
	{
	  parser->translate_strings_p = save_translate_strings_p;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  return attrs;
	}
      parser->translate_strings_p = save_translate_strings_p;
    }

  return attrs;
}

/* Parse an optional balanced token sequence.

   balanced-token-sequence:
     balanced-token
     balanced-token-sequence balanced-token

   balanced-token:
     ( balanced-token-sequence[opt] )
     [ balanced-token-sequence[opt] ]
     { balanced-token-sequence[opt] }
     any token other than ()[]{}
*/

static void
c_parser_balanced_token_sequence (c_parser *parser)
{
  while (true)
    {
      c_token *token = c_parser_peek_token (parser);
      switch (token->type)
	{
	case CPP_OPEN_BRACE:
	  {
	    matching_braces braces;
	    braces.consume_open (parser);
	    c_parser_balanced_token_sequence (parser);
	    braces.require_close (parser);
	    break;
	  }

	case CPP_OPEN_PAREN:
	  {
	    matching_parens parens;
	    parens.consume_open (parser);
	    c_parser_balanced_token_sequence (parser);
	    parens.require_close (parser);
	    break;
	  }

	case CPP_OPEN_SQUARE:
	  c_parser_consume_token (parser);
	  c_parser_balanced_token_sequence (parser);
	  c_parser_require (parser, CPP_CLOSE_SQUARE, "expected %<]%>");
	  break;

	case CPP_CLOSE_BRACE:
	case CPP_CLOSE_PAREN:
	case CPP_CLOSE_SQUARE:
	case CPP_EOF:
	  return;

	case CPP_PRAGMA:
	  c_parser_consume_pragma (parser);
	  c_parser_skip_to_pragma_eol (parser, false);
	  break;

	default:
	  c_parser_consume_token (parser);
	  break;
	}
    }
}

/* Parse standard (C2X) attributes (including GNU attributes in the
   gnu:: namespace).

   attribute-specifier-sequence:
     attribute-specifier-sequence[opt] attribute-specifier

   attribute-specifier:
     [ [ attribute-list ] ]

   attribute-list:
     attribute[opt]
     attribute-list, attribute[opt]

   attribute:
     attribute-token attribute-argument-clause[opt]

   attribute-token:
     standard-attribute
     attribute-prefixed-token

   standard-attribute:
     identifier

   attribute-prefixed-token:
     attribute-prefix :: identifier

   attribute-prefix:
     identifier

   attribute-argument-clause:
     ( balanced-token-sequence[opt] )

   Keywords are accepted as identifiers for this purpose.
*/

static tree
c_parser_std_attribute (c_parser *parser, bool for_tm)
{
  c_token *token = c_parser_peek_token (parser);
  tree ns, name, attribute;

  /* Parse the attribute-token.  */
  if (token->type != CPP_NAME && token->type != CPP_KEYWORD)
    {
      c_parser_error (parser, "expected identifier");
      return error_mark_node;
    }
  name = canonicalize_attr_name (token->value);
  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_SCOPE))
    {
      ns = name;
      c_parser_consume_token (parser);
      token = c_parser_peek_token (parser);
      if (token->type != CPP_NAME && token->type != CPP_KEYWORD)
	{
	  c_parser_error (parser, "expected identifier");
	  return error_mark_node;
	}
      name = canonicalize_attr_name (token->value);
      c_parser_consume_token (parser);
    }
  else
    ns = NULL_TREE;
  attribute = build_tree_list (build_tree_list (ns, name), NULL_TREE);

  /* Parse the arguments, if any.  */
  const attribute_spec *as = lookup_attribute_spec (TREE_PURPOSE (attribute));
  if (c_parser_next_token_is_not (parser, CPP_OPEN_PAREN))
    goto out;
  {
    location_t open_loc = c_parser_peek_token (parser)->location;
    matching_parens parens;
    parens.consume_open (parser);
    if ((as && as->max_length == 0)
	/* Special-case the transactional-memory attribute "outer",
	   which is specially handled but not registered as an
	   attribute, to avoid allowing arbitrary balanced token
	   sequences as arguments.  */
	|| is_attribute_p ("outer", name))
      {
	error_at (open_loc, "%qE attribute does not take any arguments", name);
	parens.skip_until_found_close (parser);
	return error_mark_node;
      }
    /* If this is a fake attribute created to handle -Wno-attributes,
       we must skip parsing the arguments.  */
    if (as && !attribute_ignored_p (as))
      {
	bool takes_identifier
	  = (ns != NULL_TREE
	     && strcmp (IDENTIFIER_POINTER (ns), "gnu") == 0
	     && attribute_takes_identifier_p (name));
	bool require_string
	  = (ns == NULL_TREE
	     && (strcmp (IDENTIFIER_POINTER (name), "deprecated") == 0
		 || strcmp (IDENTIFIER_POINTER (name), "nodiscard") == 0));
	bool assume_attr
	  = (ns != NULL_TREE
	     && strcmp (IDENTIFIER_POINTER (ns), "gnu") == 0
	     && strcmp (IDENTIFIER_POINTER (name), "assume") == 0);
	TREE_VALUE (attribute)
	  = c_parser_attribute_arguments (parser, takes_identifier,
					  require_string, assume_attr, false);
      }
    else
      c_parser_balanced_token_sequence (parser);
    parens.require_close (parser);
  }
 out:
  if (ns == NULL_TREE && !for_tm && !as)
    {
      /* An attribute with standard syntax and no namespace specified
	 is a constraint violation if it is not one of the known
	 standard attributes.  Diagnose it here with a pedwarn and
	 then discard it to prevent a duplicate warning later.  */
      pedwarn (input_location, OPT_Wattributes, "%qE attribute ignored",
	       name);
      return error_mark_node;
    }
  return attribute;
}

static tree
c_parser_std_attribute_specifier (c_parser *parser, bool for_tm)
{
  location_t loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_OPEN_SQUARE, "expected %<[%>"))
    return NULL_TREE;
  if (!c_parser_require (parser, CPP_OPEN_SQUARE, "expected %<[%>"))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, "expected %<]%>");
      return NULL_TREE;
    }
  if (!for_tm)
    pedwarn_c11 (loc, OPT_Wpedantic,
		 "ISO C does not support %<[[]]%> attributes before C2X");
  tree attributes = NULL_TREE;
  while (true)
    {
      c_token *token = c_parser_peek_token (parser);
      if (token->type == CPP_CLOSE_SQUARE)
	break;
      if (token->type == CPP_COMMA)
	{
	  c_parser_consume_token (parser);
	  continue;
	}
      tree attribute = c_parser_std_attribute (parser, for_tm);
      if (attribute != error_mark_node)
	{
	  TREE_CHAIN (attribute) = attributes;
	  attributes = attribute;
	}
      if (c_parser_next_token_is_not (parser, CPP_COMMA))
	break;
    }
  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, "expected %<]%>");
  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, "expected %<]%>");
  return nreverse (attributes);
}

/* Look past an optional balanced token sequence of raw look-ahead
   tokens starting with the *Nth token.  *N is updated to point to the
   following token.  Return true if such a sequence was found, false
   if the tokens parsed were not balanced.  */

static bool
c_parser_check_balanced_raw_token_sequence (c_parser *parser, unsigned int *n)
{
  while (true)
    {
      c_token *token = c_parser_peek_nth_token_raw (parser, *n);
      switch (token->type)
	{
	case CPP_OPEN_BRACE:
	  {
	    ++*n;
	    if (c_parser_check_balanced_raw_token_sequence (parser, n))
	      {
		token = c_parser_peek_nth_token_raw (parser, *n);
		if (token->type == CPP_CLOSE_BRACE)
		  ++*n;
		else
		  return false;
	      }
	    else
	      return false;
	    break;
	  }

	case CPP_OPEN_PAREN:
	  {
	    ++*n;
	    if (c_parser_check_balanced_raw_token_sequence (parser, n))
	      {
		token = c_parser_peek_nth_token_raw (parser, *n);
		if (token->type == CPP_CLOSE_PAREN)
		  ++*n;
		else
		  return false;
	      }
	    else
	      return false;
	    break;
	  }

	case CPP_OPEN_SQUARE:
	  {
	    ++*n;
	    if (c_parser_check_balanced_raw_token_sequence (parser, n))
	      {
		token = c_parser_peek_nth_token_raw (parser, *n);
		if (token->type == CPP_CLOSE_SQUARE)
		  ++*n;
		else
		  return false;
	      }
	    else
	      return false;
	    break;
	  }

	case CPP_CLOSE_BRACE:
	case CPP_CLOSE_PAREN:
	case CPP_CLOSE_SQUARE:
	case CPP_EOF:
	  return true;

	default:
	  ++*n;
	  break;
	}
    }
}

/* Return whether standard attributes start with the Nth token.  */

static bool
c_parser_nth_token_starts_std_attributes (c_parser *parser, unsigned int n)
{
  if (!(c_parser_peek_nth_token (parser, n)->type == CPP_OPEN_SQUARE
	&& c_parser_peek_nth_token (parser, n + 1)->type == CPP_OPEN_SQUARE))
    return false;
  /* In C, '[[' must start attributes.  In Objective-C, we need to
     check whether '[[' is matched by ']]'.  */
  if (!c_dialect_objc ())
    return true;
  n += 2;
  if (!c_parser_check_balanced_raw_token_sequence (parser, &n))
    return false;
  c_token *token = c_parser_peek_nth_token_raw (parser, n);
  if (token->type != CPP_CLOSE_SQUARE)
    return false;
  token = c_parser_peek_nth_token_raw (parser, n + 1);
  return token->type == CPP_CLOSE_SQUARE;
}

static tree
c_parser_std_attribute_specifier_sequence (c_parser *parser)
{
  tree attributes = NULL_TREE;
  do
    {
      tree attrs = c_parser_std_attribute_specifier (parser, false);
      attributes = chainon (attributes, attrs);
    }
  while (c_parser_nth_token_starts_std_attributes (parser, 1));
  return attributes;
}

/* Parse a type name (C90 6.5.5, C99 6.7.6, C11 6.7.7).  ALIGNAS_OK
   says whether alignment specifiers are OK (only in cases that might
   be the type name of a compound literal).

   type-name:
     specifier-qualifier-list abstract-declarator[opt]
*/

struct c_type_name *
c_parser_type_name (c_parser *parser, bool alignas_ok)
{
  struct c_declspecs *specs = build_null_declspecs ();
  struct c_declarator *declarator;
  struct c_type_name *ret;
  bool dummy = false;
  c_parser_declspecs (parser, specs, false, true, true, alignas_ok, false,
		      false, true, cla_prefer_type);
  if (!specs->declspecs_seen_p)
    {
      c_parser_error (parser, "expected specifier-qualifier-list");
      return NULL;
    }
  if (specs->type != error_mark_node)
    {
      pending_xref_error ();
      finish_declspecs (specs);
    }
  declarator = c_parser_declarator (parser,
				    specs->typespec_kind != ctsk_none,
				    C_DTR_ABSTRACT, &dummy);
  if (declarator == NULL)
    return NULL;
  ret = XOBNEW (&parser_obstack, struct c_type_name);
  ret->specs = specs;
  ret->declarator = declarator;
  return ret;
}

/* Parse an initializer (C90 6.5.7, C99 6.7.8, C11 6.7.9).

   initializer:
     assignment-expression
     { initializer-list }
     { initializer-list , }

   initializer-list:
     designation[opt] initializer
     initializer-list , designation[opt] initializer

   designation:
     designator-list =

   designator-list:
     designator
     designator-list designator

   designator:
     array-designator
     . identifier

   array-designator:
     [ constant-expression ]

   GNU extensions:

   initializer:
     { }

   designation:
     array-designator
     identifier :

   array-designator:
     [ constant-expression ... constant-expression ]

   Any expression without commas is accepted in the syntax for the
   constant-expressions, with non-constant expressions rejected later.

   DECL is the declaration we're parsing this initializer for.

   This function is only used for top-level initializers; for nested
   ones, see c_parser_initval.  */

static struct c_expr
c_parser_initializer (c_parser *parser, tree decl)
{
  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    return c_parser_braced_init (parser, NULL_TREE, false, NULL, decl);
  else
    {
      struct c_expr ret;
      location_t loc = c_parser_peek_token (parser)->location;
      ret = c_parser_expr_no_commas (parser, NULL);
      if (decl != error_mark_node && C_DECL_VARIABLE_SIZE (decl))
	{
	  error_at (loc,
		    "variable-sized object may not be initialized except "
		    "with an empty initializer");
	  ret.set_error ();
	}
      /* This is handled mostly by gimplify.cc, but we have to deal with
	 not warning about int x = x; as it is a GCC extension to turn off
	 this warning but only if warn_init_self is zero.  */
      if (VAR_P (decl)
	  && !DECL_EXTERNAL (decl)
	  && !TREE_STATIC (decl)
	  && ret.value == decl
	  && !warning_enabled_at (DECL_SOURCE_LOCATION (decl), OPT_Winit_self))
	suppress_warning (decl, OPT_Winit_self);
      if (TREE_CODE (ret.value) != STRING_CST
	  && (TREE_CODE (ret.value) != COMPOUND_LITERAL_EXPR
	      || C_DECL_DECLARED_CONSTEXPR (COMPOUND_LITERAL_EXPR_DECL
					    (ret.value))))
	ret = convert_lvalue_to_rvalue (loc, ret, true, true, true);
      return ret;
    }
}

/* The location of the last comma within the current initializer list,
   or UNKNOWN_LOCATION if not within one.  */

location_t last_init_list_comma;

/* Parse a braced initializer list.  TYPE is the type specified for a
   compound literal, and NULL_TREE for other initializers and for
   nested braced lists.  NESTED_P is true for nested braced lists,
   false for the list of a compound literal or the list that is the
   top-level initializer in a declaration.  DECL is the declaration for
   the top-level initializer for a declaration, otherwise NULL_TREE.  */

static struct c_expr
c_parser_braced_init (c_parser *parser, tree type, bool nested_p,
		      struct obstack *outer_obstack, tree decl)
{
  struct c_expr ret;
  struct obstack braced_init_obstack;
  location_t brace_loc = c_parser_peek_token (parser)->location;
  gcc_obstack_init (&braced_init_obstack);
  gcc_assert (c_parser_next_token_is (parser, CPP_OPEN_BRACE));
  matching_braces braces;
  braces.consume_open (parser);
  if (nested_p)
    {
      finish_implicit_inits (brace_loc, outer_obstack);
      push_init_level (brace_loc, 0, &braced_init_obstack);
    }
  else
    really_start_incremental_init (type);
  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      pedwarn_c11 (brace_loc, OPT_Wpedantic,
		   "ISO C forbids empty initializer braces before C2X");
    }
  else
    {
      if (decl && decl != error_mark_node && C_DECL_VARIABLE_SIZE (decl))
	error_at (brace_loc,
		  "variable-sized object may not be initialized except "
		  "with an empty initializer");
      /* Parse a non-empty initializer list, possibly with a trailing
	 comma.  */
      while (true)
	{
	  c_parser_initelt (parser, &braced_init_obstack);
	  if (parser->error)
	    break;
	  if (c_parser_next_token_is (parser, CPP_COMMA))
	    {
	      last_init_list_comma = c_parser_peek_token (parser)->location;
	      c_parser_consume_token (parser);
	    }
	  else
	    break;
	  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
	    break;
	}
    }
  c_token *next_tok = c_parser_peek_token (parser);
  if (next_tok->type != CPP_CLOSE_BRACE)
    {
      ret.set_error ();
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      braces.skip_until_found_close (parser);
      pop_init_level (brace_loc, 0, &braced_init_obstack, last_init_list_comma);
      obstack_free (&braced_init_obstack, NULL);
      return ret;
    }
  location_t close_loc = next_tok->location;
  c_parser_consume_token (parser);
  ret = pop_init_level (brace_loc, 0, &braced_init_obstack, close_loc);
  obstack_free (&braced_init_obstack, NULL);
  set_c_expr_source_range (&ret, brace_loc, close_loc);
  return ret;
}

/* Parse a nested initializer, including designators.  */

static void
c_parser_initelt (c_parser *parser, struct obstack * braced_init_obstack)
{
  /* Parse any designator or designator list.  A single array
     designator may have the subsequent "=" omitted in GNU C, but a
     longer list or a structure member designator may not.  */
  if (c_parser_next_token_is (parser, CPP_NAME)
      && c_parser_peek_2nd_token (parser)->type == CPP_COLON)
    {
      /* Old-style structure member designator.  */
      set_init_label (c_parser_peek_token (parser)->location,
		      c_parser_peek_token (parser)->value,
		      c_parser_peek_token (parser)->location,
		      braced_init_obstack);
      /* Use the colon as the error location.  */
      pedwarn (c_parser_peek_2nd_token (parser)->location, OPT_Wpedantic,
	       "obsolete use of designated initializer with %<:%>");
      c_parser_consume_token (parser);
      c_parser_consume_token (parser);
    }
  else
    {
      /* des_seen is 0 if there have been no designators, 1 if there
	 has been a single array designator and 2 otherwise.  */
      int des_seen = 0;
      /* Location of a designator.  */
      location_t des_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
      while (c_parser_next_token_is (parser, CPP_OPEN_SQUARE)
	     || c_parser_next_token_is (parser, CPP_DOT))
	{
	  int des_prev = des_seen;
	  if (!des_seen)
	    des_loc = c_parser_peek_token (parser)->location;
	  if (des_seen < 2)
	    des_seen++;
	  if (c_parser_next_token_is (parser, CPP_DOT))
	    {
	      des_seen = 2;
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is (parser, CPP_NAME))
		{
		  set_init_label (des_loc, c_parser_peek_token (parser)->value,
				  c_parser_peek_token (parser)->location,
				  braced_init_obstack);
		  c_parser_consume_token (parser);
		}
	      else
		{
		  struct c_expr init;
		  init.set_error ();
		  init.original_code = ERROR_MARK;
		  init.original_type = NULL;
		  c_parser_error (parser, "expected identifier");
		  c_parser_skip_until_found (parser, CPP_COMMA, NULL);
		  process_init_element (input_location, init, false,
					braced_init_obstack);
		  return;
		}
	    }
	  else
	    {
	      struct c_expr first_expr;
	      tree first, second;
	      location_t ellipsis_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
	      location_t array_index_loc = UNKNOWN_LOCATION;
	      /* ??? Following the old parser, [ objc-receiver
		 objc-message-args ] is accepted as an initializer,
		 being distinguished from a designator by what follows
		 the first assignment expression inside the square
		 brackets, but after a first array designator a
		 subsequent square bracket is for Objective-C taken to
		 start an expression, using the obsolete form of
		 designated initializer without '=', rather than
		 possibly being a second level of designation: in LALR
		 terms, the '[' is shifted rather than reducing
		 designator to designator-list.  */
	      if (des_prev == 1 && c_dialect_objc ())
		{
		  des_seen = des_prev;
		  break;
		}

// Modified for Scpel
// Possible BUG

	      c_parser_consume_token (parser);
	      array_index_loc = c_parser_peek_token (parser)->location;
	      first_expr = c_parser_expr_no_commas (parser, NULL);
	      mark_exp_read (first_expr.value);
	    array_desig_after_first:
	      first_expr = convert_lvalue_to_rvalue (array_index_loc,
						     first_expr,
						     true, true);
	      first = first_expr.value;
	      if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
		{
		  ellipsis_loc = c_parser_peek_token (parser)->location;
		  c_parser_consume_token (parser);
		  second = convert_lvalue_to_rvalue (ellipsis_loc,
						     (c_parser_expr_no_commas
						      (parser, NULL)),
						     true, true).value;
		  mark_exp_read (second);
		}
	      else
		second = NULL_TREE;
	      if (c_parser_next_token_is (parser, CPP_CLOSE_SQUARE))
		{
		  c_parser_consume_token (parser);
		  set_init_index (array_index_loc, first, second,
				  braced_init_obstack);
		  if (second)
		    pedwarn (ellipsis_loc, OPT_Wpedantic,
			     "ISO C forbids specifying range of elements to initialize");
		}
	      else
		c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
					   "expected %<]%>");
	    }
	}
      if (des_seen >= 1)
	{
	  if (c_parser_next_token_is (parser, CPP_EQ))
	    {
	      pedwarn_c90 (des_loc, OPT_Wpedantic,
			   "ISO C90 forbids specifying subobject "
			   "to initialize");
	      c_parser_consume_token (parser);
	    }
	  else
	    {
	      if (des_seen == 1)
		pedwarn (c_parser_peek_token (parser)->location, OPT_Wpedantic,
			 "obsolete use of designated initializer without %<=%>");
	      else
		{
		  struct c_expr init;
		  init.set_error ();
		  init.original_code = ERROR_MARK;
		  init.original_type = NULL;
		  c_parser_error (parser, "expected %<=%>");
		  c_parser_skip_until_found (parser, CPP_COMMA, NULL);
		  process_init_element (input_location, init, false,
					braced_init_obstack);
		  return;
		}
	    }
	}
    }
  c_parser_initval (parser, NULL, braced_init_obstack);
}

/* Parse a nested initializer; as c_parser_initializer but parses
   initializers within braced lists, after any designators have been
   applied.  If AFTER is not NULL then it is an Objective-C message
   expression which is the primary-expression starting the
   initializer.  */

static void
c_parser_initval (c_parser *parser, struct c_expr *after,
		  struct obstack * braced_init_obstack)
{
  struct c_expr init;
  gcc_assert (!after || c_dialect_objc ());
  location_t loc = c_parser_peek_token (parser)->location;

  if (c_parser_next_token_is (parser, CPP_OPEN_BRACE) && !after)
    init = c_parser_braced_init (parser, NULL_TREE, true,
				 braced_init_obstack, NULL_TREE);
  else
    {
      init = c_parser_expr_no_commas (parser, after);
      if (init.value != NULL_TREE
	  && TREE_CODE (init.value) != STRING_CST
	  && (TREE_CODE (init.value) != COMPOUND_LITERAL_EXPR
	      || C_DECL_DECLARED_CONSTEXPR (COMPOUND_LITERAL_EXPR_DECL
					    (init.value))))
	init = convert_lvalue_to_rvalue (loc, init, true, true, true);
    }
  process_init_element (loc, init, false, braced_init_obstack);
}

/* Parse a compound statement (possibly a function body) (C90 6.6.2,
   C99 6.8.2, C11 6.8.2, C2X 6.8.2).

   compound-statement:
     { block-item-list[opt] }
     { label-declarations block-item-list }

   block-item-list:
     block-item
     block-item-list block-item

   block-item:
     label
     nested-declaration
     statement

   nested-declaration:
     declaration

   GNU extensions:

   compound-statement:
     { label-declarations block-item-list }

   nested-declaration:
     __extension__ nested-declaration
     nested-function-definition

   label-declarations:
     label-declaration
     label-declarations label-declaration

   label-declaration:
     __label__ identifier-list ;

   Allowing the mixing of declarations and code is new in C99.  The
   GNU syntax also permits (not shown above) labels at the end of
   compound statements, which yield an error.  We don't allow labels
   on declarations; this might seem like a natural extension, but
   there would be a conflict between gnu-attributes on the label and
   prefix gnu-attributes on the declaration.  ??? The syntax follows the
   old parser in requiring something after label declarations.
   Although they are erroneous if the labels declared aren't defined,
   is it useful for the syntax to be this way?

   OpenACC:

   block-item:
     openacc-directive

   openacc-directive:
     update-directive

   OpenMP:

   block-item:
     openmp-directive

   openmp-directive:
     barrier-directive
     flush-directive
     taskwait-directive
     taskyield-directive
     cancel-directive
     cancellation-point-directive  */

static tree
c_parser_compound_statement (c_parser *parser, location_t *endlocp)
{
  tree stmt;
  location_t brace_loc;
  brace_loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_OPEN_BRACE, "expected %<{%>"))
    {
      /* Ensure a scope is entered and left anyway to avoid confusion
	 if we have just prepared to enter a function body.  */
      stmt = c_begin_compound_stmt (true);
      c_end_compound_stmt (brace_loc, stmt, true);
      return error_mark_node;
    }
  stmt = c_begin_compound_stmt (true);
  location_t end_loc = c_parser_compound_statement_nostart (parser);
  if (endlocp)
    *endlocp = end_loc;

  return c_end_compound_stmt (brace_loc, stmt, true);
}

/* Parse a compound statement except for the opening brace.  This is
   used for parsing both compound statements and statement expressions
   (which follow different paths to handling the opening).  */

static location_t
c_parser_compound_statement_nostart (c_parser *parser)
{
  bool last_stmt = false;
  bool last_label = false;
  bool save_valid_for_pragma = valid_location_for_stdc_pragma_p ();
  location_t label_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      location_t endloc = c_parser_peek_token (parser)->location;
      add_debug_begin_stmt (endloc);
      c_parser_consume_token (parser);
      return endloc;
    }
  mark_valid_location_for_stdc_pragma (true);
  if (c_parser_next_token_is_keyword (parser, RID_LABEL))
    {
      /* Read zero or more forward-declarations for labels that nested
	 functions can jump to.  */
      mark_valid_location_for_stdc_pragma (false);
      while (c_parser_next_token_is_keyword (parser, RID_LABEL))
	{
	  label_loc = c_parser_peek_token (parser)->location;
	  c_parser_consume_token (parser);
	  /* Any identifiers, including those declared as type names,
	     are OK here.  */
	  while (true)
	    {
	      tree label;
	      if (c_parser_next_token_is_not (parser, CPP_NAME))
		{
		  c_parser_error (parser, "expected identifier");
		  break;
		}
	      label
		= declare_label (c_parser_peek_token (parser)->value);
	      C_DECLARED_LABEL_FLAG (label) = 1;
	      add_stmt (build_stmt (label_loc, DECL_EXPR, label));
	      c_parser_consume_token (parser);
	      if (c_parser_next_token_is (parser, CPP_COMMA))
		c_parser_consume_token (parser);
	      else
		break;
	    }
	  c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
	}
      pedwarn (label_loc, OPT_Wpedantic, "ISO C forbids label declarations");
    }
  /* We must now have at least one statement, label or declaration.  */
  if (c_parser_next_token_is (parser, CPP_CLOSE_BRACE))
    {
      mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
      c_parser_error (parser, "expected declaration or statement");
      location_t endloc = c_parser_peek_token (parser)->location;
      c_parser_consume_token (parser);
      return endloc;
    }
  while (c_parser_next_token_is_not (parser, CPP_CLOSE_BRACE))
    {
      location_t loc = c_parser_peek_token (parser)->location;
      loc = expansion_point_location_if_in_system_header (loc);
      /* Standard attributes may start a label, statement or declaration.  */
      bool have_std_attrs
	= c_parser_nth_token_starts_std_attributes (parser, 1);
      tree std_attrs = NULL_TREE;
      if (have_std_attrs)
	std_attrs = c_parser_std_attribute_specifier_sequence (parser);
      if (c_parser_next_token_is_keyword (parser, RID_CASE)
	  || c_parser_next_token_is_keyword (parser, RID_DEFAULT)
	  || (c_parser_next_token_is (parser, CPP_NAME)
	      && c_parser_peek_2nd_token (parser)->type == CPP_COLON))
	{
	  if (c_parser_next_token_is_keyword (parser, RID_CASE))
	    label_loc = c_parser_peek_2nd_token (parser)->location;
	  else
	    label_loc = c_parser_peek_token (parser)->location;
	  last_label = true;
	  last_stmt = false;
	  mark_valid_location_for_stdc_pragma (false);
	  c_parser_label (parser, std_attrs);
	}
      else if (c_parser_next_tokens_start_declaration (parser)
	       || (have_std_attrs
		   && c_parser_next_token_is (parser, CPP_SEMICOLON)))
	{
	  if (last_label)
	    pedwarn_c11 (c_parser_peek_token (parser)->location, OPT_Wpedantic,
			 "a label can only be part of a statement and "
			 "a declaration is not a statement");

	  mark_valid_location_for_stdc_pragma (false);
	  bool fallthru_attr_p = false;
	  c_parser_declaration_or_fndef (parser, true, !have_std_attrs,
					 true, true, true, NULL,
					 NULL, have_std_attrs, std_attrs,
					 NULL, &fallthru_attr_p);

	  if (last_stmt && !fallthru_attr_p)
	    pedwarn_c90 (loc, OPT_Wdeclaration_after_statement,
			 "ISO C90 forbids mixed declarations and code");
	  last_stmt = fallthru_attr_p;
	  last_label = false;
	}
      else if (c_parser_next_token_is_keyword (parser, RID_EXTENSION))
	{
	  /* __extension__ can start a declaration, but is also an
	     unary operator that can start an expression.  Consume all
	     but the last of a possible series of __extension__ to
	     determine which.  If standard attributes have already
	     been seen, it must start a statement, not a declaration,
	     but standard attributes starting a declaration may appear
	     after __extension__.  */
	  while (c_parser_peek_2nd_token (parser)->type == CPP_KEYWORD
		 && (c_parser_peek_2nd_token (parser)->keyword
		     == RID_EXTENSION))
	    c_parser_consume_token (parser);
	  if (!have_std_attrs
	      && (c_token_starts_declaration (c_parser_peek_2nd_token (parser))
		  || c_parser_nth_token_starts_std_attributes (parser, 2)))
	    {
	      int ext;
	      ext = disable_extension_diagnostics ();
	      c_parser_consume_token (parser);
	      last_label = false;
	      mark_valid_location_for_stdc_pragma (false);
	      c_parser_declaration_or_fndef (parser, true, true, true, true,
					     true);
	      /* Following the old parser, __extension__ does not
		 disable this diagnostic.  */
	      restore_extension_diagnostics (ext);
	      if (last_stmt)
		pedwarn_c90 (loc, OPT_Wdeclaration_after_statement,
			     "ISO C90 forbids mixed declarations and code");
	      last_stmt = false;
	    }
	  else
	    goto statement;
	}
      else if (c_parser_next_token_is (parser, CPP_PRAGMA))
	{
	  if (have_std_attrs)
	    c_parser_error (parser, "expected declaration or statement");
	  /* External pragmas, and some omp pragmas, are not associated
	     with regular c code, and so are not to be considered statements
	     syntactically.  This ensures that the user doesn't put them
	     places that would turn into syntax errors if the directive
	     were ignored.  */
	  if (c_parser_pragma (parser,
			       last_label ? pragma_stmt : pragma_compound,
			       NULL))
	    last_label = false, last_stmt = true;
	}
      else if (c_parser_next_token_is (parser, CPP_EOF))
	{
	  mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
	  c_parser_error (parser, "expected declaration or statement");
	  return c_parser_peek_token (parser)->location;
	}
      else if (c_parser_next_token_is_keyword (parser, RID_ELSE))
        {
          if (parser->in_if_block)
            {
	      mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
	      error_at (loc, "expected %<}%> before %<else%>");
	      return c_parser_peek_token (parser)->location;
            }
          else
            {
              error_at (loc, "%<else%> without a previous %<if%>");
              c_parser_consume_token (parser);
              continue;
            }
        }
      else
	{
	statement:
	  c_warn_unused_attributes (std_attrs);
	  last_label = false;
	  last_stmt = true;
	  mark_valid_location_for_stdc_pragma (false);
	  c_parser_statement_after_labels (parser, NULL);
	}

      parser->error = false;
    }
  if (last_label)
    pedwarn_c11 (label_loc, OPT_Wpedantic, "label at end of compound statement");
  location_t endloc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);
  /* Restore the value we started with.  */
  mark_valid_location_for_stdc_pragma (save_valid_for_pragma);
  return endloc;
}

/* Parse all consecutive labels, possibly preceded by standard
   attributes.  In this context, a statement is required, not a
   declaration, so attributes must be followed by a statement that is
   not just a semicolon.  */

static void
c_parser_all_labels (c_parser *parser)
{
  tree std_attrs = NULL;
  if (c_parser_nth_token_starts_std_attributes (parser, 1))
    {
      std_attrs = c_parser_std_attribute_specifier_sequence (parser);
      if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	c_parser_error (parser, "expected statement");
    }
  while (c_parser_next_token_is_keyword (parser, RID_CASE)
	 || c_parser_next_token_is_keyword (parser, RID_DEFAULT)
	 || (c_parser_next_token_is (parser, CPP_NAME)
	     && c_parser_peek_2nd_token (parser)->type == CPP_COLON))
    {
      c_parser_label (parser, std_attrs);
      std_attrs = NULL;
      if (c_parser_nth_token_starts_std_attributes (parser, 1))
	{
	  std_attrs = c_parser_std_attribute_specifier_sequence (parser);
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    c_parser_error (parser, "expected statement");
	}
    }
   if (std_attrs)
     c_warn_unused_attributes (std_attrs);
}

/* Parse a label (C90 6.6.1, C99 6.8.1, C11 6.8.1).

   label:
     identifier : gnu-attributes[opt]
     case constant-expression :
     default :

   GNU extensions:

   label:
     case constant-expression ... constant-expression :

   The use of gnu-attributes on labels is a GNU extension.  The syntax in
   GNU C accepts any expressions without commas, non-constant
   expressions being rejected later.  Any standard
   attribute-specifier-sequence before the first label has been parsed
   in the caller, to distinguish statements from declarations.  Any
   attribute-specifier-sequence after the label is parsed in this
   function.  */
static void
c_parser_label (c_parser *parser, tree std_attrs)
{
  location_t loc1 = c_parser_peek_token (parser)->location;
  tree label = NULL_TREE;

  /* Remember whether this case or a user-defined label is allowed to fall
     through to.  */
  bool fallthrough_p = c_parser_peek_token (parser)->flags & PREV_FALLTHROUGH;

  if (c_parser_next_token_is_keyword (parser, RID_CASE))
    {
      tree exp1, exp2;
      c_parser_consume_token (parser);
      exp1 = convert_lvalue_to_rvalue (loc1,
				       c_parser_expr_no_commas (parser, NULL),
				       true, true).value;
      if (c_parser_next_token_is (parser, CPP_COLON))
	{
	  c_parser_consume_token (parser);
	  label = do_case (loc1, exp1, NULL_TREE, std_attrs);
	}
      else if (c_parser_next_token_is (parser, CPP_ELLIPSIS))
	{
	  c_parser_consume_token (parser);
	  exp2 = convert_lvalue_to_rvalue (loc1,
					   c_parser_expr_no_commas (parser,
								    NULL),
					   true, true).value;
	  if (c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	    label = do_case (loc1, exp1, exp2, std_attrs);
	}
      else
	c_parser_error (parser, "expected %<:%> or %<...%>");
    }
  else if (c_parser_next_token_is_keyword (parser, RID_DEFAULT))
    {
      c_parser_consume_token (parser);
      if (c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	label = do_case (loc1, NULL_TREE, NULL_TREE, std_attrs);
    }
  else
    {
      tree name = c_parser_peek_token (parser)->value;
      tree tlab;
      tree attrs;
      location_t loc2 = c_parser_peek_token (parser)->location;
      gcc_assert (c_parser_next_token_is (parser, CPP_NAME));
      c_parser_consume_token (parser);
      gcc_assert (c_parser_next_token_is (parser, CPP_COLON));
      c_parser_consume_token (parser);
      attrs = c_parser_gnu_attributes (parser);
      tlab = define_label (loc2, name);
      if (tlab)
	{
	  decl_attributes (&tlab, attrs, 0);
	  decl_attributes (&tlab, std_attrs, 0);
	  label = add_stmt (build_stmt (loc1, LABEL_EXPR, tlab));
	}
      if (attrs
	  && c_parser_next_tokens_start_declaration (parser))
	  warning_at (loc2, OPT_Wattributes, "GNU-style attribute between"
		      " label and declaration appertains to the label");
    }
  if (label)
    {
      if (TREE_CODE (label) == LABEL_EXPR)
	FALLTHROUGH_LABEL_P (LABEL_EXPR_LABEL (label)) = fallthrough_p;
      else
	FALLTHROUGH_LABEL_P (CASE_LABEL (label)) = fallthrough_p;
    }
}

/* Parse a statement (C90 6.6, C99 6.8, C11 6.8).

   statement:
     labeled-statement
     attribute-specifier-sequence[opt] compound-statement
     expression-statement
     attribute-specifier-sequence[opt] selection-statement
     attribute-specifier-sequence[opt] iteration-statement
     attribute-specifier-sequence[opt] jump-statement

   labeled-statement:
     attribute-specifier-sequence[opt] label statement

   expression-statement:
     expression[opt] ;
     attribute-specifier-sequence expression ;

   selection-statement:
     if-statement
     switch-statement

   iteration-statement:
     while-statement
     do-statement
     for-statement

   jump-statement:
     goto identifier ;
     continue ;
     break ;
     return expression[opt] ;

   GNU extensions:

   statement:
     attribute-specifier-sequence[opt] asm-statement

   jump-statement:
     goto * expression ;

   expression-statement:
     gnu-attributes ;

   Objective-C:

   statement:
     attribute-specifier-sequence[opt] objc-throw-statement
     attribute-specifier-sequence[opt] objc-try-catch-statement
     attribute-specifier-sequence[opt] objc-synchronized-statement

   objc-throw-statement:
     @throw expression ;
     @throw ;

   OpenACC:

   statement:
     attribute-specifier-sequence[opt] openacc-construct

   openacc-construct:
     parallel-construct
     kernels-construct
     data-construct
     loop-construct

   parallel-construct:
     parallel-directive structured-block

   kernels-construct:
     kernels-directive structured-block

   data-construct:
     data-directive structured-block

   loop-construct:
     loop-directive structured-block

   OpenMP:

   statement:
     attribute-specifier-sequence[opt] openmp-construct

   openmp-construct:
     parallel-construct
     for-construct
     simd-construct
     for-simd-construct
     sections-construct
     single-construct
     parallel-for-construct
     parallel-for-simd-construct
     parallel-sections-construct
     master-construct
     critical-construct
     atomic-construct
     ordered-construct

   parallel-construct:
     parallel-directive structured-block

   for-construct:
     for-directive iteration-statement

   simd-construct:
     simd-directive iteration-statements

   for-simd-construct:
     for-simd-directive iteration-statements

   sections-construct:
     sections-directive section-scope

   single-construct:
     single-directive structured-block

   parallel-for-construct:
     parallel-for-directive iteration-statement

   parallel-for-simd-construct:
     parallel-for-simd-directive iteration-statement

   parallel-sections-construct:
     parallel-sections-directive section-scope

   master-construct:
     master-directive structured-block

   critical-construct:
     critical-directive structured-block

   atomic-construct:
     atomic-directive expression-statement

   ordered-construct:
     ordered-directive structured-block

   Transactional Memory:

   statement:
     attribute-specifier-sequence[opt] transaction-statement
     attribute-specifier-sequence[opt] transaction-cancel-statement

   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static void
c_parser_statement (c_parser *parser, bool *if_p, location_t *loc_after_labels)
{
  c_parser_all_labels (parser);
  if (loc_after_labels)
    *loc_after_labels = c_parser_peek_token (parser)->location;
  c_parser_statement_after_labels (parser, if_p, NULL);
}

/* Parse a statement, other than a labeled statement.  CHAIN is a vector
   of if-else-if conditions.  All labels and standard attributes have
   been parsed in the caller.

   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static void
c_parser_statement_after_labels (c_parser *parser, bool *if_p,
				 vec<tree> *chain)
{
  location_t loc = c_parser_peek_token (parser)->location;
  tree stmt = NULL_TREE;
  bool in_if_block = parser->in_if_block;
  parser->in_if_block = false;
  if (if_p != NULL)
    *if_p = false;

  if (c_parser_peek_token (parser)->type != CPP_OPEN_BRACE)
    add_debug_begin_stmt (loc);

 restart:
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_OPEN_BRACE:
      add_stmt (c_parser_compound_statement (parser));
      break;
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_IF:
	  c_parser_if_statement (parser, if_p, chain);
	  break;
	case RID_SWITCH:
	  c_parser_switch_statement (parser, if_p);
	  break;
	case RID_WHILE:
	  c_parser_while_statement (parser, false, 0, if_p);
	  break;
	case RID_DO:
	  c_parser_do_statement (parser, false, 0);
	  break;
	case RID_FOR:
	  c_parser_for_statement (parser, false, 0, if_p);
	  break;
	case RID_GOTO:
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    {
	      stmt = c_finish_goto_label (loc,
					  c_parser_peek_token (parser)->value);
	      c_parser_consume_token (parser);
	    }
	  else if (c_parser_next_token_is (parser, CPP_MULT))
	    {
	      struct c_expr val;

	      c_parser_consume_token (parser);
	      val = c_parser_expression (parser);
	      val = convert_lvalue_to_rvalue (loc, val, false, true);
	      stmt = c_finish_goto_ptr (loc, val);
	    }
	  else
	    c_parser_error (parser, "expected identifier or %<*%>");
	  goto expect_semicolon;
	case RID_CONTINUE:
	  c_parser_consume_token (parser);
	  stmt = c_finish_bc_stmt (loc, objc_foreach_continue_label, false);
	  goto expect_semicolon;
	case RID_BREAK:
	  c_parser_consume_token (parser);
	  stmt = c_finish_bc_stmt (loc, objc_foreach_break_label, true);
	  goto expect_semicolon;
	case RID_RETURN:
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      stmt = c_finish_return (loc, NULL_TREE, NULL_TREE);
	      c_parser_consume_token (parser);
	    }
	  else
	    {
	      location_t xloc = c_parser_peek_token (parser)->location;
	      struct c_expr expr = c_parser_expression_conv (parser);
	      mark_exp_read (expr.value);
	      stmt = c_finish_return (EXPR_LOC_OR_LOC (expr.value, xloc),
				      expr.value, expr.original_type);
	      goto expect_semicolon;
	    }
	  break;
	case RID_ASM:
	  stmt = c_parser_asm_statement (parser);
	  break;
	case RID_ATTRIBUTE:
	  {
	    /* Allow '__attribute__((fallthrough));' or
	       '__attribute__((assume(cond)));'.  */
	    tree attrs = c_parser_gnu_attributes (parser);
	    bool has_assume = lookup_attribute ("assume", attrs);
	    if (has_assume)
	      {
		if (c_parser_next_token_is (parser, CPP_SEMICOLON))
		  attrs = handle_assume_attribute (loc, attrs, true);
		else
		  {
		    warning_at (loc, OPT_Wattributes,
				"%<assume%> attribute not followed by %<;%>");
		    has_assume = false;
		  }
	      }
	    if (attribute_fallthrough_p (attrs))
	      {
		if (c_parser_next_token_is (parser, CPP_SEMICOLON))
		  {
		    tree fn = build_call_expr_internal_loc (loc,
							    IFN_FALLTHROUGH,
							    void_type_node, 0);
		    add_stmt (fn);
		    /* Eat the ';'.  */
		    c_parser_consume_token (parser);
		  }
		else
		  warning_at (loc, OPT_Wattributes,
			      "%<fallthrough%> attribute not followed "
			      "by %<;%>");
	      }
	    else if (has_assume)
	      /* Eat the ';'.  */
	      c_parser_consume_token (parser);
	    else if (attrs != NULL_TREE)
	      warning_at (loc, OPT_Wattributes,
			  "only attribute %<fallthrough%> or %<assume%> can "
			  "be applied to a null statement");
	    break;
	  }
	default:
	  goto expr_stmt;
	}
      break;
    case CPP_SEMICOLON:
      c_parser_consume_token (parser);
      break;
    case CPP_CLOSE_PAREN:
    case CPP_CLOSE_SQUARE:
      /* Avoid infinite loop in error recovery:
	 c_parser_skip_until_found stops at a closing nesting
	 delimiter without consuming it, but here we need to consume
	 it to proceed further.  */
      c_parser_error (parser, "expected statement");
      c_parser_consume_token (parser);
      break;
    case CPP_PRAGMA:
      if (!c_parser_pragma (parser, pragma_stmt, if_p))
        goto restart;
      break;
    default:
    expr_stmt:
      stmt = c_finish_expr_stmt (loc, c_parser_expression_conv (parser).value);
    expect_semicolon:
      c_parser_skip_until_found (parser, CPP_SEMICOLON, "expected %<;%>");
      break;
    }
  /* Two cases cannot and do not have line numbers associated: If stmt
     is degenerate, such as "2;", then stmt is an INTEGER_CST, which
     cannot hold line numbers.  But that's OK because the statement
     will either be changed to a MODIFY_EXPR during gimplification of
     the statement expr, or discarded.  If stmt was compound, but
     without new variables, we will have skipped the creation of a
     BIND and will have a bare STATEMENT_LIST.  But that's OK because
     (recursively) all of the component statements should already have
     line numbers assigned.  ??? Can we discard no-op statements
     earlier?  */
  if (EXPR_LOCATION (stmt) == UNKNOWN_LOCATION)
    protected_set_expr_location (stmt, loc);

  parser->in_if_block = in_if_block;
}

/* Parse the condition from an if, do, while or for statements.  */

static tree
c_parser_condition (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  tree cond;
  cond = c_parser_expression_conv (parser).value;
  cond = c_objc_common_truthvalue_conversion (loc, cond);
  cond = c_fully_fold (cond, false, NULL);
  if (warn_sequence_point)
    verify_sequence_points (cond);
  return cond;
}

/* Parse a parenthesized condition from an if, do or while statement.

   condition:
     ( expression )
*/
static tree
c_parser_paren_condition (c_parser *parser)
{
  tree cond;
  matching_parens parens;
  if (!parens.require_open (parser))
    return error_mark_node;
  cond = c_parser_condition (parser);
  parens.skip_until_found_close (parser);
  return cond;
}

/* Parse a statement which is a block in C99.

   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static tree
c_parser_c99_block_statement (c_parser *parser, bool *if_p,
			      location_t *loc_after_labels)
{
  tree block = c_begin_compound_stmt (flag_isoc99);
  location_t loc = c_parser_peek_token (parser)->location;
  c_parser_statement (parser, if_p, loc_after_labels);
  return c_end_compound_stmt (loc, block, flag_isoc99);
}

/* Parse the body of an if statement.  This is just parsing a
   statement but (a) it is a block in C99, (b) we track whether the
   body is an if statement for the sake of -Wparentheses warnings, (c)
   we handle an empty body specially for the sake of -Wempty-body
   warnings, and (d) we call parser_compound_statement directly
   because c_parser_statement_after_labels resets
   parser->in_if_block.

   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static tree
c_parser_if_body (c_parser *parser, bool *if_p,
		  const token_indent_info &if_tinfo)
{
  tree block = c_begin_compound_stmt (flag_isoc99);
  location_t body_loc = c_parser_peek_token (parser)->location;
  location_t body_loc_after_labels = UNKNOWN_LOCATION;
  token_indent_info body_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));

  c_parser_all_labels (parser);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      location_t loc = c_parser_peek_token (parser)->location;
      add_stmt (build_empty_stmt (loc));
      c_parser_consume_token (parser);
      if (!c_parser_next_token_is_keyword (parser, RID_ELSE))
	warning_at (loc, OPT_Wempty_body,
		    "suggest braces around empty body in an %<if%> statement");
    }
  else if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
    add_stmt (c_parser_compound_statement (parser));
  else
    {
      body_loc_after_labels = c_parser_peek_token (parser)->location;
      c_parser_statement_after_labels (parser, if_p);
    }

  token_indent_info next_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  warn_for_misleading_indentation (if_tinfo, body_tinfo, next_tinfo);
  if (body_loc_after_labels != UNKNOWN_LOCATION
      && next_tinfo.type != CPP_SEMICOLON)
    warn_for_multistatement_macros (body_loc_after_labels, next_tinfo.location,
				    if_tinfo.location, RID_IF);

  return c_end_compound_stmt (body_loc, block, flag_isoc99);
}

/* Parse the else body of an if statement.  This is just parsing a
   statement but (a) it is a block in C99, (b) we handle an empty body
   specially for the sake of -Wempty-body warnings.  CHAIN is a vector
   of if-else-if conditions.  */

static tree
c_parser_else_body (c_parser *parser, const token_indent_info &else_tinfo,
		    vec<tree> *chain)
{
  location_t body_loc = c_parser_peek_token (parser)->location;
  tree block = c_begin_compound_stmt (flag_isoc99);
  token_indent_info body_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  location_t body_loc_after_labels = UNKNOWN_LOCATION;

  c_parser_all_labels (parser);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    {
      location_t loc = c_parser_peek_token (parser)->location;
      warning_at (loc,
		  OPT_Wempty_body,
	         "suggest braces around empty body in an %<else%> statement");
      add_stmt (build_empty_stmt (loc));
      c_parser_consume_token (parser);
    }
  else
    {
      if (!c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	body_loc_after_labels = c_parser_peek_token (parser)->location;
      c_parser_statement_after_labels (parser, NULL, chain);
    }

  token_indent_info next_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  warn_for_misleading_indentation (else_tinfo, body_tinfo, next_tinfo);
  if (body_loc_after_labels != UNKNOWN_LOCATION
      && next_tinfo.type != CPP_SEMICOLON)
    warn_for_multistatement_macros (body_loc_after_labels, next_tinfo.location,
				    else_tinfo.location, RID_ELSE);

  return c_end_compound_stmt (body_loc, block, flag_isoc99);
}

/* We might need to reclassify any previously-lexed identifier, e.g.
   when we've left a for loop with an if-statement without else in the
   body - we might have used a wrong scope for the token.  See PR67784.  */

static void
c_parser_maybe_reclassify_token (c_parser *parser)
{
  if (c_parser_next_token_is (parser, CPP_NAME))
    {
      c_token *token = c_parser_peek_token (parser);

      if (token->id_kind != C_ID_CLASSNAME)
	{
	  tree decl = lookup_name (token->value);

	  token->id_kind = C_ID_ID;
	  if (decl)
	    {
	      if (TREE_CODE (decl) == TYPE_DECL)
		token->id_kind = C_ID_TYPENAME;
	    }
	  else if (c_dialect_objc ())
	    {
	      tree objc_interface_decl = objc_is_class_name (token->value);
	      /* Objective-C class names are in the same namespace as
		 variables and typedefs, and hence are shadowed by local
		 declarations.  */
	      if (objc_interface_decl)
		{
		  token->value = objc_interface_decl;
		  token->id_kind = C_ID_CLASSNAME;
		}
	    }
	}
    }
}

/* Parse an if statement (C90 6.6.4, C99 6.8.4, C11 6.8.4).

   if-statement:
     if ( expression ) statement
     if ( expression ) statement else statement

   CHAIN is a vector of if-else-if conditions.
   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static void
c_parser_if_statement (c_parser *parser, bool *if_p, vec<tree> *chain)
{
  tree block;
  location_t loc;
  tree cond;
  bool nested_if = false;
  tree first_body, second_body;
  bool in_if_block;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_IF));
  token_indent_info if_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  c_parser_consume_token (parser);
  block = c_begin_compound_stmt (flag_isoc99);
  loc = c_parser_peek_token (parser)->location;
  cond = c_parser_paren_condition (parser);
  in_if_block = parser->in_if_block;
  parser->in_if_block = true;
  first_body = c_parser_if_body (parser, &nested_if, if_tinfo);
  parser->in_if_block = in_if_block;

  if (warn_duplicated_cond)
    warn_duplicated_cond_add_or_warn (EXPR_LOCATION (cond), cond, &chain);

  if (c_parser_next_token_is_keyword (parser, RID_ELSE))
    {
      token_indent_info else_tinfo
	= get_token_indent_info (c_parser_peek_token (parser));
      c_parser_consume_token (parser);
      if (warn_duplicated_cond)
	{
	  if (c_parser_next_token_is_keyword (parser, RID_IF)
	      && chain == NULL)
	    {
	      /* We've got "if (COND) else if (COND2)".  Start the
		 condition chain and add COND as the first element.  */
	      chain = new vec<tree> ();
	      if (!CONSTANT_CLASS_P (cond) && !TREE_SIDE_EFFECTS (cond))
		chain->safe_push (cond);
	    }
	  else if (!c_parser_next_token_is_keyword (parser, RID_IF))
	    /* This is if-else without subsequent if.  Zap the condition
	       chain; we would have already warned at this point.  */
	    vec_free (chain);
	}
      second_body = c_parser_else_body (parser, else_tinfo, chain);
      /* Set IF_P to true to indicate that this if statement has an
	 else clause.  This may trigger the Wparentheses warning
	 below when we get back up to the parent if statement.  */
      if (if_p != NULL)
	*if_p = true;
    }
  else
    {
      second_body = NULL_TREE;

      /* Diagnose an ambiguous else if if-then-else is nested inside
	 if-then.  */
      if (nested_if)
	warning_at (loc, OPT_Wdangling_else,
		    "suggest explicit braces to avoid ambiguous %<else%>");

      if (warn_duplicated_cond)
	/* This if statement does not have an else clause.  We don't
	   need the condition chain anymore.  */
	vec_free (chain);
    }
  c_finish_if_stmt (loc, cond, first_body, second_body);
  add_stmt (c_end_compound_stmt (loc, block, flag_isoc99));

  c_parser_maybe_reclassify_token (parser);
}

/* Parse a switch statement (C90 6.6.4, C99 6.8.4, C11 6.8.4).

   switch-statement:
     switch (expression) statement
*/

static void
c_parser_switch_statement (c_parser *parser, bool *if_p)
{
  struct c_expr ce;
  tree block, expr, body;
  unsigned char save_in_statement;
  location_t switch_loc = c_parser_peek_token (parser)->location;
  location_t switch_cond_loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_SWITCH));
  c_parser_consume_token (parser);
  block = c_begin_compound_stmt (flag_isoc99);
  bool explicit_cast_p = false;
  matching_parens parens;
  if (parens.require_open (parser))
    {
      switch_cond_loc = c_parser_peek_token (parser)->location;
      if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
	  && c_token_starts_typename (c_parser_peek_2nd_token (parser)))
	explicit_cast_p = true;
      ce = c_parser_expression (parser);
      ce = convert_lvalue_to_rvalue (switch_cond_loc, ce, true, true);
      expr = ce.value;
      /* ??? expr has no valid location?  */
      parens.skip_until_found_close (parser);
    }
  else
    {
      switch_cond_loc = UNKNOWN_LOCATION;
      expr = error_mark_node;
      ce.original_type = error_mark_node;
    }
  c_start_switch (switch_loc, switch_cond_loc, expr, explicit_cast_p);
  save_in_statement = in_statement;
  in_statement |= IN_SWITCH_STMT;
  location_t loc_after_labels;
  bool open_brace_p = c_parser_peek_token (parser)->type == CPP_OPEN_BRACE;
  body = c_parser_c99_block_statement (parser, if_p, &loc_after_labels);
  location_t next_loc = c_parser_peek_token (parser)->location;
  if (!open_brace_p && c_parser_peek_token (parser)->type != CPP_SEMICOLON)
    warn_for_multistatement_macros (loc_after_labels, next_loc, switch_loc,
				    RID_SWITCH);
  c_finish_switch (body, ce.original_type);
  in_statement = save_in_statement;
  add_stmt (c_end_compound_stmt (switch_loc, block, flag_isoc99));
  c_parser_maybe_reclassify_token (parser);
}

/* Parse a while statement (C90 6.6.5, C99 6.8.5, C11 6.8.5).

   while-statement:
      while (expression) statement

   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static void
c_parser_while_statement (c_parser *parser, bool ivdep, unsigned short unroll,
			  bool *if_p)
{
  tree block, cond, body;
  unsigned char save_in_statement;
  location_t loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_WHILE));
  token_indent_info while_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  c_parser_consume_token (parser);
  block = c_begin_compound_stmt (flag_isoc99);
  loc = c_parser_peek_token (parser)->location;
  cond = c_parser_paren_condition (parser);
  if (ivdep && cond != error_mark_node)
    cond = build3 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
		   build_int_cst (integer_type_node,
				  annot_expr_ivdep_kind),
		   integer_zero_node);
  if (unroll && cond != error_mark_node)
    cond = build3 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
		   build_int_cst (integer_type_node,
				  annot_expr_unroll_kind),
		   build_int_cst (integer_type_node, unroll));
  save_in_statement = in_statement;
  in_statement = IN_ITERATION_STMT;

  token_indent_info body_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));

  location_t loc_after_labels;
  bool open_brace = c_parser_next_token_is (parser, CPP_OPEN_BRACE);
  body = c_parser_c99_block_statement (parser, if_p, &loc_after_labels);
  add_stmt (build_stmt (loc, WHILE_STMT, cond, body));
  add_stmt (c_end_compound_stmt (loc, block, flag_isoc99));
  c_parser_maybe_reclassify_token (parser);

  token_indent_info next_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  warn_for_misleading_indentation (while_tinfo, body_tinfo, next_tinfo);

  if (next_tinfo.type != CPP_SEMICOLON && !open_brace)
    warn_for_multistatement_macros (loc_after_labels, next_tinfo.location,
				    while_tinfo.location, RID_WHILE);

  in_statement = save_in_statement;
}

/* Parse a do statement (C90 6.6.5, C99 6.8.5, C11 6.8.5).

   do-statement:
     do statement while ( expression ) ;
*/

static void
c_parser_do_statement (c_parser *parser, bool ivdep, unsigned short unroll)
{
  tree block, cond, body;
  unsigned char save_in_statement;
  location_t loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_DO));
  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
    warning_at (c_parser_peek_token (parser)->location,
		OPT_Wempty_body,
		"suggest braces around empty body in %<do%> statement");
  block = c_begin_compound_stmt (flag_isoc99);
  loc = c_parser_peek_token (parser)->location;
  save_in_statement = in_statement;
  in_statement = IN_ITERATION_STMT;
  body = c_parser_c99_block_statement (parser, NULL);
  c_parser_require_keyword (parser, RID_WHILE, "expected %<while%>");
  in_statement = save_in_statement;
  cond = c_parser_paren_condition (parser);
  if (ivdep && cond != error_mark_node)
    cond = build3 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
		   build_int_cst (integer_type_node,
				  annot_expr_ivdep_kind),
		   integer_zero_node);
  if (unroll && cond != error_mark_node)
    cond = build3 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
		   build_int_cst (integer_type_node,
				  annot_expr_unroll_kind),
 		   build_int_cst (integer_type_node, unroll));
  if (!c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
    c_parser_skip_to_end_of_block_or_statement (parser);

  add_stmt (build_stmt (loc, DO_STMT, cond, body));
  add_stmt (c_end_compound_stmt (loc, block, flag_isoc99));
}

/* Parse a for statement (C90 6.6.5, C99 6.8.5, C11 6.8.5).

   for-statement:
     for ( expression[opt] ; expression[opt] ; expression[opt] ) statement
     for ( nested-declaration expression[opt] ; expression[opt] ) statement

   The form with a declaration is new in C99.

   ??? In accordance with the old parser, the declaration may be a
   nested function, which is then rejected in check_for_loop_decls,
   but does it make any sense for this to be included in the grammar?
   Note in particular that the nested function does not include a
   trailing ';', whereas the "declaration" production includes one.
   Also, can we reject bad declarations earlier and cheaper than
   check_for_loop_decls?

   In Objective-C, there are two additional variants:

   foreach-statement:
     for ( expression in expresssion ) statement
     for ( declaration in expression ) statement

   This is inconsistent with C, because the second variant is allowed
   even if c99 is not enabled.

   The rest of the comment documents these Objective-C foreach-statement.

   Here is the canonical example of the first variant:
    for (object in array)    { do something with object }
   we call the first expression ("object") the "object_expression" and 
   the second expression ("array") the "collection_expression".
   object_expression must be an lvalue of type "id" (a generic Objective-C
   object) because the loop works by assigning to object_expression the
   various objects from the collection_expression.  collection_expression
   must evaluate to something of type "id" which responds to the method
   countByEnumeratingWithState:objects:count:.

   The canonical example of the second variant is:
    for (id object in array)    { do something with object }
   which is completely equivalent to
    {
      id object;
      for (object in array) { do something with object }
    }
   Note that initizializing 'object' in some way (eg, "for ((object =
   xxx) in array) { do something with object }") is possibly
   technically valid, but completely pointless as 'object' will be
   assigned to something else as soon as the loop starts.  We should
   most likely reject it (TODO).

   The beginning of the Objective-C foreach-statement looks exactly
   like the beginning of the for-statement, and we can tell it is a
   foreach-statement only because the initial declaration or
   expression is terminated by 'in' instead of ';'.

   IF_P is used to track whether there's a (possibly labeled) if statement
   which is not enclosed in braces and has an else clause.  This is used to
   implement -Wparentheses.  */

static void
c_parser_for_statement (c_parser *parser, bool ivdep, unsigned short unroll,
			bool *if_p)
{
  tree block, cond, incr, body;
  unsigned char save_in_statement;
  tree save_objc_foreach_break_label, save_objc_foreach_continue_label;
  /* The following are only used when parsing an ObjC foreach statement.  */
  tree object_expression;
  /* Silence the bogus uninitialized warning.  */
  tree collection_expression = NULL;
  location_t loc = c_parser_peek_token (parser)->location;
  location_t for_loc = loc;
  bool is_foreach_statement = false;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_FOR));
  token_indent_info for_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  c_parser_consume_token (parser);
  /* Open a compound statement in Objective-C as well, just in case this is
     as foreach expression.  */
  block = c_begin_compound_stmt (flag_isoc99 || c_dialect_objc ());
  cond = error_mark_node;
  incr = error_mark_node;
  matching_parens parens;
  if (parens.require_open (parser))
    {
      /* Parse the initialization declaration or expression.  */
      object_expression = error_mark_node;
      parser->objc_could_be_foreach_context = c_dialect_objc ();
      if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	{
	  parser->objc_could_be_foreach_context = false;
	  c_parser_consume_token (parser);
	  c_finish_expr_stmt (loc, NULL_TREE);
	}
      else if (c_parser_next_tokens_start_declaration (parser)
	       || c_parser_nth_token_starts_std_attributes (parser, 1))
	{
	  c_parser_declaration_or_fndef (parser, true, true, true, true, true, 
					 &object_expression);
	  parser->objc_could_be_foreach_context = false;
	  
	  if (c_parser_next_token_is_keyword (parser, RID_IN))
	    {
	      c_parser_consume_token (parser);
	      is_foreach_statement = true;
	      if (check_for_loop_decls (for_loc, true) == NULL_TREE)
		c_parser_error (parser, "multiple iterating variables in "
					"fast enumeration");
	    }
	  else
	    check_for_loop_decls (for_loc, flag_isoc99);
	}
      else if (c_parser_next_token_is_keyword (parser, RID_EXTENSION))
	{
	  /* __extension__ can start a declaration, but is also an
	     unary operator that can start an expression.  Consume all
	     but the last of a possible series of __extension__ to
	     determine which.  */
	  while (c_parser_peek_2nd_token (parser)->type == CPP_KEYWORD
		 && (c_parser_peek_2nd_token (parser)->keyword
		     == RID_EXTENSION))
	    c_parser_consume_token (parser);
	  if (c_token_starts_declaration (c_parser_peek_2nd_token (parser))
	      || c_parser_nth_token_starts_std_attributes (parser, 2))
	    {
	      int ext;
	      ext = disable_extension_diagnostics ();
	      c_parser_consume_token (parser);
	      c_parser_declaration_or_fndef (parser, true, true, true, true,
					     true, &object_expression);
	      parser->objc_could_be_foreach_context = false;
	      
	      restore_extension_diagnostics (ext);
	      if (c_parser_next_token_is_keyword (parser, RID_IN))
		{
		  c_parser_consume_token (parser);
		  is_foreach_statement = true;
		  if (check_for_loop_decls (for_loc, true) == NULL_TREE)
		    c_parser_error (parser, "multiple iterating variables in "
					    "fast enumeration");
		}
	      else
		check_for_loop_decls (for_loc, flag_isoc99);
	    }
	  else
	    goto init_expr;
	}
      else
	{
	init_expr:
	  {
	    struct c_expr ce;
	    tree init_expression;
	    ce = c_parser_expression (parser);
	    init_expression = ce.value;
	    parser->objc_could_be_foreach_context = false;
	    if (c_parser_next_token_is_keyword (parser, RID_IN))
	      {
		c_parser_consume_token (parser);
		is_foreach_statement = true;
		if (! lvalue_p (init_expression))
		  c_parser_error (parser, "invalid iterating variable in "
					  "fast enumeration");
		object_expression
		  = c_fully_fold (init_expression, false, NULL);
	      }
	    else
	      {
		ce = convert_lvalue_to_rvalue (loc, ce, true, false);
		init_expression = ce.value;
		c_finish_expr_stmt (loc, init_expression);
		c_parser_skip_until_found (parser, CPP_SEMICOLON,
					   "expected %<;%>");
	      }
	  }
	}
      /* Parse the loop condition.  In the case of a foreach
	 statement, there is no loop condition.  */
      gcc_assert (!parser->objc_could_be_foreach_context);
      if (!is_foreach_statement)
	{
	  if (c_parser_next_token_is (parser, CPP_SEMICOLON))
	    {
	      if (ivdep)
		{
		  c_parser_error (parser, "missing loop condition in loop "
					  "with %<GCC ivdep%> pragma");
		  cond = error_mark_node;
		}
	      else if (unroll)
		{
		  c_parser_error (parser, "missing loop condition in loop "
					  "with %<GCC unroll%> pragma");
		  cond = error_mark_node;
		}
	      else
		{
		  c_parser_consume_token (parser);
		  cond = NULL_TREE;
		}
	    }
	  else
	    {
	      cond = c_parser_condition (parser);
	      c_parser_skip_until_found (parser, CPP_SEMICOLON,
					 "expected %<;%>");
	    }
	  if (ivdep && cond != error_mark_node)
	    cond = build3 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
			   build_int_cst (integer_type_node,
					  annot_expr_ivdep_kind),
			   integer_zero_node);
	  if (unroll && cond != error_mark_node)
	    cond = build3 (ANNOTATE_EXPR, TREE_TYPE (cond), cond,
 			   build_int_cst (integer_type_node,
					  annot_expr_unroll_kind),
			   build_int_cst (integer_type_node, unroll));
	}
      /* Parse the increment expression (the third expression in a
	 for-statement).  In the case of a foreach-statement, this is
	 the expression that follows the 'in'.  */
      loc = c_parser_peek_token (parser)->location;
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	{
	  if (is_foreach_statement)
	    {
	      c_parser_error (parser,
			      "missing collection in fast enumeration");
	      collection_expression = error_mark_node;
	    }
	  else
	    incr = c_process_expr_stmt (loc, NULL_TREE);
	}
      else
	{
	  if (is_foreach_statement)
	    collection_expression
	      = c_fully_fold (c_parser_expression (parser).value, false, NULL);
	  else
	    {
	      struct c_expr ce = c_parser_expression (parser);
	      ce = convert_lvalue_to_rvalue (loc, ce, true, false);
	      incr = c_process_expr_stmt (loc, ce.value);
	    }
	}
      parens.skip_until_found_close (parser);
    }
  save_in_statement = in_statement;
  if (is_foreach_statement)
    {
      in_statement = IN_OBJC_FOREACH;
      save_objc_foreach_break_label = objc_foreach_break_label;
      save_objc_foreach_continue_label = objc_foreach_continue_label;
      objc_foreach_break_label = create_artificial_label (loc);
      objc_foreach_continue_label = create_artificial_label (loc);
    }
  else
    in_statement = IN_ITERATION_STMT;

  token_indent_info body_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));

  location_t loc_after_labels;
  bool open_brace = c_parser_next_token_is (parser, CPP_OPEN_BRACE);
  body = c_parser_c99_block_statement (parser, if_p, &loc_after_labels);

  if (is_foreach_statement)
    objc_finish_foreach_loop (for_loc, object_expression,
			      collection_expression, body,
			      objc_foreach_break_label,
			      objc_foreach_continue_label);
  else
    add_stmt (build_stmt (for_loc, FOR_STMT, NULL_TREE, cond, incr,
			  body, NULL_TREE));
  add_stmt (c_end_compound_stmt (for_loc, block,
				 flag_isoc99 || c_dialect_objc ()));
  c_parser_maybe_reclassify_token (parser);

  token_indent_info next_tinfo
    = get_token_indent_info (c_parser_peek_token (parser));
  warn_for_misleading_indentation (for_tinfo, body_tinfo, next_tinfo);

  if (next_tinfo.type != CPP_SEMICOLON && !open_brace)
    warn_for_multistatement_macros (loc_after_labels, next_tinfo.location,
				    for_tinfo.location, RID_FOR);

  in_statement = save_in_statement;
  if (is_foreach_statement)
    {
      objc_foreach_break_label = save_objc_foreach_break_label;
      objc_foreach_continue_label = save_objc_foreach_continue_label;
    }
}

/* Parse an asm statement, a GNU extension.  This is a full-blown asm
   statement with inputs, outputs, clobbers, and volatile, inline, and goto
   tags allowed.

   asm-qualifier:
     volatile
     inline
     goto

   asm-qualifier-list:
     asm-qualifier-list asm-qualifier
     asm-qualifier

   asm-statement:
     asm asm-qualifier-list[opt] ( asm-argument ) ;

   asm-argument:
     asm-string-literal
     asm-string-literal : asm-operands[opt]
     asm-string-literal : asm-operands[opt] : asm-operands[opt]
     asm-string-literal : asm-operands[opt] : asm-operands[opt] \
       : asm-clobbers[opt]
     asm-string-literal : : asm-operands[opt] : asm-clobbers[opt] \
       : asm-goto-operands

   The form with asm-goto-operands is valid if and only if the
   asm-qualifier-list contains goto, and is the only allowed form in that case.
   Duplicate asm-qualifiers are not allowed.

   The :: token is considered equivalent to two consecutive : tokens.  */

static tree
c_parser_asm_statement (c_parser *parser)
{
  tree str, outputs, inputs, clobbers, labels, ret;
  bool simple;
  location_t asm_loc = c_parser_peek_token (parser)->location;
  int section, nsections;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ASM));
  c_parser_consume_token (parser);

  /* Handle the asm-qualifier-list.  */
  location_t volatile_loc = UNKNOWN_LOCATION;
  location_t inline_loc = UNKNOWN_LOCATION;
  location_t goto_loc = UNKNOWN_LOCATION;
  for (;;)
    {
      c_token *token = c_parser_peek_token (parser);
      location_t loc = token->location;
      switch (token->keyword)
	{
	case RID_VOLATILE:
	  if (volatile_loc)
	    {
	      error_at (loc, "duplicate %<asm%> qualifier %qE", token->value);
	      inform (volatile_loc, "first seen here");
	    }
	  else
	    volatile_loc = loc;
	  c_parser_consume_token (parser);
	  continue;

	case RID_INLINE:
	  if (inline_loc)
	    {
	      error_at (loc, "duplicate %<asm%> qualifier %qE", token->value);
	      inform (inline_loc, "first seen here");
	    }
	  else
	    inline_loc = loc;
	  c_parser_consume_token (parser);
	  continue;

	case RID_GOTO:
	  if (goto_loc)
	    {
	      error_at (loc, "duplicate %<asm%> qualifier %qE", token->value);
	      inform (goto_loc, "first seen here");
	    }
	  else
	    goto_loc = loc;
	  c_parser_consume_token (parser);
	  continue;

	case RID_CONST:
	case RID_RESTRICT:
	  error_at (loc, "%qE is not a valid %<asm%> qualifier", token->value);
	  c_parser_consume_token (parser);
	  continue;

	default:
	  break;
	}
      break;
    }

  bool is_volatile = (volatile_loc != UNKNOWN_LOCATION);
  bool is_inline = (inline_loc != UNKNOWN_LOCATION);
  bool is_goto = (goto_loc != UNKNOWN_LOCATION);

  ret = NULL;

  matching_parens parens;
  if (!parens.require_open (parser))
    goto error;

  str = c_parser_asm_string_literal (parser);
  if (str == NULL_TREE)
    goto error_close_paren;

  simple = true;
  outputs = NULL_TREE;
  inputs = NULL_TREE;
  clobbers = NULL_TREE;
  labels = NULL_TREE;

  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN) && !is_goto)
    goto done_asm;

  /* Parse each colon-delimited section of operands.  */
  nsections = 3 + is_goto;
  for (section = 0; section < nsections; ++section)
    {
      if (c_parser_next_token_is (parser, CPP_SCOPE))
	{
	  ++section;
	  if (section == nsections)
	    {
	      c_parser_error (parser, "expected %<)%>");
	      goto error_close_paren;
	    }
	  c_parser_consume_token (parser);
	}
      else if (!c_parser_require (parser, CPP_COLON,
				  is_goto
				  ? G_("expected %<:%>")
				  : G_("expected %<:%> or %<)%>"),
				  UNKNOWN_LOCATION, is_goto))
	goto error_close_paren;

      /* Once past any colon, we're no longer a simple asm.  */
      simple = false;

      if ((!c_parser_next_token_is (parser, CPP_COLON)
	   && !c_parser_next_token_is (parser, CPP_SCOPE)
	   && !c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	  || section == 3)
	switch (section)
	  {
	  case 0:
	    outputs = c_parser_asm_operands (parser);
	    break;
	  case 1:
	    inputs = c_parser_asm_operands (parser);
	    break;
	  case 2:
	    clobbers = c_parser_asm_clobbers (parser);
	    break;
	  case 3:
	    labels = c_parser_asm_goto_operands (parser);
	    break;
	  default:
	    gcc_unreachable ();
	  }

      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN) && !is_goto)
	goto done_asm;
    }

 done_asm:
  if (!parens.require_close (parser))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      goto error;
    }

  if (!c_parser_require (parser, CPP_SEMICOLON, "expected %<;%>"))
    c_parser_skip_to_end_of_block_or_statement (parser);

  ret = build_asm_stmt (is_volatile,
			build_asm_expr (asm_loc, str, outputs, inputs,
					clobbers, labels, simple, is_inline));

 error:
  return ret;

 error_close_paren:
  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
  goto error;
}

/* Parse asm operands, a GNU extension.

   asm-operands:
     asm-operand
     asm-operands , asm-operand

   asm-operand:
     asm-string-literal ( expression )
     [ identifier ] asm-string-literal ( expression )
*/

static tree
c_parser_asm_operands (c_parser *parser)
{
  tree list = NULL_TREE;
  while (true)
    {
      tree name, str;
      struct c_expr expr;
      if (c_parser_next_token_is (parser, CPP_OPEN_SQUARE))
	{
	  c_parser_consume_token (parser);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    {
	      tree id = c_parser_peek_token (parser)->value;
	      c_parser_consume_token (parser);
	      name = build_string (IDENTIFIER_LENGTH (id),
				   IDENTIFIER_POINTER (id));
	    }
	  else
	    {
	      c_parser_error (parser, "expected identifier");
	      c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE, NULL);
	      return NULL_TREE;
	    }
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
				     "expected %<]%>");
	}
      else
	name = NULL_TREE;
      str = c_parser_asm_string_literal (parser);
      if (str == NULL_TREE)
	return NULL_TREE;
      matching_parens parens;
      if (!parens.require_open (parser))
	return NULL_TREE;
      expr = c_parser_expression (parser);
      mark_exp_read (expr.value);
      if (!parens.require_close (parser))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return NULL_TREE;
	}
      list = chainon (list, build_tree_list (build_tree_list (name, str),
					     expr.value));
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  return list;
}

/* Parse asm clobbers, a GNU extension.

   asm-clobbers:
     asm-string-literal
     asm-clobbers , asm-string-literal
*/

static tree
c_parser_asm_clobbers (c_parser *parser)
{
  tree list = NULL_TREE;
  while (true)
    {
      tree str = c_parser_asm_string_literal (parser);
      if (str)
	list = tree_cons (NULL_TREE, str, list);
      else
	return NULL_TREE;
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	break;
    }
  return list;
}

/* Parse asm goto labels, a GNU extension.

   asm-goto-operands:
     identifier
     asm-goto-operands , identifier
*/

static tree
c_parser_asm_goto_operands (c_parser *parser)
{
  tree list = NULL_TREE;
  while (true)
    {
      tree name, label;

      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  c_token *tok = c_parser_peek_token (parser);
	  name = tok->value;
	  label = lookup_label_for_goto (tok->location, name);
	  c_parser_consume_token (parser);
	  TREE_USED (label) = 1;
	}
      else
	{
	  c_parser_error (parser, "expected identifier");
	  return NULL_TREE;
	}

      name = build_string (IDENTIFIER_LENGTH (name),
			   IDENTIFIER_POINTER (name));
      list = tree_cons (name, label, list);
      if (c_parser_next_token_is (parser, CPP_COMMA))
	c_parser_consume_token (parser);
      else
	return nreverse (list);
    }
}

/* Parse a possibly concatenated sequence of string literals.
   TRANSLATE says whether to translate them to the execution character
   set; WIDE_OK says whether any kind of prefixed string literal is
   permitted in this context.  This code is based on that in
   lex_string.  */

struct c_expr
c_parser_string_literal (c_parser *parser, bool translate, bool wide_ok)
{
  struct c_expr ret;
  size_t count;
  struct obstack str_ob;
  struct obstack loc_ob;
  cpp_string str, istr, *strs;
  c_token *tok;
  location_t loc, last_tok_loc;
  enum cpp_ttype type;
  tree value, string_tree;

  tok = c_parser_peek_token (parser);
  loc = tok->location;
  last_tok_loc = linemap_resolve_location (line_table, loc,
					   LRK_MACRO_DEFINITION_LOCATION,
					   NULL);
  type = tok->type;
  switch (type)
    {
    case CPP_STRING:
    case CPP_WSTRING:
    case CPP_STRING16:
    case CPP_STRING32:
    case CPP_UTF8STRING:
      string_tree = tok->value;
      break;

    default:
      c_parser_error (parser, "expected string literal");
      ret.set_error ();
      ret.value = NULL_TREE;
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL_TREE;
      return ret;
    }

  /* Try to avoid the overhead of creating and destroying an obstack
     for the common case of just one string.  */
  switch (c_parser_peek_2nd_token (parser)->type)
    {
    default:
      c_parser_consume_token (parser);
      str.text = (const unsigned char *) TREE_STRING_POINTER (string_tree);
      str.len = TREE_STRING_LENGTH (string_tree);
      count = 1;
      strs = &str;
      break;

    case CPP_STRING:
    case CPP_WSTRING:
    case CPP_STRING16:
    case CPP_STRING32:
    case CPP_UTF8STRING:
      gcc_obstack_init (&str_ob);
      gcc_obstack_init (&loc_ob);
      count = 0;
      do
	{
	  c_parser_consume_token (parser);
	  count++;
	  str.text = (const unsigned char *) TREE_STRING_POINTER (string_tree);
	  str.len = TREE_STRING_LENGTH (string_tree);
	  if (type != tok->type)
	    {
	      if (type == CPP_STRING)
		type = tok->type;
	      else if (tok->type != CPP_STRING)
		error ("unsupported non-standard concatenation "
		       "of string literals");
	    }
	  obstack_grow (&str_ob, &str, sizeof (cpp_string));
	  obstack_grow (&loc_ob, &last_tok_loc, sizeof (location_t));
	  tok = c_parser_peek_token (parser);
	  string_tree = tok->value;
	  last_tok_loc
	    = linemap_resolve_location (line_table, tok->location,
					LRK_MACRO_DEFINITION_LOCATION, NULL);
	}
      while (tok->type == CPP_STRING
	     || tok->type == CPP_WSTRING
	     || tok->type == CPP_STRING16
	     || tok->type == CPP_STRING32
	     || tok->type == CPP_UTF8STRING);
      strs = (cpp_string *) obstack_finish (&str_ob);
    }

  if (count > 1 && !in_system_header_at (input_location))
    warning (OPT_Wtraditional,
	     "traditional C rejects string constant concatenation");

  if ((type == CPP_STRING || wide_ok)
      && ((translate
	  ? cpp_interpret_string : cpp_interpret_string_notranslate)
	  (parse_in, strs, count, &istr, type)))
    {
      value = build_string (istr.len, (const char *) istr.text);
      free (CONST_CAST (unsigned char *, istr.text));
      if (count > 1)
	{
	  location_t *locs = (location_t *) obstack_finish (&loc_ob);
	  gcc_assert (g_string_concat_db);
	  g_string_concat_db->record_string_concatenation (count, locs);
	}
    }
  else
    {
      if (type != CPP_STRING && !wide_ok)
	{
	  error_at (loc, "a wide string is invalid in this context");
	  type = CPP_STRING;
	}
      /* Callers cannot generally handle error_mark_node in this
	 context, so return the empty string instead.  An error has
	 been issued, either above or from cpp_interpret_string.  */
      switch (type)
	{
	default:
	case CPP_STRING:
	case CPP_UTF8STRING:
	  if (type == CPP_UTF8STRING && flag_char8_t)
	    {
	      value = build_string (TYPE_PRECISION (char8_type_node)
				    / TYPE_PRECISION (char_type_node),
				    "");  /* char8_t is 8 bits */
	    }
	  else
	    value = build_string (1, "");
	  break;
	case CPP_STRING16:
	  value = build_string (TYPE_PRECISION (char16_type_node)
				/ TYPE_PRECISION (char_type_node),
				"\0");  /* char16_t is 16 bits */
	  break;
	case CPP_STRING32:
	  value = build_string (TYPE_PRECISION (char32_type_node)
				/ TYPE_PRECISION (char_type_node),
				"\0\0\0");  /* char32_t is 32 bits */
	  break;
	case CPP_WSTRING:
	  value = build_string (TYPE_PRECISION (wchar_type_node)
				/ TYPE_PRECISION (char_type_node),
				"\0\0\0");  /* widest supported wchar_t
					       is 32 bits */
	  break;
        }
    }

  switch (type)
    {
    default:
    case CPP_STRING:
      TREE_TYPE (value) = char_array_type_node;
      break;
    case CPP_UTF8STRING:
      if (flag_char8_t)
	TREE_TYPE (value) = char8_array_type_node;
      else
	TREE_TYPE (value) = char_array_type_node;
      break;
    case CPP_STRING16:
      TREE_TYPE (value) = char16_array_type_node;
      break;
    case CPP_STRING32:
      TREE_TYPE (value) = char32_array_type_node;
      break;
    case CPP_WSTRING:
      TREE_TYPE (value) = wchar_array_type_node;
    }
  value = fix_string_type (value);

  if (count > 1)
    {
      obstack_free (&str_ob, 0);
      obstack_free (&loc_ob, 0);
    }

  ret.value = value;
  ret.original_code = STRING_CST;
  ret.original_type = NULL_TREE;
  set_c_expr_source_range (&ret, get_range_from_loc (line_table, loc));
  ret.m_decimal = 0;
  parser->seen_string_literal = true;
  return ret;
}

/* Parse an expression other than a compound expression; that is, an
   assignment expression (C90 6.3.16, C99 6.5.16, C11 6.5.16).  If
   AFTER is not NULL then it is an Objective-C message expression which
   is the primary-expression starting the expression as an initializer.

   assignment-expression:
     conditional-expression
     unary-expression assignment-operator assignment-expression

   assignment-operator: one of
     = *= /= %= += -= <<= >>= &= ^= |=

   In GNU C we accept any conditional expression on the LHS and
   diagnose the invalid lvalue rather than producing a syntax
   error.  */

static struct c_expr
c_parser_expr_no_commas (c_parser *parser, struct c_expr *after,
			 tree omp_atomic_lhs)
{
  struct c_expr lhs, rhs, ret;
  enum tree_code code;
  location_t op_location, exp_location;
  bool save_in_omp_for = c_in_omp_for;
  c_in_omp_for = false;
  gcc_assert (!after || c_dialect_objc ());
  lhs = c_parser_conditional_expression (parser, after, omp_atomic_lhs);
  op_location = c_parser_peek_token (parser)->location;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_EQ:
      code = NOP_EXPR;
      break;
    case CPP_MULT_EQ:
      code = MULT_EXPR;
      break;
    case CPP_DIV_EQ:
      code = TRUNC_DIV_EXPR;
      break;
    case CPP_MOD_EQ:
      code = TRUNC_MOD_EXPR;
      break;
    case CPP_PLUS_EQ:
      code = PLUS_EXPR;
      break;
    case CPP_MINUS_EQ:
      code = MINUS_EXPR;
      break;
    case CPP_LSHIFT_EQ:
      code = LSHIFT_EXPR;
      break;
    case CPP_RSHIFT_EQ:
      code = RSHIFT_EXPR;
      break;
    case CPP_AND_EQ:
      code = BIT_AND_EXPR;
      break;
    case CPP_XOR_EQ:
      code = BIT_XOR_EXPR;
      break;
    case CPP_OR_EQ:
      code = BIT_IOR_EXPR;
      break;
    default:
      c_in_omp_for = save_in_omp_for;
      return lhs;
    }
  c_parser_consume_token (parser);
  exp_location = c_parser_peek_token (parser)->location;
  rhs = c_parser_expr_no_commas (parser, NULL);
  rhs = convert_lvalue_to_rvalue (exp_location, rhs, true, true);
  
  ret.value = build_modify_expr (op_location, lhs.value, lhs.original_type,
				 code, exp_location, rhs.value,
				 rhs.original_type);
  ret.m_decimal = 0;
  set_c_expr_source_range (&ret, lhs.get_start (), rhs.get_finish ());
  if (code == NOP_EXPR)
    ret.original_code = MODIFY_EXPR;
  else
    {
      suppress_warning (ret.value, OPT_Wparentheses);
      ret.original_code = ERROR_MARK;
    }
  ret.original_type = NULL;
  c_in_omp_for = save_in_omp_for;
  return ret;
}

/* Parse a conditional expression (C90 6.3.15, C99 6.5.15, C11 6.5.15).  If
   AFTER is not NULL then it is an Objective-C message expression which is
   the primary-expression starting the expression as an initializer.

   conditional-expression:
     logical-OR-expression
     logical-OR-expression ? expression : conditional-expression

   GNU extensions:

   conditional-expression:
     logical-OR-expression ? : conditional-expression
*/

static struct c_expr
c_parser_conditional_expression (c_parser *parser, struct c_expr *after,
				 tree omp_atomic_lhs)
{
  struct c_expr cond, exp1, exp2, ret;
  location_t start, cond_loc, colon_loc;

  gcc_assert (!after || c_dialect_objc ());

  cond = c_parser_binary_expression (parser, after, omp_atomic_lhs);

  if (c_parser_next_token_is_not (parser, CPP_QUERY))
    return cond;
  if (cond.value != error_mark_node)
    start = cond.get_start ();
  else
    start = UNKNOWN_LOCATION;
  cond_loc = c_parser_peek_token (parser)->location;
  cond = convert_lvalue_to_rvalue (cond_loc, cond, true, true);
  c_parser_consume_token (parser);
  if (c_parser_next_token_is (parser, CPP_COLON))
    {
      tree eptype = NULL_TREE;

      location_t middle_loc = c_parser_peek_token (parser)->location;
      pedwarn (middle_loc, OPT_Wpedantic,
	       "ISO C forbids omitting the middle term of a %<?:%> expression");
      if (TREE_CODE (cond.value) == EXCESS_PRECISION_EXPR)
	{
	  eptype = TREE_TYPE (cond.value);
	  cond.value = TREE_OPERAND (cond.value, 0);
	}
      tree e = cond.value;
      while (TREE_CODE (e) == COMPOUND_EXPR)
	e = TREE_OPERAND (e, 1);
      warn_for_omitted_condop (middle_loc, e);
      /* Make sure first operand is calculated only once.  */
      exp1.value = save_expr (default_conversion (cond.value));
      if (eptype)
	exp1.value = build1 (EXCESS_PRECISION_EXPR, eptype, exp1.value);
      exp1.original_type = NULL;
      exp1.src_range = cond.src_range;
      cond.value = c_objc_common_truthvalue_conversion (cond_loc, exp1.value);
      c_inhibit_evaluation_warnings += cond.value == truthvalue_true_node;
    }
  else
    {
      cond.value
	= c_objc_common_truthvalue_conversion
	(cond_loc, default_conversion (cond.value));
      c_inhibit_evaluation_warnings += cond.value == truthvalue_false_node;
      exp1 = c_parser_expression_conv (parser);
      mark_exp_read (exp1.value);
      c_inhibit_evaluation_warnings +=
	((cond.value == truthvalue_true_node)
	 - (cond.value == truthvalue_false_node));
    }

  colon_loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
    {
      c_inhibit_evaluation_warnings -= cond.value == truthvalue_true_node;
      ret.set_error ();
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      return ret;
    }
  {
    location_t exp2_loc = c_parser_peek_token (parser)->location;
    exp2 = c_parser_conditional_expression (parser, NULL, NULL_TREE);
    exp2 = convert_lvalue_to_rvalue (exp2_loc, exp2, true, true);
  }
  c_inhibit_evaluation_warnings -= cond.value == truthvalue_true_node;
  location_t loc1 = make_location (exp1.get_start (), exp1.src_range);
  location_t loc2 = make_location (exp2.get_start (), exp2.src_range);
  if (UNLIKELY (omp_atomic_lhs != NULL)
      && (TREE_CODE (cond.value) == GT_EXPR
	  || TREE_CODE (cond.value) == LT_EXPR
	  || TREE_CODE (cond.value) == EQ_EXPR)
      && c_tree_equal (exp2.value, omp_atomic_lhs)
      && (c_tree_equal (TREE_OPERAND (cond.value, 0), omp_atomic_lhs)
	  || c_tree_equal (TREE_OPERAND (cond.value, 1), omp_atomic_lhs)))
    ret.value = build3_loc (colon_loc, COND_EXPR, TREE_TYPE (omp_atomic_lhs),
			    cond.value, exp1.value, exp2.value);
  else
    ret.value
      = build_conditional_expr (colon_loc, cond.value,
				cond.original_code == C_MAYBE_CONST_EXPR,
				exp1.value, exp1.original_type, loc1,
				exp2.value, exp2.original_type, loc2);
  ret.original_code = ERROR_MARK;
  if (exp1.value == error_mark_node || exp2.value == error_mark_node)
    ret.original_type = NULL;
  else
    {
      tree t1, t2;

      /* If both sides are enum type, the default conversion will have
	 made the type of the result be an integer type.  We want to
	 remember the enum types we started with.  */
      t1 = exp1.original_type ? exp1.original_type : TREE_TYPE (exp1.value);
      t2 = exp2.original_type ? exp2.original_type : TREE_TYPE (exp2.value);
      ret.original_type = ((t1 != error_mark_node
			    && t2 != error_mark_node
			    && (TYPE_MAIN_VARIANT (t1)
				== TYPE_MAIN_VARIANT (t2)))
			   ? t1
			   : NULL);
    }
  set_c_expr_source_range (&ret, start, exp2.get_finish ());
  ret.m_decimal = 0;
  return ret;
}

/* Parse a binary expression; that is, a logical-OR-expression (C90
   6.3.5-6.3.14, C99 6.5.5-6.5.14, C11 6.5.5-6.5.14).  If AFTER is not
   NULL then it is an Objective-C message expression which is the
   primary-expression starting the expression as an initializer.

   OMP_ATOMIC_LHS is NULL, unless parsing OpenMP #pragma omp atomic,
   when it should be the unfolded lhs.  In a valid OpenMP source,
   one of the operands of the toplevel binary expression must be equal
   to it.  In that case, just return a build2 created binary operation
   rather than result of parser_build_binary_op.

   multiplicative-expression:
     cast-expression
     multiplicative-expression * cast-expression
     multiplicative-expression / cast-expression
     multiplicative-expression % cast-expression

   additive-expression:
     multiplicative-expression
     additive-expression + multiplicative-expression
     additive-expression - multiplicative-expression

   shift-expression:
     additive-expression
     shift-expression << additive-expression
     shift-expression >> additive-expression

   relational-expression:
     shift-expression
     relational-expression < shift-expression
     relational-expression > shift-expression
     relational-expression <= shift-expression
     relational-expression >= shift-expression

   equality-expression:
     relational-expression
     equality-expression == relational-expression
     equality-expression != relational-expression

   AND-expression:
     equality-expression
     AND-expression & equality-expression

   exclusive-OR-expression:
     AND-expression
     exclusive-OR-expression ^ AND-expression

   inclusive-OR-expression:
     exclusive-OR-expression
     inclusive-OR-expression | exclusive-OR-expression

   logical-AND-expression:
     inclusive-OR-expression
     logical-AND-expression && inclusive-OR-expression

   logical-OR-expression:
     logical-AND-expression
     logical-OR-expression || logical-AND-expression
*/

static struct c_expr
c_parser_binary_expression (c_parser *parser, struct c_expr *after,
			    tree omp_atomic_lhs)
{
  /* A binary expression is parsed using operator-precedence parsing,
     with the operands being cast expressions.  All the binary
     operators are left-associative.  Thus a binary expression is of
     form:

     E0 op1 E1 op2 E2 ...

     which we represent on a stack.  On the stack, the precedence
     levels are strictly increasing.  When a new operator is
     encountered of higher precedence than that at the top of the
     stack, it is pushed; its LHS is the top expression, and its RHS
     is everything parsed until it is popped.  When a new operator is
     encountered with precedence less than or equal to that at the top
     of the stack, triples E[i-1] op[i] E[i] are popped and replaced
     by the result of the operation until the operator at the top of
     the stack has lower precedence than the new operator or there is
     only one element on the stack; then the top expression is the LHS
     of the new operator.  In the case of logical AND and OR
     expressions, we also need to adjust c_inhibit_evaluation_warnings
     as appropriate when the operators are pushed and popped.  */

  struct {
    /* The expression at this stack level.  */
    struct c_expr expr;
    /* The precedence of the operator on its left, PREC_NONE at the
       bottom of the stack.  */
    enum c_parser_prec prec;
    /* The operation on its left.  */
    enum tree_code op;
    /* The source location of this operation.  */
    location_t loc;
    /* The sizeof argument if expr.original_code == {PAREN_,}SIZEOF_EXPR.  */
    tree sizeof_arg;
  } stack[NUM_PRECS];
  int sp;
  /* Location of the binary operator.  */
  location_t binary_loc = UNKNOWN_LOCATION;  /* Quiet warning.  */
#define POP								      \
  do {									      \
    switch (stack[sp].op)						      \
      {									      \
      case TRUTH_ANDIF_EXPR:						      \
	c_inhibit_evaluation_warnings -= (stack[sp - 1].expr.value	      \
					  == truthvalue_false_node);	      \
	break;								      \
      case TRUTH_ORIF_EXPR:						      \
	c_inhibit_evaluation_warnings -= (stack[sp - 1].expr.value	      \
					  == truthvalue_true_node);	      \
	break;								      \
      case TRUNC_DIV_EXPR:						      \
	if ((stack[sp - 1].expr.original_code == SIZEOF_EXPR		      \
	     || stack[sp - 1].expr.original_code == PAREN_SIZEOF_EXPR)	      \
	    && (stack[sp].expr.original_code == SIZEOF_EXPR		      \
		|| stack[sp].expr.original_code == PAREN_SIZEOF_EXPR))	      \
	  {								      \
	    tree type0 = stack[sp - 1].sizeof_arg;			      \
	    tree type1 = stack[sp].sizeof_arg;				      \
	    tree first_arg = type0;					      \
	    if (!TYPE_P (type0))					      \
	      type0 = TREE_TYPE (type0);				      \
	    if (!TYPE_P (type1))					      \
	      type1 = TREE_TYPE (type1);				      \
	    if (POINTER_TYPE_P (type0)					      \
		&& comptypes (TREE_TYPE (type0), type1)			      \
		&& !(TREE_CODE (first_arg) == PARM_DECL			      \
		     && C_ARRAY_PARAMETER (first_arg)			      \
		     && warn_sizeof_array_argument))			      \
	      {								      \
		auto_diagnostic_group d;				      \
		if (warning_at (stack[sp].loc, OPT_Wsizeof_pointer_div,	      \
				  "division %<sizeof (%T) / sizeof (%T)%> "   \
				  "does not compute the number of array "     \
				  "elements",				      \
				  type0, type1))			      \
		  if (DECL_P (first_arg))				      \
		    inform (DECL_SOURCE_LOCATION (first_arg),		      \
			      "first %<sizeof%> operand was declared here");  \
	      }								      \
	    else if (TREE_CODE (type0) == ARRAY_TYPE			      \
		     && !char_type_p (TYPE_MAIN_VARIANT (TREE_TYPE (type0)))  \
		     && stack[sp].expr.original_code != PAREN_SIZEOF_EXPR)    \
	      maybe_warn_sizeof_array_div (stack[sp].loc, first_arg, type0,   \
					   stack[sp].sizeof_arg, type1);      \
	  }								      \
	break;								      \
      default:								      \
	break;								      \
      }									      \
    stack[sp - 1].expr							      \
      = convert_lvalue_to_rvalue (stack[sp - 1].loc,			      \
				  stack[sp - 1].expr, true, true);	      \
    stack[sp].expr							      \
      = convert_lvalue_to_rvalue (stack[sp].loc,			      \
				  stack[sp].expr, true, true);		      \
    if (UNLIKELY (omp_atomic_lhs != NULL_TREE) && sp == 1		      \
	&& ((c_parser_next_token_is (parser, CPP_SEMICOLON)		      \
	     && ((1 << stack[sp].prec)					      \
		 & ((1 << PREC_BITOR) | (1 << PREC_BITXOR)		      \
		     | (1 << PREC_BITAND) | (1 << PREC_SHIFT)		      \
		     | (1 << PREC_ADD) | (1 << PREC_MULT)		      \
		     | (1 << PREC_EQ))))				      \
	    || ((c_parser_next_token_is (parser, CPP_QUERY)		      \
		 || (omp_atomic_lhs == void_list_node			      \
		     && c_parser_next_token_is (parser, CPP_CLOSE_PAREN)))    \
		&& (stack[sp].prec == PREC_REL || stack[sp].prec == PREC_EQ)))\
	&& stack[sp].op != TRUNC_MOD_EXPR				      \
	&& stack[sp].op != GE_EXPR					      \
	&& stack[sp].op != LE_EXPR					      \
	&& stack[sp].op != NE_EXPR					      \
	&& stack[0].expr.value != error_mark_node			      \
	&& stack[1].expr.value != error_mark_node			      \
	&& (omp_atomic_lhs == void_list_node				      \
	    || c_tree_equal (stack[0].expr.value, omp_atomic_lhs)	      \
	    || c_tree_equal (stack[1].expr.value, omp_atomic_lhs)	      \
	    || (stack[sp].op == EQ_EXPR					      \
		&& c_parser_peek_2nd_token (parser)->keyword == RID_IF)))     \
      {									      \
	tree t = make_node (stack[1].op);				      \
	TREE_TYPE (t) = TREE_TYPE (stack[0].expr.value);		      \
	TREE_OPERAND (t, 0) = stack[0].expr.value;			      \
	TREE_OPERAND (t, 1) = stack[1].expr.value;			      \
	stack[0].expr.value = t;					      \
	stack[0].expr.m_decimal = 0;					      \
      }									      \
    else								      \
      stack[sp - 1].expr = parser_build_binary_op (stack[sp].loc,	      \
						   stack[sp].op,	      \
						   stack[sp - 1].expr,	      \
						   stack[sp].expr);	      \
    sp--;								      \
  } while (0)
  gcc_assert (!after || c_dialect_objc ());
  stack[0].loc = c_parser_peek_token (parser)->location;
  stack[0].expr = c_parser_cast_expression (parser, after);
  stack[0].prec = PREC_NONE;
  stack[0].sizeof_arg = c_last_sizeof_arg;
  sp = 0;
  while (true)
    {
      enum c_parser_prec oprec;
      enum tree_code ocode;
      source_range src_range;
      if (parser->error)
	goto out;
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_MULT:
	  oprec = PREC_MULT;
	  ocode = MULT_EXPR;
	  break;
	case CPP_DIV:
	  oprec = PREC_MULT;
	  ocode = TRUNC_DIV_EXPR;
	  break;
	case CPP_MOD:
	  oprec = PREC_MULT;
	  ocode = TRUNC_MOD_EXPR;
	  break;
	case CPP_PLUS:
	  oprec = PREC_ADD;
	  ocode = PLUS_EXPR;
	  break;
	case CPP_MINUS:
	  oprec = PREC_ADD;
	  ocode = MINUS_EXPR;
	  break;
	case CPP_LSHIFT:
	  oprec = PREC_SHIFT;
	  ocode = LSHIFT_EXPR;
	  break;
	case CPP_RSHIFT:
	  oprec = PREC_SHIFT;
	  ocode = RSHIFT_EXPR;
	  break;
	case CPP_LESS:
	  oprec = PREC_REL;
	  ocode = LT_EXPR;
	  break;
	case CPP_GREATER:
	  oprec = PREC_REL;
	  ocode = GT_EXPR;
	  break;
	case CPP_LESS_EQ:
	  oprec = PREC_REL;
	  ocode = LE_EXPR;
	  break;
	case CPP_GREATER_EQ:
	  oprec = PREC_REL;
	  ocode = GE_EXPR;
	  break;
	case CPP_EQ_EQ:
	  oprec = PREC_EQ;
	  ocode = EQ_EXPR;
	  break;
	case CPP_NOT_EQ:
	  oprec = PREC_EQ;
	  ocode = NE_EXPR;
	  break;
	case CPP_AND:
	  oprec = PREC_BITAND;
	  ocode = BIT_AND_EXPR;
	  break;
	case CPP_XOR:
	  oprec = PREC_BITXOR;
	  ocode = BIT_XOR_EXPR;
	  break;
	case CPP_OR:
	  oprec = PREC_BITOR;
	  ocode = BIT_IOR_EXPR;
	  break;
	case CPP_AND_AND:
	  oprec = PREC_LOGAND;
	  ocode = TRUTH_ANDIF_EXPR;
	  break;
	case CPP_OR_OR:
	  oprec = PREC_LOGOR;
	  ocode = TRUTH_ORIF_EXPR;
	  break;
	default:
	  /* Not a binary operator, so end of the binary
	     expression.  */
	  goto out;
	}
      binary_loc = c_parser_peek_token (parser)->location;
      while (oprec <= stack[sp].prec)
	POP;
      c_parser_consume_token (parser);
      switch (ocode)
	{
	case TRUTH_ANDIF_EXPR:
	  src_range = stack[sp].expr.src_range;
	  stack[sp].expr
	    = convert_lvalue_to_rvalue (stack[sp].loc,
					stack[sp].expr, true, true);
	  stack[sp].expr.value = c_objc_common_truthvalue_conversion
	    (stack[sp].loc, default_conversion (stack[sp].expr.value));
	  c_inhibit_evaluation_warnings += (stack[sp].expr.value
					    == truthvalue_false_node);
	  set_c_expr_source_range (&stack[sp].expr, src_range);
	  break;
	case TRUTH_ORIF_EXPR:
	  src_range = stack[sp].expr.src_range;
	  stack[sp].expr
	    = convert_lvalue_to_rvalue (stack[sp].loc,
					stack[sp].expr, true, true);
	  stack[sp].expr.value = c_objc_common_truthvalue_conversion
	    (stack[sp].loc, default_conversion (stack[sp].expr.value));
	  c_inhibit_evaluation_warnings += (stack[sp].expr.value
					    == truthvalue_true_node);
	  set_c_expr_source_range (&stack[sp].expr, src_range);
	  break;
	default:
	  break;
	}
      sp++;
      stack[sp].loc = binary_loc;
      stack[sp].expr = c_parser_cast_expression (parser, NULL);
      stack[sp].prec = oprec;
      stack[sp].op = ocode;
      stack[sp].sizeof_arg = c_last_sizeof_arg;
    }
 out:
  while (sp > 0)
    POP;
  return stack[0].expr;
#undef POP
}

/* Parse any storage class specifiers after an open parenthesis in a
   context where a compound literal is permitted.  */

static struct c_declspecs *
c_parser_compound_literal_scspecs (c_parser *parser)
{
  bool seen_scspec = false;
  struct c_declspecs *specs = build_null_declspecs ();
  while (c_parser_next_token_is (parser, CPP_KEYWORD))
    {
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_CONSTEXPR:
	case RID_REGISTER:
	case RID_STATIC:
	case RID_THREAD:
	  seen_scspec = true;
	  declspecs_add_scspec (c_parser_peek_token (parser)->location,
				specs, c_parser_peek_token (parser)->value);
	  c_parser_consume_token (parser);
	  break;
	default:
	  goto out;
	}
    }
 out:
  return seen_scspec ? specs : NULL;
}

/* Parse a cast expression (C90 6.3.4, C99 6.5.4, C11 6.5.4).  If AFTER
   is not NULL then it is an Objective-C message expression which is the
   primary-expression starting the expression as an initializer.

   cast-expression:
     unary-expression
     ( type-name ) unary-expression
*/

static struct c_expr
c_parser_cast_expression (c_parser *parser, struct c_expr *after)
{
  location_t cast_loc = c_parser_peek_token (parser)->location;
  gcc_assert (!after || c_dialect_objc ());
  if (after)
    return c_parser_postfix_expression_after_primary (parser,
						      cast_loc, *after);
  /* If the expression begins with a parenthesized type name, it may
     be either a cast or a compound literal; we need to see whether
     the next character is '{' to tell the difference.  If not, it is
     an unary expression.  Full detection of unknown typenames here
     would require a 3-token lookahead.  */
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
      && c_token_starts_compound_literal (c_parser_peek_2nd_token (parser)))
    {
      struct c_declspecs *scspecs;
      struct c_type_name *type_name;
      struct c_expr ret;
      struct c_expr expr;
      matching_parens parens;
      parens.consume_open (parser);
      scspecs = c_parser_compound_literal_scspecs (parser);
      type_name = c_parser_type_name (parser, true);
      parens.skip_until_found_close (parser);
      if (type_name == NULL)
	{
	  ret.set_error ();
	  ret.original_code = ERROR_MARK;
	  ret.original_type = NULL;
	  return ret;
	}

      /* Save casted types in the function's used types hash table.  */
      used_types_insert (type_name->specs->type);

      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	return c_parser_postfix_expression_after_paren_type (parser, scspecs,
							     type_name,
							     cast_loc);
      if (scspecs)
	error_at (cast_loc, "storage class specifier in cast");
      if (type_name->specs->alignas_p)
	error_at (type_name->specs->locations[cdw_alignas],
		  "alignment specified for type name in cast");
      {
	location_t expr_loc = c_parser_peek_token (parser)->location;
	expr = c_parser_cast_expression (parser, NULL);
	expr = convert_lvalue_to_rvalue (expr_loc, expr, true, true);
      }
      ret.value = c_cast_expr (cast_loc, type_name, expr.value);
      if (ret.value && expr.value)
	set_c_expr_source_range (&ret, cast_loc, expr.get_finish ());
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      ret.m_decimal = 0;
      return ret;
    }
  else
    return c_parser_unary_expression (parser);
}

/* Parse an unary expression (C90 6.3.3, C99 6.5.3, C11 6.5.3).

   unary-expression:
     postfix-expression
     ++ unary-expression
     -- unary-expression
     unary-operator cast-expression
     sizeof unary-expression
     sizeof ( type-name )

   unary-operator: one of
     & * + - ~ !

   GNU extensions:

   unary-expression:
     __alignof__ unary-expression
     __alignof__ ( type-name )
     && identifier

   (C11 permits _Alignof with type names only.)

   unary-operator: one of
     __extension__ __real__ __imag__

   Transactional Memory:

   unary-expression:
     transaction-expression

   In addition, the GNU syntax treats ++ and -- as unary operators, so
   they may be applied to cast expressions with errors for non-lvalues
   given later.  */

static struct c_expr
c_parser_unary_expression (c_parser *parser)
{
  int ext;
  struct c_expr ret, op;
  location_t op_loc = c_parser_peek_token (parser)->location;
  location_t exp_loc;
  location_t finish;
  ret.original_code = ERROR_MARK;
  ret.original_type = NULL;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_PLUS_PLUS:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);

      op = default_function_array_read_conversion (exp_loc, op);
      return parser_build_unary_op (op_loc, PREINCREMENT_EXPR, op);
    case CPP_MINUS_MINUS:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      
      op = default_function_array_read_conversion (exp_loc, op);
      return parser_build_unary_op (op_loc, PREDECREMENT_EXPR, op);
    case CPP_AND:
      c_parser_consume_token (parser);
      op = c_parser_cast_expression (parser, NULL);
      mark_exp_read (op.value);
      return parser_build_unary_op (op_loc, ADDR_EXPR, op);
    case CPP_MULT:
      {
	c_parser_consume_token (parser);
	exp_loc = c_parser_peek_token (parser)->location;
	op = c_parser_cast_expression (parser, NULL);
	finish = op.get_finish ();
	op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
	location_t combined_loc = make_location (op_loc, op_loc, finish);
	ret.value = build_indirect_ref (combined_loc, op.value, RO_UNARY_STAR);
	ret.src_range.m_start = op_loc;
	ret.src_range.m_finish = finish;
	ret.m_decimal = 0;
	return ret;
      }
    case CPP_PLUS:
      if (!c_dialect_objc () && !in_system_header_at (input_location))
	warning_at (op_loc,
		    OPT_Wtraditional,
		    "traditional C rejects the unary plus operator");
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, CONVERT_EXPR, op);
    case CPP_MINUS:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, NEGATE_EXPR, op);
    case CPP_COMPL:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, BIT_NOT_EXPR, op);
    case CPP_NOT:
      c_parser_consume_token (parser);
      exp_loc = c_parser_peek_token (parser)->location;
      op = c_parser_cast_expression (parser, NULL);
      op = convert_lvalue_to_rvalue (exp_loc, op, true, true);
      return parser_build_unary_op (op_loc, TRUTH_NOT_EXPR, op);
    case CPP_AND_AND:
      /* Refer to the address of a label as a pointer.  */
      c_parser_consume_token (parser);
      if (c_parser_next_token_is (parser, CPP_NAME))
	{
	  ret.value = finish_label_address_expr
	    (c_parser_peek_token (parser)->value, op_loc);
	  set_c_expr_source_range (&ret, op_loc,
				   c_parser_peek_token (parser)->get_finish ());
	  c_parser_consume_token (parser);
	}
      else
	{
	  c_parser_error (parser, "expected identifier");
	  ret.set_error ();
	}
      return ret;
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_SIZEOF:
	  return c_parser_sizeof_expression (parser);
	case RID_ALIGNOF:
	  return c_parser_alignof_expression (parser);
	case RID_BUILTIN_HAS_ATTRIBUTE:
	  return c_parser_has_attribute_expression (parser);
	case RID_EXTENSION:
	  c_parser_consume_token (parser);
	  ext = disable_extension_diagnostics ();
	  ret = c_parser_cast_expression (parser, NULL);
	  restore_extension_diagnostics (ext);
	  return ret;
	case RID_REALPART:
	  c_parser_consume_token (parser);
	  exp_loc = c_parser_peek_token (parser)->location;
	  op = c_parser_cast_expression (parser, NULL);
	  op = default_function_array_conversion (exp_loc, op);
	  return parser_build_unary_op (op_loc, REALPART_EXPR, op);
	case RID_IMAGPART:
	  c_parser_consume_token (parser);
	  exp_loc = c_parser_peek_token (parser)->location;
	  op = c_parser_cast_expression (parser, NULL);
	  op = default_function_array_conversion (exp_loc, op);
	  return parser_build_unary_op (op_loc, IMAGPART_EXPR, op);
	default:
	  return c_parser_postfix_expression (parser);
	}
    default:
      return c_parser_postfix_expression (parser);
    }
}

/* Parse a sizeof expression.  */

static struct c_expr
c_parser_sizeof_expression (c_parser *parser)
{
  struct c_expr expr;
  struct c_expr result;
  location_t expr_loc;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_SIZEOF));

  location_t start;
  location_t finish = UNKNOWN_LOCATION;

  start = c_parser_peek_token (parser)->location;

  c_parser_consume_token (parser);
  c_inhibit_evaluation_warnings++;
  in_sizeof++;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
      && c_token_starts_compound_literal (c_parser_peek_2nd_token (parser)))
    {
      /* Either sizeof ( type-name ) or sizeof unary-expression
	 starting with a compound literal.  */
      struct c_declspecs *scspecs;
      struct c_type_name *type_name;
      matching_parens parens;
      parens.consume_open (parser);
      expr_loc = c_parser_peek_token (parser)->location;
      scspecs = c_parser_compound_literal_scspecs (parser);
      type_name = c_parser_type_name (parser, true);
      parens.skip_until_found_close (parser);
      finish = parser->tokens_buf[0].location;
      if (type_name == NULL)
	{
	  struct c_expr ret;
	  c_inhibit_evaluation_warnings--;
	  in_sizeof--;
	  ret.set_error ();
	  ret.original_code = ERROR_MARK;
	  ret.original_type = NULL;
	  return ret;
	}
      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	{
	  expr = c_parser_postfix_expression_after_paren_type (parser, scspecs,
							       type_name,
							       expr_loc);
	  finish = expr.get_finish ();
	  goto sizeof_expr;
	}
      /* sizeof ( type-name ).  */
      if (scspecs)
	error_at (expr_loc, "storage class specifier in %<sizeof%>");
      if (type_name->specs->alignas_p)
	error_at (type_name->specs->locations[cdw_alignas],
		  "alignment specified for type name in %<sizeof%>");
      c_inhibit_evaluation_warnings--;
      in_sizeof--;
      result = c_expr_sizeof_type (expr_loc, type_name);
    }
  else
    {
      expr_loc = c_parser_peek_token (parser)->location;
      expr = c_parser_unary_expression (parser);
      finish = expr.get_finish ();
    sizeof_expr:
      c_inhibit_evaluation_warnings--;
      in_sizeof--;
      mark_exp_read (expr.value);
      if (TREE_CODE (expr.value) == COMPONENT_REF
	  && DECL_C_BIT_FIELD (TREE_OPERAND (expr.value, 1)))
	error_at (expr_loc, "%<sizeof%> applied to a bit-field");
      result = c_expr_sizeof_expr (expr_loc, expr);
    }
  if (finish == UNKNOWN_LOCATION)
    finish = start;
  set_c_expr_source_range (&result, start, finish);
  return result;
}

/* Parse an alignof expression.  */

static struct c_expr
c_parser_alignof_expression (c_parser *parser)
{
  struct c_expr expr;
  location_t start_loc = c_parser_peek_token (parser)->location;
  location_t end_loc;
  tree alignof_spelling = c_parser_peek_token (parser)->value;
  gcc_assert (c_parser_next_token_is_keyword (parser, RID_ALIGNOF));
  bool is_c11_alignof = (strcmp (IDENTIFIER_POINTER (alignof_spelling),
				"_Alignof") == 0
			 || strcmp (IDENTIFIER_POINTER (alignof_spelling),
				    "alignof") == 0);
  /* A diagnostic is not required for the use of this identifier in
     the implementation namespace; only diagnose it for the C11 or C2X
     spelling because of existing code using the other spellings.  */
  if (is_c11_alignof)
    {
      if (flag_isoc99)
	pedwarn_c99 (start_loc, OPT_Wpedantic, "ISO C99 does not support %qE",
		     alignof_spelling);
      else
	pedwarn_c99 (start_loc, OPT_Wpedantic, "ISO C90 does not support %qE",
		     alignof_spelling);
    }
  c_parser_consume_token (parser);
  c_inhibit_evaluation_warnings++;
  in_alignof++;
  if (c_parser_next_token_is (parser, CPP_OPEN_PAREN)
      && c_token_starts_compound_literal (c_parser_peek_2nd_token (parser)))
    {
      /* Either __alignof__ ( type-name ) or __alignof__
	 unary-expression starting with a compound literal.  */
      location_t loc;
      struct c_declspecs *scspecs;
      struct c_type_name *type_name;
      struct c_expr ret;
      matching_parens parens;
      parens.consume_open (parser);
      loc = c_parser_peek_token (parser)->location;
      scspecs = c_parser_compound_literal_scspecs (parser);
      type_name = c_parser_type_name (parser, true);
      end_loc = c_parser_peek_token (parser)->location;
      parens.skip_until_found_close (parser);
      if (type_name == NULL)
	{
	  struct c_expr ret;
	  c_inhibit_evaluation_warnings--;
	  in_alignof--;
	  ret.set_error ();
	  ret.original_code = ERROR_MARK;
	  ret.original_type = NULL;
	  return ret;
	}
      if (c_parser_next_token_is (parser, CPP_OPEN_BRACE))
	{
	  expr = c_parser_postfix_expression_after_paren_type (parser, scspecs,
							       type_name,
							       loc);
	  goto alignof_expr;
	}
      /* alignof ( type-name ).  */
      if (scspecs)
	error_at (loc, "storage class specifier in %qE", alignof_spelling);
      if (type_name->specs->alignas_p)
	error_at (type_name->specs->locations[cdw_alignas],
		  "alignment specified for type name in %qE",
		  alignof_spelling);
      c_inhibit_evaluation_warnings--;
      in_alignof--;
      ret.value = c_sizeof_or_alignof_type (loc, groktypename (type_name,
							       NULL, NULL),
					    false, is_c11_alignof, 1);
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      set_c_expr_source_range (&ret, start_loc, end_loc);
      ret.m_decimal = 0;
      return ret;
    }
  else
    {
      struct c_expr ret;
      expr = c_parser_unary_expression (parser);
      end_loc = expr.src_range.m_finish;
    alignof_expr:
      mark_exp_read (expr.value);
      c_inhibit_evaluation_warnings--;
      in_alignof--;
      if (is_c11_alignof)
	pedwarn (start_loc,
		 OPT_Wpedantic, "ISO C does not allow %<%E (expression)%>",
		 alignof_spelling);
      ret.value = c_alignof_expr (start_loc, expr.value);
      ret.original_code = ERROR_MARK;
      ret.original_type = NULL;
      set_c_expr_source_range (&ret, start_loc, end_loc);
      ret.m_decimal = 0;
      return ret;
    }
}

/* Parse the __builtin_has_attribute ([expr|type], attribute-spec)
   expression.  */

static struct c_expr
c_parser_has_attribute_expression (c_parser *parser)
{
  gcc_assert (c_parser_next_token_is_keyword (parser,
					      RID_BUILTIN_HAS_ATTRIBUTE));
  location_t start = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);

  c_inhibit_evaluation_warnings++;

  matching_parens parens;
  if (!parens.require_open (parser))
    {
      c_inhibit_evaluation_warnings--;
      in_typeof--;

      struct c_expr result;
      result.set_error ();
      result.original_code = ERROR_MARK;
      result.original_type = NULL;
      return result;
    }

  /* Treat the type argument the same way as in typeof for the purposes
     of warnings.  FIXME: Generalize this so the warning refers to
     __builtin_has_attribute rather than typeof.  */
  in_typeof++;

  /* The first operand: one of DECL, EXPR, or TYPE.  */
  tree oper = NULL_TREE;
  if (c_parser_next_tokens_start_typename (parser, cla_prefer_id))
    {
      struct c_type_name *tname = c_parser_type_name (parser);
      in_typeof--;
      if (tname)
	{
	  oper = groktypename (tname, NULL, NULL);
	  pop_maybe_used (c_type_variably_modified_p (oper));
	}
    }
  else
    {
      struct c_expr cexpr = c_parser_expr_no_commas (parser, NULL);
      c_inhibit_evaluation_warnings--;
      in_typeof--;
      if (cexpr.value != error_mark_node)
	{
	  mark_exp_read (cexpr.value);
	  oper = cexpr.value;
	  tree etype = TREE_TYPE (oper);
	  bool was_vm = c_type_variably_modified_p (etype);
	  /* This is returned with the type so that when the type is
	     evaluated, this can be evaluated.  */
	  if (was_vm)
	    oper = c_fully_fold (oper, false, NULL);
	  pop_maybe_used (was_vm);
	}
    }

  struct c_expr result;
  result.original_code = ERROR_MARK;
  result.original_type = NULL;

  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
    {
      /* Consume the closing parenthesis if that's the next token
	 in the likely case the built-in was invoked with fewer
	 than two arguments.  */
      if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	c_parser_consume_token (parser);
      c_inhibit_evaluation_warnings--;
      result.set_error ();
      return result;
    }

  bool save_translate_strings_p = parser->translate_strings_p;

  location_t atloc = c_parser_peek_token (parser)->location;
  /* Parse a single attribute.  Require no leading comma and do not
     allow empty attributes.  */
  tree attr = c_parser_gnu_attribute (parser, NULL_TREE, false, false);

  parser->translate_strings_p = save_translate_strings_p;

  location_t finish = c_parser_peek_token (parser)->location;
  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
    c_parser_consume_token (parser);
  else
    {
      c_parser_error (parser, "expected identifier");
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);

      result.set_error ();
      return result;
    }

  if (!attr)
    {
      error_at (atloc, "expected identifier");
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				 "expected %<)%>");
      result.set_error ();
      return result;
    }

  result.original_code = INTEGER_CST;
  result.original_type = boolean_type_node;

  if (has_attribute (atloc, oper, attr, default_conversion))
    result.value = boolean_true_node;
  else
    result.value =  boolean_false_node;

  set_c_expr_source_range (&result, start, finish);
  result.m_decimal = 0;
  return result;
}

/* Helper function to read arguments of builtins which are interfaces
   for the middle-end nodes like COMPLEX_EXPR, VEC_PERM_EXPR and
   others.  The name of the builtin is passed using BNAME parameter.
   Function returns true if there were no errors while parsing and
   stores the arguments in CEXPR_LIST.  If it returns true,
   *OUT_CLOSE_PAREN_LOC is written to with the location of the closing
   parenthesis.  */
static bool
c_parser_get_builtin_args (c_parser *parser, const char *bname,
			   vec<c_expr_t, va_gc> **ret_cexpr_list,
			   bool choose_expr_p,
			   location_t *out_close_paren_loc)
{
  location_t loc = c_parser_peek_token (parser)->location;
  vec<c_expr_t, va_gc> *cexpr_list;
  c_expr_t expr;
  bool saved_force_folding_builtin_constant_p;

  *ret_cexpr_list = NULL;
  if (c_parser_next_token_is_not (parser, CPP_OPEN_PAREN))
    {
      error_at (loc, "cannot take address of %qs", bname);
      return false;
    }

  c_parser_consume_token (parser);

  if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
    {
      *out_close_paren_loc = c_parser_peek_token (parser)->location;
      c_parser_consume_token (parser);
      return true;
    }

  saved_force_folding_builtin_constant_p
    = force_folding_builtin_constant_p;
  force_folding_builtin_constant_p |= choose_expr_p;
  expr = c_parser_expr_no_commas (parser, NULL);
  force_folding_builtin_constant_p
    = saved_force_folding_builtin_constant_p;
  vec_alloc (cexpr_list, 1);
  vec_safe_push (cexpr_list, expr);
  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);
      expr = c_parser_expr_no_commas (parser, NULL);
      vec_safe_push (cexpr_list, expr);
    }

  *out_close_paren_loc = c_parser_peek_token (parser)->location;
  if (!c_parser_require (parser, CPP_CLOSE_PAREN, "expected %<)%>"))
    return false;

  *ret_cexpr_list = cexpr_list;
  return true;
}

/* This represents a single generic-association.  */

struct c_generic_association
{
  /* The location of the starting token of the type.  */
  location_t type_location;
  /* The association's type, or NULL_TREE for 'default'.  */
  tree type;
  /* The association's expression.  */
  struct c_expr expression;
};

/* Parse a generic-selection.  (C11 6.5.1.1).
   
   generic-selection:
     _Generic ( assignment-expression , generic-assoc-list )
     
   generic-assoc-list:
     generic-association
     generic-assoc-list , generic-association
   
   generic-association:
     type-name : assignment-expression
     default : assignment-expression
*/

static struct c_expr
c_parser_generic_selection (c_parser *parser)
{
  struct c_expr selector, error_expr;
  tree selector_type;
  struct c_generic_association matched_assoc;
  int match_found = -1;
  location_t generic_loc, selector_loc;

  error_expr.original_code = ERROR_MARK;
  error_expr.original_type = NULL;
  error_expr.set_error ();
  matched_assoc.type_location = UNKNOWN_LOCATION;
  matched_assoc.type = NULL_TREE;
  matched_assoc.expression = error_expr;

  gcc_assert (c_parser_next_token_is_keyword (parser, RID_GENERIC));
  generic_loc = c_parser_peek_token (parser)->location;
  c_parser_consume_token (parser);
  if (flag_isoc99)
    pedwarn_c99 (generic_loc, OPT_Wpedantic,
		 "ISO C99 does not support %<_Generic%>");
  else
    pedwarn_c99 (generic_loc, OPT_Wpedantic,
		 "ISO C90 does not support %<_Generic%>");

  matching_parens parens;
  if (!parens.require_open (parser))
    return error_expr;

  c_inhibit_evaluation_warnings++;
  selector_loc = c_parser_peek_token (parser)->location;
  selector = c_parser_expr_no_commas (parser, NULL);
  selector = default_function_array_conversion (selector_loc, selector);
  c_inhibit_evaluation_warnings--;

  if (selector.value == error_mark_node)
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return selector;
    }
  mark_exp_read (selector.value);
  selector_type = TREE_TYPE (selector.value);
  /* In ISO C terms, rvalues (including the controlling expression of
     _Generic) do not have qualified types.  */
  if (TREE_CODE (selector_type) != ARRAY_TYPE)
    selector_type = TYPE_MAIN_VARIANT (selector_type);
  /* In ISO C terms, _Noreturn is not part of the type of expressions
     such as &abort, but in GCC it is represented internally as a type
     qualifier.  */
  if (FUNCTION_POINTER_TYPE_P (selector_type)
      && TYPE_QUALS (TREE_TYPE (selector_type)) != TYPE_UNQUALIFIED)
    selector_type
      = build_pointer_type (TYPE_MAIN_VARIANT (TREE_TYPE (selector_type)));

  if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return error_expr;
    }

  auto_vec<c_generic_association> associations;
  while (1)
    {
      struct c_generic_association assoc, *iter;
      unsigned int ix;
      c_token *token = c_parser_peek_token (parser);

      assoc.type_location = token->location;
      if (token->type == CPP_KEYWORD && token->keyword == RID_DEFAULT)
	{
	  c_parser_consume_token (parser);
	  assoc.type = NULL_TREE;
	}
      else
	{
	  struct c_type_name *type_name;

	  type_name = c_parser_type_name (parser);
	  if (type_name == NULL)
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      return error_expr;
	    }
	  assoc.type = groktypename (type_name, NULL, NULL);
	  if (assoc.type == error_mark_node)
	    {
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      return error_expr;
	    }

	  if (TREE_CODE (assoc.type) == FUNCTION_TYPE)
	    error_at (assoc.type_location,
		      "%<_Generic%> association has function type");
	  else if (!COMPLETE_TYPE_P (assoc.type))
	    error_at (assoc.type_location,
		      "%<_Generic%> association has incomplete type");

	  if (c_type_variably_modified_p (assoc.type))
	    error_at (assoc.type_location,
		      "%<_Generic%> association has "
		      "variable length type");
	}

      if (!c_parser_require (parser, CPP_COLON, "expected %<:%>"))
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return error_expr;
	}

      assoc.expression = c_parser_expr_no_commas (parser, NULL);
      if (assoc.expression.value == error_mark_node)
	{
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	  return error_expr;
	}

      for (ix = 0; associations.iterate (ix, &iter); ++ix)
	{
	  if (assoc.type == NULL_TREE)
	    {
	      if (iter->type == NULL_TREE)
		{
		  error_at (assoc.type_location,
			    "duplicate %<default%> case in %<_Generic%>");
		  inform (iter->type_location, "original %<default%> is here");
		}
	    }
	  else if (iter->type != NULL_TREE)
	    {
	      if (comptypes (assoc.type, iter->type))
		{
		  error_at (assoc.type_location,
			    "%<_Generic%> specifies two compatible types");
		  inform (iter->type_location, "compatible type is here");
		}
	    }
	}

      if (assoc.type == NULL_TREE)
	{
	  if (match_found < 0)
	    {
	      matched_assoc = assoc;
	      match_found = associations.length ();
	    }
	}
      else if (comptypes (assoc.type, selector_type))
	{
	  if (match_found < 0 || matched_assoc.type == NULL_TREE)
	    {
	      matched_assoc = assoc;
	      match_found = associations.length ();
	    }
	  else
	    {
	      error_at (assoc.type_location,
			"%<_Generic%> selector matches multiple associations");
	      inform (matched_assoc.type_location,
		      "other match is here");
	    }
	}

      associations.safe_push (assoc);

      if (c_parser_peek_token (parser)->type != CPP_COMMA)
	break;
      c_parser_consume_token (parser);
    }

  unsigned int ix;
  struct c_generic_association *iter;
  FOR_EACH_VEC_ELT (associations, ix, iter)
    if (ix != (unsigned) match_found)
      mark_exp_read (iter->expression.value);

  if (!parens.require_close (parser))
    {
      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
      return error_expr;
    }

  if (match_found < 0)
    {
      error_at (selector_loc, "%<_Generic%> selector of type %qT is not "
		"compatible with any association",
		selector_type);
      return error_expr;
    }

  return matched_assoc.expression;
}

/* Check the validity of a function pointer argument *EXPR (argument
   position POS) to __builtin_tgmath.  Return the number of function
   arguments if possibly valid; return 0 having reported an error if
   not valid.  */

static unsigned int
check_tgmath_function (c_expr *expr, unsigned int pos)
{
  tree type = TREE_TYPE (expr->value);
  if (!FUNCTION_POINTER_TYPE_P (type))
    {
      error_at (expr->get_location (),
		"argument %u of %<__builtin_tgmath%> is not a function pointer",
		pos);
      return 0;
    }
  type = TREE_TYPE (type);
  if (!prototype_p (type))
    {
      error_at (expr->get_location (),
		"argument %u of %<__builtin_tgmath%> is unprototyped", pos);
      return 0;
    }
  if (stdarg_p (type))
    {
      error_at (expr->get_location (),
		"argument %u of %<__builtin_tgmath%> has variable arguments",
		pos);
      return 0;
    }
  unsigned int nargs = 0;
  function_args_iterator iter;
  tree t;
  FOREACH_FUNCTION_ARGS (type, t, iter)
    {
      if (t == void_type_node)
	break;
      nargs++;
    }
  if (nargs == 0)
    {
      error_at (expr->get_location (),
		"argument %u of %<__builtin_tgmath%> has no arguments", pos);
      return 0;
    }
  return nargs;
}

/* Ways in which a parameter or return value of a type-generic macro
   may vary between the different functions the macro may call.  */
enum tgmath_parm_kind
  {
    tgmath_fixed, tgmath_real, tgmath_complex
  };

/* Helper function for c_parser_postfix_expression.  Parse predefined
   identifiers.  */

static struct c_expr
c_parser_predefined_identifier (c_parser *parser)
{
  location_t loc = c_parser_peek_token (parser)->location;
  switch (c_parser_peek_token (parser)->keyword)
    {
    case RID_FUNCTION_NAME:
      pedwarn (loc, OPT_Wpedantic, "ISO C does not support %qs predefined "
	       "identifier", "__FUNCTION__");
      break;
    case RID_PRETTY_FUNCTION_NAME:
      pedwarn (loc, OPT_Wpedantic, "ISO C does not support %qs predefined "
	       "identifier", "__PRETTY_FUNCTION__");
      break;
    case RID_C99_FUNCTION_NAME:
      pedwarn_c90 (loc, OPT_Wpedantic, "ISO C90 does not support "
		   "%<__func__%> predefined identifier");
      break;
    default:
      gcc_unreachable ();
    }

  struct c_expr expr;
  expr.original_code = ERROR_MARK;
  expr.original_type = NULL;
  expr.value = fname_decl (loc, c_parser_peek_token (parser)->keyword,
			   c_parser_peek_token (parser)->value);
  set_c_expr_source_range (&expr, loc, loc);
  expr.m_decimal = 0;
  c_parser_consume_token (parser);
  return expr;
}

/* Parse a postfix expression (C90 6.3.1-6.3.2, C99 6.5.1-6.5.2,
   C11 6.5.1-6.5.2).  Compound literals aren't handled here; callers have to
   call c_parser_postfix_expression_after_paren_type on encountering them.

   postfix-expression:
     primary-expression
     postfix-expression [ expression ]
     postfix-expression ( argument-expression-list[opt] )
     postfix-expression . identifier
     postfix-expression -> identifier
     postfix-expression ++
     postfix-expression --
     ( storage-class-specifiers[opt] type-name ) { initializer-list[opt] }
     ( storage-class-specifiers[opt] type-name ) { initializer-list , }

   argument-expression-list:
     argument-expression
     argument-expression-list , argument-expression

   primary-expression:
     identifier
     constant
     string-literal
     ( expression )
     generic-selection

   GNU extensions:

   primary-expression:
     __func__
       (treated as a keyword in GNU C)
     __FUNCTION__
     __PRETTY_FUNCTION__
     ( compound-statement )
     __builtin_va_arg ( assignment-expression , type-name )
     __builtin_offsetof ( type-name , offsetof-member-designator )
     __builtin_choose_expr ( assignment-expression ,
			     assignment-expression ,
			     assignment-expression )
     __builtin_types_compatible_p ( type-name , type-name )
     __builtin_tgmath ( expr-list )
     __builtin_complex ( assignment-expression , assignment-expression )
     __builtin_shuffle ( assignment-expression , assignment-expression )
     __builtin_shuffle ( assignment-expression ,
			 assignment-expression ,
			 assignment-expression, )
     __builtin_convertvector ( assignment-expression , type-name )
     __builtin_assoc_barrier ( assignment-expression )

   offsetof-member-designator:
     identifier
     offsetof-member-designator . identifier
     offsetof-member-designator [ expression ]

   Objective-C:

   primary-expression:
     [ objc-receiver objc-message-args ]
     @selector ( objc-selector-arg )
     @protocol ( identifier )
     @encode ( type-name )
     objc-string-literal
     Classname . identifier
*/

static struct c_expr
c_parser_postfix_expression (c_parser *parser)
{
  struct c_expr expr, e1;
  struct c_type_name *t1, *t2;
  location_t loc = c_parser_peek_token (parser)->location;
  source_range tok_range = c_parser_peek_token (parser)->get_range ();
  expr.original_code = ERROR_MARK;
  expr.original_type = NULL;
  expr.m_decimal = 0;
  switch (c_parser_peek_token (parser)->type)
    {
    case CPP_NUMBER:
      expr.value = c_parser_peek_token (parser)->value;
      set_c_expr_source_range (&expr, tok_range);
      loc = c_parser_peek_token (parser)->location;
      expr.m_decimal = c_parser_peek_token (parser)->flags & DECIMAL_INT;
      c_parser_consume_token (parser);
      if (TREE_CODE (expr.value) == FIXED_CST
	  && !targetm.fixed_point_supported_p ())
	{
	  error_at (loc, "fixed-point types not supported for this target");
	  expr.set_error ();
	}
      break;
    case CPP_CHAR:
    case CPP_CHAR16:
    case CPP_CHAR32:
    case CPP_UTF8CHAR:
    case CPP_WCHAR:
      expr.value = c_parser_peek_token (parser)->value;
      /* For the purpose of warning when a pointer is compared with
	 a zero character constant.  */
      expr.original_type = char_type_node;
      set_c_expr_source_range (&expr, tok_range);
      c_parser_consume_token (parser);
      break;
    case CPP_STRING:
    case CPP_STRING16:
    case CPP_STRING32:
    case CPP_WSTRING:
    case CPP_UTF8STRING:
      expr = c_parser_string_literal (parser, parser->translate_strings_p,
				      true);
      break;
    case CPP_OBJC_STRING:
      gcc_assert (c_dialect_objc ());
      expr.value
	= objc_build_string_object (c_parser_peek_token (parser)->value);
      set_c_expr_source_range (&expr, tok_range);
      c_parser_consume_token (parser);
      break;
    case CPP_NAME:
      switch (c_parser_peek_token (parser)->id_kind)
	{
	case C_ID_ID:
	  {
	    tree id = c_parser_peek_token (parser)->value;
	    c_parser_consume_token (parser);
	    expr.value = build_external_ref (loc, id,
					     (c_parser_peek_token (parser)->type
					      == CPP_OPEN_PAREN),
					     &expr.original_type);
	    set_c_expr_source_range (&expr, tok_range);
	    break;
	  }
	case C_ID_CLASSNAME:
	  {
	    /* Here we parse the Objective-C 2.0 Class.name dot
	       syntax.  */
	    tree class_name = c_parser_peek_token (parser)->value;
	    tree component;
	    c_parser_consume_token (parser);
	    gcc_assert (c_dialect_objc ());
	    if (!c_parser_require (parser, CPP_DOT, "expected %<.%>"))
	      {
		expr.set_error ();
		break;
	      }
	    if (c_parser_next_token_is_not (parser, CPP_NAME))
	      {
		c_parser_error (parser, "expected identifier");
		expr.set_error ();
		break;
	      }
	    c_token *component_tok = c_parser_peek_token (parser);
	    component = component_tok->value;
	    location_t end_loc = component_tok->get_finish ();
	    c_parser_consume_token (parser);
	    expr.value = objc_build_class_component_ref (class_name, 
							 component);
	    set_c_expr_source_range (&expr, loc, end_loc);
	    break;
	  }
	default:
	  c_parser_error (parser, "expected expression");
	  expr.set_error ();
	  break;
	}
      break;
    case CPP_OPEN_PAREN:
      /* A parenthesized expression, statement expression or compound
	 literal.  */
      if (c_parser_peek_2nd_token (parser)->type == CPP_OPEN_BRACE)
	{
	  /* A statement expression.  */
	  tree stmt;
	  location_t brace_loc;
	  c_parser_consume_token (parser);
	  brace_loc = c_parser_peek_token (parser)->location;
	  c_parser_consume_token (parser);
	  /* If we've not yet started the current function's statement list,
	     or we're in the parameter scope of an old-style function
	     declaration, statement expressions are not allowed.  */
	  if (!building_stmt_list_p () || old_style_parameter_scope ())
	    {
	      error_at (loc, "braced-group within expression allowed "
			"only inside a function");
	      parser->error = true;
	      c_parser_skip_until_found (parser, CPP_CLOSE_BRACE, NULL);
	      c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
	      expr.set_error ();
	      break;
	    }
	  stmt = c_begin_stmt_expr ();
	  c_parser_compound_statement_nostart (parser);
	  location_t close_loc = c_parser_peek_token (parser)->location;
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>");
	  pedwarn (loc, OPT_Wpedantic,
		   "ISO C forbids braced-groups within expressions");
	  expr.value = c_finish_stmt_expr (brace_loc, stmt);
	  set_c_expr_source_range (&expr, loc, close_loc);
	  mark_exp_read (expr.value);
	}
      else
	{
	  /* A parenthesized expression.  */
	  location_t loc_open_paren = c_parser_peek_token (parser)->location;
	  c_parser_consume_token (parser);
	  expr = c_parser_expression (parser);
	  if (TREE_CODE (expr.value) == MODIFY_EXPR)
	    suppress_warning (expr.value, OPT_Wparentheses);
	  if (expr.original_code != C_MAYBE_CONST_EXPR
	      && expr.original_code != SIZEOF_EXPR)
	    expr.original_code = ERROR_MARK;
	  /* Remember that we saw ( ) around the sizeof.  */
	  if (expr.original_code == SIZEOF_EXPR)
	    expr.original_code = PAREN_SIZEOF_EXPR;
	  /* Don't change EXPR.ORIGINAL_TYPE.  */
	  location_t loc_close_paren = c_parser_peek_token (parser)->location;
	  set_c_expr_source_range (&expr, loc_open_paren, loc_close_paren);
	  c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				     "expected %<)%>", loc_open_paren);
	}
      break;
    case CPP_KEYWORD:
      switch (c_parser_peek_token (parser)->keyword)
	{
	case RID_FUNCTION_NAME:
	case RID_PRETTY_FUNCTION_NAME:
	case RID_C99_FUNCTION_NAME:
	  expr = c_parser_predefined_identifier (parser);
	  break;
	case RID_VA_ARG:
	  {
	    location_t start_loc = loc;
	    c_parser_consume_token (parser);
	    matching_parens parens;
	    if (!parens.require_open (parser))
	      {
		expr.set_error ();
		break;
	      }
	    e1 = c_parser_expr_no_commas (parser, NULL);
	    mark_exp_read (e1.value);
	    e1.value = c_fully_fold (e1.value, false, NULL);
	    if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
	      {
		c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
		expr.set_error ();
		break;
	      }
	    loc = c_parser_peek_token (parser)->location;
	    t1 = c_parser_type_name (parser);
	    location_t end_loc = c_parser_peek_token (parser)->get_finish ();
	    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				       "expected %<)%>");
	    if (t1 == NULL)
	      {
		expr.set_error ();
	      }
	    else
	      {
		tree type_expr = NULL_TREE;
		expr.value = c_build_va_arg (start_loc, e1.value, loc,
					     groktypename (t1, &type_expr, NULL));
		if (type_expr)
		  {
		    expr.value = build2 (C_MAYBE_CONST_EXPR,
					 TREE_TYPE (expr.value), type_expr,
					 expr.value);
		    C_MAYBE_CONST_EXPR_NON_CONST (expr.value) = true;
		  }
		set_c_expr_source_range (&expr, start_loc, end_loc);
	      }
	  }
	  break;
	case RID_OFFSETOF:
	  {
	    c_parser_consume_token (parser);
	    matching_parens parens;
	    if (!parens.require_open (parser))
	      {
		expr.set_error ();
		break;
	      }
	    t1 = c_parser_type_name (parser);
	    if (t1 == NULL)
	      parser->error = true;
	    if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
	      gcc_assert (parser->error);
	    if (parser->error)
	      {
		c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
		expr.set_error ();
		break;
	      }
	    tree type = groktypename (t1, NULL, NULL);
	    tree offsetof_ref;
	    if (type == error_mark_node)
	      offsetof_ref = error_mark_node;
	    else
	      {
		offsetof_ref = build1 (INDIRECT_REF, type, null_pointer_node);
		SET_EXPR_LOCATION (offsetof_ref, loc);
	      }
	    /* Parse the second argument to __builtin_offsetof.  We
	       must have one identifier, and beyond that we want to
	       accept sub structure and sub array references.  */
	    if (c_parser_next_token_is (parser, CPP_NAME))
	      {
		c_token *comp_tok = c_parser_peek_token (parser);
		offsetof_ref
		  = build_component_ref (loc, offsetof_ref, comp_tok->value,
					 comp_tok->location, UNKNOWN_LOCATION);
		c_parser_consume_token (parser);
		while (c_parser_next_token_is (parser, CPP_DOT)
		       || c_parser_next_token_is (parser,
						  CPP_OPEN_SQUARE)
		       || c_parser_next_token_is (parser,
						  CPP_DEREF))
		  {
		    if (c_parser_next_token_is (parser, CPP_DEREF))
		      {
			loc = c_parser_peek_token (parser)->location;
			offsetof_ref = build_array_ref (loc,
							offsetof_ref,
							integer_zero_node);
			goto do_dot;
		      }
		    else if (c_parser_next_token_is (parser, CPP_DOT))
		      {
		      do_dot:
			c_parser_consume_token (parser);
			if (c_parser_next_token_is_not (parser,
							CPP_NAME))
			  {
			    c_parser_error (parser, "expected identifier");
			    break;
			  }
			c_token *comp_tok = c_parser_peek_token (parser);
			offsetof_ref
			  = build_component_ref (loc, offsetof_ref,
						 comp_tok->value,
						 comp_tok->location,
						 UNKNOWN_LOCATION);
			c_parser_consume_token (parser);
		      }
		    else
		      {
			struct c_expr ce;
			tree idx;
			loc = c_parser_peek_token (parser)->location;
			c_parser_consume_token (parser);
			ce = c_parser_expression (parser);
			ce = convert_lvalue_to_rvalue (loc, ce, false, false);
			idx = ce.value;
			idx = c_fully_fold (idx, false, NULL);
			c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
						   "expected %<]%>");
			offsetof_ref = build_array_ref (loc, offsetof_ref, idx);
		      }
		  }
	      }
	    else
	      c_parser_error (parser, "expected identifier");
	    location_t end_loc = c_parser_peek_token (parser)->get_finish ();
	    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				       "expected %<)%>");
	    expr.value = fold_offsetof (offsetof_ref);
	    set_c_expr_source_range (&expr, loc, end_loc);
	  }
	  break;
	case RID_CHOOSE_EXPR:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    c_expr_t *e1_p, *e2_p, *e3_p;
	    tree c;
	    location_t close_paren_loc;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_choose_expr",
					    &cexpr_list, true,
					    &close_paren_loc))
	      {
		expr.set_error ();
		break;
	      }

	    if (vec_safe_length (cexpr_list) != 3)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_choose_expr%>");
		expr.set_error ();
		break;
	      }

	    e1_p = &(*cexpr_list)[0];
	    e2_p = &(*cexpr_list)[1];
	    e3_p = &(*cexpr_list)[2];

	    c = e1_p->value;
	    mark_exp_read (e2_p->value);
	    mark_exp_read (e3_p->value);
	    if (TREE_CODE (c) != INTEGER_CST
		|| !INTEGRAL_TYPE_P (TREE_TYPE (c)))
	      error_at (loc,
			"first argument to %<__builtin_choose_expr%> not"
			" a constant");
	    constant_expression_warning (c);
	    expr = integer_zerop (c) ? *e3_p : *e2_p;
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	    break;
	  }
	case RID_TYPES_COMPATIBLE_P:
	  {
	    c_parser_consume_token (parser);
	    matching_parens parens;
	    if (!parens.require_open (parser))
	      {
		expr.set_error ();
		break;
	      }
	    t1 = c_parser_type_name (parser);
	    if (t1 == NULL)
	      {
		expr.set_error ();
		break;
	      }
	    if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
	      {
		c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
		expr.set_error ();
		break;
	      }
	    t2 = c_parser_type_name (parser);
	    if (t2 == NULL)
	      {
		expr.set_error ();
		break;
	      }
	    location_t close_paren_loc = c_parser_peek_token (parser)->location;
	    parens.skip_until_found_close (parser);
	    tree e1, e2;
	    e1 = groktypename (t1, NULL, NULL);
	    e2 = groktypename (t2, NULL, NULL);
	    if (e1 == error_mark_node || e2 == error_mark_node)
	      {
		expr.set_error ();
		break;
	      }

	    e1 = TYPE_MAIN_VARIANT (e1);
	    e2 = TYPE_MAIN_VARIANT (e2);

	    expr.value
	      = comptypes (e1, e2) ? integer_one_node : integer_zero_node;
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	  }
	  break;
	case RID_BUILTIN_TGMATH:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    location_t close_paren_loc;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_tgmath",
					    &cexpr_list, false,
					    &close_paren_loc))
	      {
		expr.set_error ();
		break;
	      }

	    if (vec_safe_length (cexpr_list) < 3)
	      {
		error_at (loc, "too few arguments to %<__builtin_tgmath%>");
		expr.set_error ();
		break;
	      }

	    unsigned int i;
	    c_expr_t *p;
	    FOR_EACH_VEC_ELT (*cexpr_list, i, p)
	      *p = convert_lvalue_to_rvalue (loc, *p, true, true);
	    unsigned int nargs = check_tgmath_function (&(*cexpr_list)[0], 1);
	    if (nargs == 0)
	      {
		expr.set_error ();
		break;
	      }
	    if (vec_safe_length (cexpr_list) < nargs)
	      {
		error_at (loc, "too few arguments to %<__builtin_tgmath%>");
		expr.set_error ();
		break;
	      }
	    unsigned int num_functions = vec_safe_length (cexpr_list) - nargs;
	    if (num_functions < 2)
	      {
		error_at (loc, "too few arguments to %<__builtin_tgmath%>");
		expr.set_error ();
		break;
	      }

	    /* The first NUM_FUNCTIONS expressions are the function
	       pointers.  The remaining NARGS expressions are the
	       arguments that are to be passed to one of those
	       functions, chosen following <tgmath.h> rules.  */
	    for (unsigned int j = 1; j < num_functions; j++)
	      {
		unsigned int this_nargs
		  = check_tgmath_function (&(*cexpr_list)[j], j + 1);
		if (this_nargs == 0)
		  {
		    expr.set_error ();
		    goto out;
		  }
		if (this_nargs != nargs)
		  {
		    error_at ((*cexpr_list)[j].get_location (),
			      "argument %u of %<__builtin_tgmath%> has "
			      "wrong number of arguments", j + 1);
		    expr.set_error ();
		    goto out;
		  }
	      }

	    /* The functions all have the same number of arguments.
	       Determine whether arguments and return types vary in
	       ways permitted for <tgmath.h> functions.  */
	    /* The first entry in each of these vectors is for the
	       return type, subsequent entries for parameter
	       types.  */
	    auto_vec<enum tgmath_parm_kind> parm_kind (nargs + 1);
	    auto_vec<tree> parm_first (nargs + 1);
	    auto_vec<bool> parm_complex (nargs + 1);
	    auto_vec<bool> parm_varies (nargs + 1);
	    tree first_type = TREE_TYPE (TREE_TYPE ((*cexpr_list)[0].value));
	    tree first_ret = TYPE_MAIN_VARIANT (TREE_TYPE (first_type));
	    parm_first.quick_push (first_ret);
	    parm_complex.quick_push (TREE_CODE (first_ret) == COMPLEX_TYPE);
	    parm_varies.quick_push (false);
	    function_args_iterator iter;
	    tree t;
	    unsigned int argpos;
	    FOREACH_FUNCTION_ARGS (first_type, t, iter)
	      {
		if (t == void_type_node)
		  break;
		parm_first.quick_push (TYPE_MAIN_VARIANT (t));
		parm_complex.quick_push (TREE_CODE (t) == COMPLEX_TYPE);
		parm_varies.quick_push (false);
	      }
	    for (unsigned int j = 1; j < num_functions; j++)
	      {
		tree type = TREE_TYPE (TREE_TYPE ((*cexpr_list)[j].value));
		tree ret = TYPE_MAIN_VARIANT (TREE_TYPE (type));
		if (ret != parm_first[0])
		  {
		    parm_varies[0] = true;
		    if (!SCALAR_FLOAT_TYPE_P (parm_first[0])
			&& !COMPLEX_FLOAT_TYPE_P (parm_first[0]))
		      {
			error_at ((*cexpr_list)[0].get_location (),
				  "invalid type-generic return type for "
				  "argument %u of %<__builtin_tgmath%>",
				  1);
			expr.set_error ();
			goto out;
		      }
		    if (!SCALAR_FLOAT_TYPE_P (ret)
			&& !COMPLEX_FLOAT_TYPE_P (ret))
		      {
			error_at ((*cexpr_list)[j].get_location (),
				  "invalid type-generic return type for "
				  "argument %u of %<__builtin_tgmath%>",
				  j + 1);
			expr.set_error ();
			goto out;
		      }
		  }
		if (TREE_CODE (ret) == COMPLEX_TYPE)
		  parm_complex[0] = true;
		argpos = 1;
		FOREACH_FUNCTION_ARGS (type, t, iter)
		  {
		    if (t == void_type_node)
		      break;
		    t = TYPE_MAIN_VARIANT (t);
		    if (t != parm_first[argpos])
		      {
			parm_varies[argpos] = true;
			if (!SCALAR_FLOAT_TYPE_P (parm_first[argpos])
			    && !COMPLEX_FLOAT_TYPE_P (parm_first[argpos]))
			  {
			    error_at ((*cexpr_list)[0].get_location (),
				      "invalid type-generic type for "
				      "argument %u of argument %u of "
				      "%<__builtin_tgmath%>", argpos, 1);
			    expr.set_error ();
			    goto out;
			  }
			if (!SCALAR_FLOAT_TYPE_P (t)
			    && !COMPLEX_FLOAT_TYPE_P (t))
			  {
			    error_at ((*cexpr_list)[j].get_location (),
				      "invalid type-generic type for "
				      "argument %u of argument %u of "
				      "%<__builtin_tgmath%>", argpos, j + 1);
			    expr.set_error ();
			    goto out;
			  }
		      }
		    if (TREE_CODE (t) == COMPLEX_TYPE)
		      parm_complex[argpos] = true;
		    argpos++;
		  }
	      }
	    enum tgmath_parm_kind max_variation = tgmath_fixed;
	    for (unsigned int j = 0; j <= nargs; j++)
	      {
		enum tgmath_parm_kind this_kind;
		if (parm_varies[j])
		  {
		    if (parm_complex[j])
		      max_variation = this_kind = tgmath_complex;
		    else
		      {
			this_kind = tgmath_real;
			if (max_variation != tgmath_complex)
			  max_variation = tgmath_real;
		      }
		  }
		else
		  this_kind = tgmath_fixed;
		parm_kind.quick_push (this_kind);
	      }
	    if (max_variation == tgmath_fixed)
	      {
		error_at (loc, "function arguments of %<__builtin_tgmath%> "
			  "all have the same type");
		expr.set_error ();
		break;
	      }

	    /* Identify a parameter (not the return type) that varies,
	       including with complex types if any variation includes
	       complex types; there must be at least one such
	       parameter.  */
	    unsigned int tgarg = 0;
	    for (unsigned int j = 1; j <= nargs; j++)
	      if (parm_kind[j] == max_variation)
		{
		  tgarg = j;
		  break;
		}
	    if (tgarg == 0)
	      {
		error_at (loc, "function arguments of %<__builtin_tgmath%> "
			  "lack type-generic parameter");
		expr.set_error ();
		break;
	      }

	    /* Determine the type of the relevant parameter for each
	       function.  */
	    auto_vec<tree> tg_type (num_functions);
	    for (unsigned int j = 0; j < num_functions; j++)
	      {
		tree type = TREE_TYPE (TREE_TYPE ((*cexpr_list)[j].value));
		argpos = 1;
		FOREACH_FUNCTION_ARGS (type, t, iter)
		  {
		    if (argpos == tgarg)
		      {
			tg_type.quick_push (TYPE_MAIN_VARIANT (t));
			break;
		      }
		    argpos++;
		  }
	      }

	    /* Verify that the corresponding types are different for
	       all the listed functions.  Also determine whether all
	       the types are complex, whether all the types are
	       standard or binary, and whether all the types are
	       decimal.  */
	    bool all_complex = true;
	    bool all_binary = true;
	    bool all_decimal = true;
	    hash_set<tree> tg_types;
	    FOR_EACH_VEC_ELT (tg_type, i, t)
	      {
		if (TREE_CODE (t) == COMPLEX_TYPE)
		  all_decimal = false;
		else
		  {
		    all_complex = false;
		    if (DECIMAL_FLOAT_TYPE_P (t))
		      all_binary = false;
		    else
		      all_decimal = false;
		  }
		if (tg_types.add (t))
		  {
		    error_at ((*cexpr_list)[i].get_location (),
			      "duplicate type-generic parameter type for "
			      "function argument %u of %<__builtin_tgmath%>",
			      i + 1);
		    expr.set_error ();
		    goto out;
		  }
	      }

	    /* Verify that other parameters and the return type whose
	       types vary have their types varying in the correct
	       way.  */
	    for (unsigned int j = 0; j < num_functions; j++)
	      {
		tree exp_type = tg_type[j];
		tree exp_real_type = exp_type;
		if (TREE_CODE (exp_type) == COMPLEX_TYPE)
		  exp_real_type = TREE_TYPE (exp_type);
		tree type = TREE_TYPE (TREE_TYPE ((*cexpr_list)[j].value));
		tree ret = TYPE_MAIN_VARIANT (TREE_TYPE (type));
		if ((parm_kind[0] == tgmath_complex && ret != exp_type)
		    || (parm_kind[0] == tgmath_real && ret != exp_real_type))
		  {
		    error_at ((*cexpr_list)[j].get_location (),
			      "bad return type for function argument %u "
			      "of %<__builtin_tgmath%>", j + 1);
		    expr.set_error ();
		    goto out;
		  }
		argpos = 1;
		FOREACH_FUNCTION_ARGS (type, t, iter)
		  {
		    if (t == void_type_node)
		      break;
		    t = TYPE_MAIN_VARIANT (t);
		    if ((parm_kind[argpos] == tgmath_complex
			 && t != exp_type)
			|| (parm_kind[argpos] == tgmath_real
			    && t != exp_real_type))
		      {
			error_at ((*cexpr_list)[j].get_location (),
				  "bad type for argument %u of "
				  "function argument %u of "
				  "%<__builtin_tgmath%>", argpos, j + 1);
			expr.set_error ();
			goto out;
		      }
		    argpos++;
		  }
	      }

	    /* The functions listed are a valid set of functions for a
	       <tgmath.h> macro to select between.  Identify the
	       matching function, if any.  First, the argument types
	       must be combined following <tgmath.h> rules.  Integer
	       types are treated as _Decimal64 if any type-generic
	       argument is decimal, or if the only alternatives for
	       type-generic arguments are of decimal types, and are
	       otherwise treated as _Float32x (or _Complex _Float32x
	       for complex integer types) if any type-generic argument
	       has _FloatNx type, otherwise as double (or _Complex
	       double for complex integer types).  After that
	       adjustment, types are combined following the usual
	       arithmetic conversions.  If the function only accepts
	       complex arguments, a complex type is produced.  */
	    bool arg_complex = all_complex;
	    bool arg_binary = all_binary;
	    bool arg_int_decimal = all_decimal;
	    bool arg_int_floatnx = false;
	    for (unsigned int j = 1; j <= nargs; j++)
	      {
		if (parm_kind[j] == tgmath_fixed)
		  continue;
		c_expr_t *ce = &(*cexpr_list)[num_functions + j - 1];
		tree type = TREE_TYPE (ce->value);
		if (!INTEGRAL_TYPE_P (type)
		    && !SCALAR_FLOAT_TYPE_P (type)
		    && TREE_CODE (type) != COMPLEX_TYPE)
		  {
		    error_at (ce->get_location (),
			      "invalid type of argument %u of type-generic "
			      "function", j);
		    expr.set_error ();
		    goto out;
		  }
		if (DECIMAL_FLOAT_TYPE_P (type))
		  {
		    arg_int_decimal = true;
		    if (all_complex)
		      {
			error_at (ce->get_location (),
				  "decimal floating-point argument %u to "
				  "complex-only type-generic function", j);
			expr.set_error ();
			goto out;
		      }
		    else if (all_binary)
		      {
			error_at (ce->get_location (),
				  "decimal floating-point argument %u to "
				  "binary-only type-generic function", j);
			expr.set_error ();
			goto out;
		      }
		    else if (arg_complex)
		      {
			error_at (ce->get_location (),
				  "both complex and decimal floating-point "
				  "arguments to type-generic function");
			expr.set_error ();
			goto out;
		      }
		    else if (arg_binary)
		      {
			error_at (ce->get_location (),
				  "both binary and decimal floating-point "
				  "arguments to type-generic function");
			expr.set_error ();
			goto out;
		      }
		  }
		else if (TREE_CODE (type) == COMPLEX_TYPE)
		  {
		    arg_complex = true;
		    if (COMPLEX_FLOAT_TYPE_P (type))
		      arg_binary = true;
		    if (all_decimal)
		      {
			error_at (ce->get_location (),
				  "complex argument %u to "
				  "decimal-only type-generic function", j);
			expr.set_error ();
			goto out;
		      }
		    else if (arg_int_decimal)
		      {
			error_at (ce->get_location (),
				  "both complex and decimal floating-point "
				  "arguments to type-generic function");
			expr.set_error ();
			goto out;
		      }
		  }
		else if (SCALAR_FLOAT_TYPE_P (type))
		  {
		    arg_binary = true;
		    if (all_decimal)
		      {
			error_at (ce->get_location (),
				  "binary argument %u to "
				  "decimal-only type-generic function", j);
			expr.set_error ();
			goto out;
		      }
		    else if (arg_int_decimal)
		      {
			error_at (ce->get_location (),
				  "both binary and decimal floating-point "
				  "arguments to type-generic function");
			expr.set_error ();
			goto out;
		      }
		  }
		tree rtype = TYPE_MAIN_VARIANT (type);
		if (TREE_CODE (rtype) == COMPLEX_TYPE)
		  rtype = TREE_TYPE (rtype);
		if (SCALAR_FLOAT_TYPE_P (rtype))
		  for (unsigned int j = 0; j < NUM_FLOATNX_TYPES; j++)
		    if (rtype == FLOATNX_TYPE_NODE (j))
		      {
			arg_int_floatnx = true;
			break;
		      }
	      }
	    tree arg_real = NULL_TREE;
	    for (unsigned int j = 1; j <= nargs; j++)
	      {
		if (parm_kind[j] == tgmath_fixed)
		  continue;
		c_expr_t *ce = &(*cexpr_list)[num_functions + j - 1];
		tree type = TYPE_MAIN_VARIANT (TREE_TYPE (ce->value));
		if (TREE_CODE (type) == COMPLEX_TYPE)
		  type = TREE_TYPE (type);
		if (INTEGRAL_TYPE_P (type))
		  type = (arg_int_decimal
			  ? dfloat64_type_node
			  : arg_int_floatnx
			  ? float32x_type_node
			  : double_type_node);
		if (arg_real == NULL_TREE)
		  arg_real = type;
		else
		  arg_real = common_type (arg_real, type);
		if (arg_real == error_mark_node)
		  {
		    expr.set_error ();
		    goto out;
		  }
	      }
	    tree arg_type = (arg_complex
			     ? build_complex_type (arg_real)
			     : arg_real);

	    /* Look for a function to call with type-generic parameter
	       type ARG_TYPE.  */
	    c_expr_t *fn = NULL;
	    for (unsigned int j = 0; j < num_functions; j++)
	      {
		if (tg_type[j] == arg_type)
		  {
		    fn = &(*cexpr_list)[j];
		    break;
		  }
	      }
	    if (fn == NULL
		&& parm_kind[0] == tgmath_fixed
		&& SCALAR_FLOAT_TYPE_P (parm_first[0]))
	      {
		/* Presume this is a macro that rounds its result to a
		   narrower type, and look for the first function with
		   at least the range and precision of the argument
		   type.  */
		for (unsigned int j = 0; j < num_functions; j++)
		  {
		    if (arg_complex
			!= (TREE_CODE (tg_type[j]) == COMPLEX_TYPE))
		      continue;
		    tree real_tg_type = (arg_complex
					 ? TREE_TYPE (tg_type[j])
					 : tg_type[j]);
		    if (DECIMAL_FLOAT_TYPE_P (arg_real)
			!= DECIMAL_FLOAT_TYPE_P (real_tg_type))
		      continue;
		    scalar_float_mode arg_mode
		      = SCALAR_FLOAT_TYPE_MODE (arg_real);
		    scalar_float_mode tg_mode
		      = SCALAR_FLOAT_TYPE_MODE (real_tg_type);
		    const real_format *arg_fmt = REAL_MODE_FORMAT (arg_mode);
		    const real_format *tg_fmt = REAL_MODE_FORMAT (tg_mode);
		    if (arg_fmt->b == tg_fmt->b
			&& arg_fmt->p <= tg_fmt->p
			&& arg_fmt->emax <= tg_fmt->emax
			&& (arg_fmt->emin - arg_fmt->p
			    >= tg_fmt->emin - tg_fmt->p))
		      {
			fn = &(*cexpr_list)[j];
			break;
		      }
		  }
	      }
	    if (fn == NULL)
	      {
		error_at (loc, "no matching function for type-generic call");
		expr.set_error ();
		break;
	      }

	    /* Construct a call to FN.  */
	    vec<tree, va_gc> *args;
	    vec_alloc (args, nargs);
	    vec<tree, va_gc> *origtypes;
	    vec_alloc (origtypes, nargs);
	    auto_vec<location_t> arg_loc (nargs);
	    for (unsigned int j = 0; j < nargs; j++)
	      {
		c_expr_t *ce = &(*cexpr_list)[num_functions + j];
		args->quick_push (ce->value);
		arg_loc.quick_push (ce->get_location ());
		origtypes->quick_push (ce->original_type);
	      }
	    expr.value = c_build_function_call_vec (loc, arg_loc, fn->value,
						    args, origtypes);
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	    break;
	  }
	case RID_BUILTIN_CALL_WITH_STATIC_CHAIN:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    c_expr_t *e2_p;
	    tree chain_value;
	    location_t close_paren_loc;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_call_with_static_chain",
					    &cexpr_list, false,
					    &close_paren_loc))
	      {
		expr.set_error ();
		break;
	      }
	    if (vec_safe_length (cexpr_list) != 2)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_call_with_static_chain%>");
		expr.set_error ();
		break;
	      }

	    expr = (*cexpr_list)[0];
	    e2_p = &(*cexpr_list)[1];
	    *e2_p = convert_lvalue_to_rvalue (loc, *e2_p, true, true);
	    chain_value = e2_p->value;
	    mark_exp_read (chain_value);

	    if (TREE_CODE (expr.value) != CALL_EXPR)
	      error_at (loc, "first argument to "
			"%<__builtin_call_with_static_chain%> "
			"must be a call expression");
	    else if (TREE_CODE (TREE_TYPE (chain_value)) != POINTER_TYPE)
	      error_at (loc, "second argument to "
			"%<__builtin_call_with_static_chain%> "
			"must be a pointer type");
	    else
	      CALL_EXPR_STATIC_CHAIN (expr.value) = chain_value;
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	    break;
	  }
	case RID_BUILTIN_COMPLEX:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    c_expr_t *e1_p, *e2_p;
	    location_t close_paren_loc;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_complex",
					    &cexpr_list, false,
					    &close_paren_loc))
	      {
		expr.set_error ();
		break;
	      }

	    if (vec_safe_length (cexpr_list) != 2)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_complex%>");
		expr.set_error ();
		break;
	      }

	    e1_p = &(*cexpr_list)[0];
	    e2_p = &(*cexpr_list)[1];

	    *e1_p = convert_lvalue_to_rvalue (loc, *e1_p, true, true);
	    if (TREE_CODE (e1_p->value) == EXCESS_PRECISION_EXPR)
	      e1_p->value = convert (TREE_TYPE (e1_p->value),
				     TREE_OPERAND (e1_p->value, 0));
	    *e2_p = convert_lvalue_to_rvalue (loc, *e2_p, true, true);
	    if (TREE_CODE (e2_p->value) == EXCESS_PRECISION_EXPR)
	      e2_p->value = convert (TREE_TYPE (e2_p->value),
				     TREE_OPERAND (e2_p->value, 0));
	    if (!SCALAR_FLOAT_TYPE_P (TREE_TYPE (e1_p->value))
		|| DECIMAL_FLOAT_TYPE_P (TREE_TYPE (e1_p->value))
		|| !SCALAR_FLOAT_TYPE_P (TREE_TYPE (e2_p->value))
		|| DECIMAL_FLOAT_TYPE_P (TREE_TYPE (e2_p->value)))
	      {
		error_at (loc, "%<__builtin_complex%> operand "
			  "not of real binary floating-point type");
		expr.set_error ();
		break;
	      }
	    if (TYPE_MAIN_VARIANT (TREE_TYPE (e1_p->value))
		!= TYPE_MAIN_VARIANT (TREE_TYPE (e2_p->value)))
	      {
		error_at (loc,
			  "%<__builtin_complex%> operands of different types");
		expr.set_error ();
		break;
	      }
	    pedwarn_c90 (loc, OPT_Wpedantic,
			 "ISO C90 does not support complex types");
	    expr.value = build2_loc (loc, COMPLEX_EXPR,
				     build_complex_type
				     (TYPE_MAIN_VARIANT
				      (TREE_TYPE (e1_p->value))),
				     e1_p->value, e2_p->value);
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	    break;
	  }
	case RID_BUILTIN_SHUFFLE:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    unsigned int i;
	    c_expr_t *p;
	    location_t close_paren_loc;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_shuffle",
					    &cexpr_list, false,
					    &close_paren_loc))
	      {
		expr.set_error ();
		break;
	      }

	    FOR_EACH_VEC_SAFE_ELT (cexpr_list, i, p)
	      *p = convert_lvalue_to_rvalue (loc, *p, true, true);

	    if (vec_safe_length (cexpr_list) == 2)
	      expr.value = c_build_vec_perm_expr (loc, (*cexpr_list)[0].value,
						  NULL_TREE,
						  (*cexpr_list)[1].value);

	    else if (vec_safe_length (cexpr_list) == 3)
	      expr.value = c_build_vec_perm_expr (loc, (*cexpr_list)[0].value,
						  (*cexpr_list)[1].value,
						  (*cexpr_list)[2].value);
	    else
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_shuffle%>");
		expr.set_error ();
	      }
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	    break;
	  }
	case RID_BUILTIN_SHUFFLEVECTOR:
	  {
	    vec<c_expr_t, va_gc> *cexpr_list;
	    unsigned int i;
	    c_expr_t *p;
	    location_t close_paren_loc;

	    c_parser_consume_token (parser);
	    if (!c_parser_get_builtin_args (parser,
					    "__builtin_shufflevector",
					    &cexpr_list, false,
					    &close_paren_loc))
	      {
		expr.set_error ();
		break;
	      }

	    FOR_EACH_VEC_SAFE_ELT (cexpr_list, i, p)
	      *p = convert_lvalue_to_rvalue (loc, *p, true, true);

	    if (vec_safe_length (cexpr_list) < 3)
	      {
		error_at (loc, "wrong number of arguments to "
			       "%<__builtin_shuffle%>");
		expr.set_error ();
	      }
	    else
	      {
		auto_vec<tree, 16> mask;
		for (i = 2; i < cexpr_list->length (); ++i)
		  mask.safe_push ((*cexpr_list)[i].value);
		expr.value = c_build_shufflevector (loc, (*cexpr_list)[0].value,
						    (*cexpr_list)[1].value,
						    mask);
	      }
	    set_c_expr_source_range (&expr, loc, close_paren_loc);
	    break;
	  }
	case RID_BUILTIN_CONVERTVECTOR:
	  {
	    location_t start_loc = loc;
	    c_parser_consume_token (parser);
	    matching_parens parens;
	    if (!parens.require_open (parser))
	      {
		expr.set_error ();
		break;
	      }
	    e1 = c_parser_expr_no_commas (parser, NULL);
	    mark_exp_read (e1.value);
	    if (!c_parser_require (parser, CPP_COMMA, "expected %<,%>"))
	      {
		c_parser_skip_until_found (parser, CPP_CLOSE_PAREN, NULL);
		expr.set_error ();
		break;
	      }
	    loc = c_parser_peek_token (parser)->location;
	    t1 = c_parser_type_name (parser);
	    location_t end_loc = c_parser_peek_token (parser)->get_finish ();
	    c_parser_skip_until_found (parser, CPP_CLOSE_PAREN,
				       "expected %<)%>");
	    if (t1 == NULL)
	      expr.set_error ();
	    else
	      {
		tree type_expr = NULL_TREE;
		expr.value = c_build_vec_convert (start_loc, e1.value, loc,
						  groktypename (t1, &type_expr,
								NULL));
		set_c_expr_source_range (&expr, start_loc, end_loc);
	      }
	  }
	  break;
	case RID_BUILTIN_ASSOC_BARRIER:
	  {
	    location_t start_loc = loc;
	    c_parser_consume_token (parser);
	    matching_parens parens;
	    if (!parens.require_open (parser))
	      {
		expr.set_error ();
		break;
	      }
	    e1 = c_parser_expr_no_commas (parser, NULL);
	    mark_exp_read (e1.value);
	    location_t end_loc = c_parser_peek_token (parser)->get_finish ();
	    parens.skip_until_found_close (parser);
	    expr = parser_build_unary_op (loc, PAREN_EXPR, e1);
	    set_c_expr_source_range (&expr, start_loc, end_loc);
	  }
	  break;
	case RID_GENERIC:
	  expr = c_parser_generic_selection (parser);
	  break;
	case RID_OMP_ALL_MEMORY:
	  gcc_assert (flag_openmp);
	  c_parser_consume_token (parser);
	  error_at (loc, "%<omp_all_memory%> may only be used in OpenMP "
			 "%<depend%> clause");
	  expr.set_error ();
	  break;
	/* C23 'nullptr' literal.  */
	case RID_NULLPTR:
	  c_parser_consume_token (parser);
	  expr.value = nullptr_node;
	  set_c_expr_source_range (&expr, tok_range);
	  pedwarn_c11 (loc, OPT_Wpedantic,
		       "ISO C does not support %qs before C2X", "nullptr");
	  break;
	case RID_TRUE:
	  c_parser_consume_token (parser);
	  expr.value = boolean_true_node;
	  set_c_expr_source_range (&expr, tok_range);
	  break;
	case RID_FALSE:
	  c_parser_consume_token (parser);
	  expr.value = boolean_false_node;
	  set_c_expr_source_range (&expr, tok_range);
	  break;
	default:
	  c_parser_error (parser, "expected expression");
	  expr.set_error ();
	  break;
	}
      break;
    case CPP_OPEN_SQUARE:
      /* Else fall through to report error.  */
      /* FALLTHRU */
    default:
      c_parser_error (parser, "expected expression");
      expr.set_error ();
      break;
    }
 out:
  return c_parser_postfix_expression_after_primary
    (parser, EXPR_LOC_OR_LOC (expr.value, loc), expr);
}

/* Parse a postfix expression after a parenthesized type name: the
   brace-enclosed initializer of a compound literal, possibly followed
   by some postfix operators.  This is separate because it is not
   possible to tell until after the type name whether a cast
   expression has a cast or a compound literal, or whether the operand
   of sizeof is a parenthesized type name or starts with a compound
   literal.  TYPE_LOC is the location where TYPE_NAME starts--the
   location of the first token after the parentheses around the type
   name.  */

static struct c_expr
c_parser_postfix_expression_after_paren_type (c_parser *parser,
					      struct c_declspecs *scspecs,
					      struct c_type_name *type_name,
					      location_t type_loc)
{
  tree type;
  struct c_expr init;
  bool non_const;
  struct c_expr expr;
  location_t start_loc;
  tree type_expr = NULL_TREE;
  bool type_expr_const = true;
  bool constexpr_p = scspecs ? scspecs->constexpr_p : false;
  unsigned int underspec_state = 0;
  check_compound_literal_type (type_loc, type_name);
  rich_location richloc (line_table, type_loc);
  start_loc = c_parser_peek_token (parser)->location;
  if (constexpr_p)
    {
      underspec_state = start_underspecified_init (start_loc, NULL_TREE);
      /* A constexpr compound literal is subject to the constraints on
	 underspecified declarations, which may not declare tags or
	 members or structures or unions; it is undefined behavior to
	 declare the members of an enumeration.  Where the structure,
	 union or enumeration type is declared within the compound
	 literal initializer, this is diagnosed elsewhere as a result
	 of the above call to start_underspecified_init.  Diagnose
	 here the case of declaring such a type in the type specifiers
	 of the compound literal.  */
      switch (type_name->specs->typespec_kind)
	{
	case ctsk_tagfirstref:
	case ctsk_tagfirstref_attrs:
	  error_at (type_loc, "%qT declared in %<constexpr%> compound literal",
		    type_name->specs->type);
	  break;

	case ctsk_tagdef:
	  error_at (type_loc, "%qT defined in %<constexpr%> compound literal",
		    type_name->specs->type);
	  break;

	default:
	  break;
	}
    }
  start_init (NULL_TREE, NULL,
	      (global_bindings_p ()
	       || (scspecs && scspecs->storage_class == csc_static)
	       || constexpr_p), constexpr_p, &richloc);
  type = groktypename (type_name, &type_expr, &type_expr_const);
  if (type != error_mark_node && C_TYPE_VARIABLE_SIZE (type))
    {
      error_at (type_loc, "compound literal has variable size");
      type = error_mark_node;
    }
  else if (TREE_CODE (type) == FUNCTION_TYPE)
    {
      error_at (type_loc, "compound literal has function type");
      type = error_mark_node;
    }
  if (constexpr_p && type != error_mark_node)
    {
      tree type_no_array = strip_array_types (type);
      /* The type of a constexpr object must not be variably modified
	 (which applies to all compound literals), volatile, atomic or
	 restrict qualified or have a member with such a qualifier.
	 const qualification is implicitly added.  */
      if (TYPE_QUALS (type_no_array)
	  & (TYPE_QUAL_VOLATILE | TYPE_QUAL_RESTRICT | TYPE_QUAL_ATOMIC))
	error_at (type_loc, "invalid qualifiers for %<constexpr%> object");
      else if (RECORD_OR_UNION_TYPE_P (type_no_array)
	       && C_TYPE_FIELDS_NON_CONSTEXPR (type_no_array))
	error_at (type_loc, "invalid qualifiers for field of "
		  "%<constexpr%> object");
      type = c_build_qualified_type (type,
				     (TYPE_QUALS (type_no_array)
				      | TYPE_QUAL_CONST));
    }
  init = c_parser_braced_init (parser, type, false, NULL, NULL_TREE);
  if (constexpr_p)
    finish_underspecified_init (NULL_TREE, underspec_state);
  finish_init ();
  maybe_warn_string_init (type_loc, type, init);

  if (type != error_mark_node
      && !ADDR_SPACE_GENERIC_P (TYPE_ADDR_SPACE (type))
      && current_function_decl)
    {
      error ("compound literal qualified by address-space qualifier");
      type = error_mark_node;
    }

  if (!pedwarn_c90 (start_loc, OPT_Wpedantic,
		    "ISO C90 forbids compound literals") && scspecs)
    pedwarn_c11 (start_loc, OPT_Wpedantic,
		 "ISO C forbids storage class specifiers in compound literals "
		 "before C2X");
  non_const = ((init.value && TREE_CODE (init.value) == CONSTRUCTOR)
	       ? CONSTRUCTOR_NON_CONST (init.value)
	       : init.original_code == C_MAYBE_CONST_EXPR);
  non_const |= !type_expr_const;
  unsigned int alignas_align = 0;
  if (type != error_mark_node
      && type_name->specs->align_log != -1)
    {
      alignas_align = 1U << type_name->specs->align_log;
      if (alignas_align < min_align_of_type (type))
	{
	  error_at (type_name->specs->locations[cdw_alignas],
		    "%<_Alignas%> specifiers cannot reduce "
		    "alignment of compound literal");
	  alignas_align = 0;
	}
    }
  expr.value = build_compound_literal (start_loc, type, init.value, non_const,
				       alignas_align, scspecs);
  set_c_expr_source_range (&expr, init.src_range);
  expr.m_decimal = 0;
  expr.original_code = ERROR_MARK;
  expr.original_type = NULL;
  if (type != error_mark_node
      && expr.value != error_mark_node
      && type_expr)
    {
      if (TREE_CODE (expr.value) == C_MAYBE_CONST_EXPR)
	{
	  gcc_assert (C_MAYBE_CONST_EXPR_PRE (expr.value) == NULL_TREE);
	  C_MAYBE_CONST_EXPR_PRE (expr.value) = type_expr;
	}
      else
	{
	  gcc_assert (!non_const);
	  expr.value = build2 (C_MAYBE_CONST_EXPR, type,
			       type_expr, expr.value);
	}
    }
  return c_parser_postfix_expression_after_primary (parser, start_loc, expr);
}

/* Callback function for sizeof_pointer_memaccess_warning to compare
   types.  */

static bool
sizeof_ptr_memacc_comptypes (tree type1, tree type2)
{
  return comptypes (type1, type2) == 1;
}

/* Warn for patterns where abs-like function appears to be used incorrectly,
   gracefully ignore any non-abs-like function.  The warning location should
   be LOC.  FNDECL is the declaration of called function, it must be a
   BUILT_IN_NORMAL function.  ARG is the first and only argument of the
   call.  */

static void
warn_for_abs (location_t loc, tree fndecl, tree arg)
{
  /* Avoid warning in unreachable subexpressions.  */
  if (c_inhibit_evaluation_warnings)
    return;

  tree atype = TREE_TYPE (arg);

  /* Casts from pointers (and thus arrays and fndecls) will generate
     -Wint-conversion warnings.  Most other wrong types hopefully lead to type
     mismatch errors.  TODO: Think about what to do with FIXED_POINT_TYPE_P
     types and possibly other exotic types.  */
  if (!INTEGRAL_TYPE_P (atype)
      && !SCALAR_FLOAT_TYPE_P (atype)
      && TREE_CODE (atype) != COMPLEX_TYPE)
    return;

  enum built_in_function fcode = DECL_FUNCTION_CODE (fndecl);

  switch (fcode)
    {
    case BUILT_IN_ABS:
    case BUILT_IN_LABS:
    case BUILT_IN_LLABS:
    case BUILT_IN_IMAXABS:
      if (!INTEGRAL_TYPE_P (atype))
	{
	  if (SCALAR_FLOAT_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using integer absolute value function %qD when "
			"argument is of floating-point type %qT",
			fndecl, atype);
	  else if (TREE_CODE (atype) == COMPLEX_TYPE)
	    warning_at (loc, OPT_Wabsolute_value,
			"using integer absolute value function %qD when "
			"argument is of complex type %qT", fndecl, atype);
	  else
	    gcc_unreachable ();
	  return;
	}
      if (TYPE_UNSIGNED (atype))
	warning_at (loc, OPT_Wabsolute_value,
		    "taking the absolute value of unsigned type %qT "
		    "has no effect", atype);
      break;

    CASE_FLT_FN (BUILT_IN_FABS):
    CASE_FLT_FN_FLOATN_NX (BUILT_IN_FABS):
      if (!SCALAR_FLOAT_TYPE_P (atype)
	  || DECIMAL_FLOAT_MODE_P (TYPE_MODE (atype)))
	{
	  if (INTEGRAL_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using floating-point absolute value function %qD "
			"when argument is of integer type %qT", fndecl, atype);
	  else if (DECIMAL_FLOAT_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using floating-point absolute value function %qD "
			"when argument is of decimal floating-point type %qT",
			fndecl, atype);
	  else if (TREE_CODE (atype) == COMPLEX_TYPE)
	    warning_at (loc, OPT_Wabsolute_value,
			"using floating-point absolute value function %qD when "
			"argument is of complex type %qT", fndecl, atype);
	  else
	    gcc_unreachable ();
	  return;
	}
      break;

    CASE_FLT_FN (BUILT_IN_CABS):
      if (TREE_CODE (atype) != COMPLEX_TYPE)
	{
	  if (INTEGRAL_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using complex absolute value function %qD when "
			"argument is of integer type %qT", fndecl, atype);
	  else if (SCALAR_FLOAT_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using complex absolute value function %qD when "
			"argument is of floating-point type %qT",
			fndecl, atype);
	  else
	    gcc_unreachable ();

	  return;
	}
      break;

    case BUILT_IN_FABSD32:
    case BUILT_IN_FABSD64:
    case BUILT_IN_FABSD128:
      if (!DECIMAL_FLOAT_TYPE_P (atype))
	{
	  if (INTEGRAL_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using decimal floating-point absolute value "
			"function %qD when argument is of integer type %qT",
			fndecl, atype);
	  else if (SCALAR_FLOAT_TYPE_P (atype))
	    warning_at (loc, OPT_Wabsolute_value,
			"using decimal floating-point absolute value "
			"function %qD when argument is of floating-point "
			"type %qT", fndecl, atype);
	  else if (TREE_CODE (atype) == COMPLEX_TYPE)
	    warning_at (loc, OPT_Wabsolute_value,
			"using decimal floating-point absolute value "
			"function %qD when argument is of complex type %qT",
			fndecl, atype);
	  else
	    gcc_unreachable ();
	  return;
	}
      break;

    default:
      return;
    }

  if (!TYPE_ARG_TYPES (TREE_TYPE (fndecl)))
    return;

  tree ftype = TREE_VALUE (TYPE_ARG_TYPES (TREE_TYPE (fndecl)));
  if (TREE_CODE (atype) == COMPLEX_TYPE)
    {
      gcc_assert (TREE_CODE (ftype) == COMPLEX_TYPE);
      atype = TREE_TYPE (atype);
      ftype = TREE_TYPE (ftype);
    }

  if (TYPE_PRECISION (ftype) < TYPE_PRECISION (atype))
    warning_at (loc, OPT_Wabsolute_value,
		"absolute value function %qD given an argument of type %qT "
		"but has parameter of type %qT which may cause truncation "
		"of value", fndecl, atype, ftype);
}


/* Parse a postfix expression after the initial primary or compound
   literal; that is, parse a series of postfix operators.

   EXPR_LOC is the location of the primary expression.  */

static struct c_expr
c_parser_postfix_expression_after_primary (c_parser *parser,
					   location_t expr_loc,
					   struct c_expr expr)
{
  struct c_expr orig_expr;
  tree ident, idx;
  location_t sizeof_arg_loc[3], comp_loc;
  tree sizeof_arg[3];
  unsigned int literal_zero_mask;
  unsigned int i;
  vec<tree, va_gc> *exprlist;
  vec<tree, va_gc> *origtypes = NULL;
  vec<location_t> arg_loc = vNULL;
  location_t start;
  location_t finish;

  while (true)
    {
      location_t op_loc = c_parser_peek_token (parser)->location;
      switch (c_parser_peek_token (parser)->type)
	{
	case CPP_OPEN_SQUARE:
	  /* Array reference.  */
	  c_parser_consume_token (parser);
	  idx = c_parser_expression (parser).value;
	  c_parser_skip_until_found (parser, CPP_CLOSE_SQUARE,
				     "expected %<]%>");
	  start = expr.get_start ();
	  finish = parser->tokens_buf[0].location;
	  expr.value = build_array_ref (op_loc, expr.value, idx);
	  set_c_expr_source_range (&expr, start, finish);
	  expr.original_code = ERROR_MARK;
	  expr.original_type = NULL;
	  expr.m_decimal = 0;
	  break;
	case CPP_OPEN_PAREN:
	  /* Function call.  */
	  {
	    matching_parens parens;
	    parens.consume_open (parser);
	    for (i = 0; i < 3; i++)
	      {
		sizeof_arg[i] = NULL_TREE;
		sizeof_arg_loc[i] = UNKNOWN_LOCATION;
	      }
	    literal_zero_mask = 0;
	    if (c_parser_next_token_is (parser, CPP_CLOSE_PAREN))
	      exprlist = NULL;
	    else
	      exprlist = c_parser_expr_list (parser, true, false, &origtypes,
					     sizeof_arg_loc, sizeof_arg,
					     &arg_loc, &literal_zero_mask);
	    parens.skip_until_found_close (parser);
	  }
	  orig_expr = expr;
	  mark_exp_read (expr.value);
	  if (warn_sizeof_pointer_memaccess)
	    sizeof_pointer_memaccess_warning (sizeof_arg_loc,
					      expr.value, exprlist,
					      sizeof_arg,
					      sizeof_ptr_memacc_comptypes);
	  if (TREE_CODE (expr.value) == FUNCTION_DECL)
	    {
	      if (fndecl_built_in_p (expr.value, BUILT_IN_MEMSET)
		  && vec_safe_length (exprlist) == 3)
		{
		  tree arg0 = (*exprlist)[0];
		  tree arg2 = (*exprlist)[2];
		  warn_for_memset (expr_loc, arg0, arg2, literal_zero_mask);
		}
	      if (warn_absolute_value
		  && fndecl_built_in_p (expr.value, BUILT_IN_NORMAL)
		  && vec_safe_length (exprlist) == 1)
		warn_for_abs (expr_loc, expr.value, (*exprlist)[0]);
	    }

	  start = expr.get_start ();
	  finish = parser->tokens_buf[0].get_finish ();
	  expr.value
	    = c_build_function_call_vec (expr_loc, arg_loc, expr.value,
					 exprlist, origtypes);
	  set_c_expr_source_range (&expr, start, finish);
	  expr.m_decimal = 0;

	  expr.original_code = ERROR_MARK;
	  if (TREE_CODE (expr.value) == INTEGER_CST
	      && TREE_CODE (orig_expr.value) == FUNCTION_DECL
	      && fndecl_built_in_p (orig_expr.value, BUILT_IN_CONSTANT_P))
	    expr.original_code = C_MAYBE_CONST_EXPR;
	  expr.original_type = NULL;
	  if (exprlist)
	    {
	      release_tree_vector (exprlist);
	      release_tree_vector (origtypes);
	    }
	  arg_loc.release ();
	  break;
	case CPP_DOT:
	  /* Structure element reference.  */
	  c_parser_consume_token (parser);
	  expr = default_function_array_conversion (expr_loc, expr);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    {
	      c_token *comp_tok = c_parser_peek_token (parser);
	      ident = comp_tok->value;
	      comp_loc = comp_tok->location;
	    }
	  else
	    {
	      c_parser_error (parser, "expected identifier");
	      expr.set_error ();
	      expr.original_code = ERROR_MARK;
              expr.original_type = NULL;
	      return expr;
	    }
	  start = expr.get_start ();
	  finish = c_parser_peek_token (parser)->get_finish ();
	  c_parser_consume_token (parser);
	  expr.value = build_component_ref (op_loc, expr.value, ident,
					    comp_loc, UNKNOWN_LOCATION);
	  set_c_expr_source_range (&expr, start, finish);
	  expr.original_code = ERROR_MARK;
	  if (TREE_CODE (expr.value) != COMPONENT_REF)
	    expr.original_type = NULL;
	  else
	    {
	      /* Remember the original type of a bitfield.  */
	      tree field = TREE_OPERAND (expr.value, 1);
	      if (TREE_CODE (field) != FIELD_DECL)
		expr.original_type = NULL;
	      else
		expr.original_type = DECL_BIT_FIELD_TYPE (field);
	    }
	  expr.m_decimal = 0;
	  break;
	case CPP_DEREF:
	  /* Structure element reference.  */
	  c_parser_consume_token (parser);
	  expr = convert_lvalue_to_rvalue (expr_loc, expr, true, false);
	  if (c_parser_next_token_is (parser, CPP_NAME))
	    {
	      c_token *comp_tok = c_parser_peek_token (parser);
	      ident = comp_tok->value;
	      comp_loc = comp_tok->location;
	    }
	  else
	    {
	      c_parser_error (parser, "expected identifier");
	      expr.set_error ();
	      expr.original_code = ERROR_MARK;
	      expr.original_type = NULL;
	      return expr;
	    }
	  start = expr.get_start ();
	  finish = c_parser_peek_token (parser)->get_finish ();
	  c_parser_consume_token (parser);
	  expr.value = build_component_ref (op_loc,
					    build_indirect_ref (op_loc,
								expr.value,
								RO_ARROW),
					    ident, comp_loc,
					    expr.get_location ());
	  set_c_expr_source_range (&expr, start, finish);
	  expr.original_code = ERROR_MARK;
	  if (TREE_CODE (expr.value) != COMPONENT_REF)
	    expr.original_type = NULL;
	  else
	    {
	      /* Remember the original type of a bitfield.  */
	      tree field = TREE_OPERAND (expr.value, 1);
	      if (TREE_CODE (field) != FIELD_DECL)
		expr.original_type = NULL;
	      else
		expr.original_type = DECL_BIT_FIELD_TYPE (field);
	    }
	  expr.m_decimal = 0;
	  break;
	case CPP_PLUS_PLUS:
	  /* Postincrement.  */
	  start = expr.get_start ();
	  finish = c_parser_peek_token (parser)->get_finish ();
	  c_parser_consume_token (parser);
	  expr = default_function_array_read_conversion (expr_loc, expr);
	  expr.value = build_unary_op (op_loc, POSTINCREMENT_EXPR,
				       expr.value, false);
	  set_c_expr_source_range (&expr, start, finish);
	  expr.original_code = ERROR_MARK;
	  expr.original_type = NULL;
	  break;
	case CPP_MINUS_MINUS:
	  /* Postdecrement.  */
	  start = expr.get_start ();
	  finish = c_parser_peek_token (parser)->get_finish ();
	  c_parser_consume_token (parser);
	  expr = default_function_array_read_conversion (expr_loc, expr);
	  expr.value = build_unary_op (op_loc, POSTDECREMENT_EXPR,
				       expr.value, false);
	  set_c_expr_source_range (&expr, start, finish);
	  expr.original_code = ERROR_MARK;
	  expr.original_type = NULL;
	  break;
	default:
	  return expr;
	}
    }
}

/* Parse an expression (C90 6.3.17, C99 6.5.17, C11 6.5.17).

   expression:
     assignment-expression
     expression , assignment-expression
*/

static struct c_expr
c_parser_expression (c_parser *parser)
{
  location_t tloc = c_parser_peek_token (parser)->location;
  struct c_expr expr;
  expr = c_parser_expr_no_commas (parser, NULL);
  if (c_parser_next_token_is (parser, CPP_COMMA))
    expr = convert_lvalue_to_rvalue (tloc, expr, true, false);
  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      struct c_expr next;
      tree lhsval;
      location_t loc = c_parser_peek_token (parser)->location;
      location_t expr_loc;
      c_parser_consume_token (parser);
      expr_loc = c_parser_peek_token (parser)->location;
      lhsval = expr.value;
      while (TREE_CODE (lhsval) == COMPOUND_EXPR
	     || TREE_CODE (lhsval) == NOP_EXPR)
	{
	  if (TREE_CODE (lhsval) == COMPOUND_EXPR)
	    lhsval = TREE_OPERAND (lhsval, 1);
	  else
	    lhsval = TREE_OPERAND (lhsval, 0);
	}
      if (DECL_P (lhsval) || handled_component_p (lhsval))
	mark_exp_read (lhsval);
      next = c_parser_expr_no_commas (parser, NULL);
      next = convert_lvalue_to_rvalue (expr_loc, next, true, false);
      expr.value = build_compound_expr (loc, expr.value, next.value);
      expr.original_code = COMPOUND_EXPR;
      expr.original_type = next.original_type;
      expr.m_decimal = 0;
    }
  return expr;
}

/* Parse an expression and convert functions or arrays to pointers and
   lvalues to rvalues.  */

static struct c_expr
c_parser_expression_conv (c_parser *parser)
{
  struct c_expr expr;
  location_t loc = c_parser_peek_token (parser)->location;
  expr = c_parser_expression (parser);
  expr = convert_lvalue_to_rvalue (loc, expr, true, false);
  return expr;
}

/* Helper function of c_parser_expr_list.  Check if IDXth (0 based)
   argument is a literal zero alone and if so, set it in literal_zero_mask.  */

static inline void
c_parser_check_literal_zero (c_parser *parser, unsigned *literal_zero_mask,
			     unsigned int idx)
{
  if (idx >= HOST_BITS_PER_INT)
    return;

  c_token *tok = c_parser_peek_token (parser);
  switch (tok->type)
    {
    case CPP_NUMBER:
    case CPP_CHAR:
    case CPP_WCHAR:
    case CPP_CHAR16:
    case CPP_CHAR32:
    case CPP_UTF8CHAR:
      /* If a parameter is literal zero alone, remember it
	 for -Wmemset-transposed-args warning.  */
      if (integer_zerop (tok->value)
	  && !TREE_OVERFLOW (tok->value)
	  && (c_parser_peek_2nd_token (parser)->type == CPP_COMMA
	      || c_parser_peek_2nd_token (parser)->type == CPP_CLOSE_PAREN))
	*literal_zero_mask |= 1U << idx;
    default:
      break;
    }
}

/* Parse a non-empty list of expressions.  If CONVERT_P, convert
   functions and arrays to pointers and lvalues to rvalues.  If
   FOLD_P, fold the expressions.  If LOCATIONS is non-NULL, save the
   locations of function arguments into this vector.

   nonempty-expr-list:
     assignment-expression
     nonempty-expr-list , assignment-expression
*/

static vec<tree, va_gc> *
c_parser_expr_list (c_parser *parser, bool convert_p, bool fold_p,
		    vec<tree, va_gc> **p_orig_types,
		    location_t *sizeof_arg_loc, tree *sizeof_arg,
		    vec<location_t> *locations,
		    unsigned int *literal_zero_mask)
{
  vec<tree, va_gc> *ret;
  vec<tree, va_gc> *orig_types;
  struct c_expr expr;
  unsigned int idx = 0;

  ret = make_tree_vector ();
  if (p_orig_types == NULL)
    orig_types = NULL;
  else
    orig_types = make_tree_vector ();

  if (literal_zero_mask)
    c_parser_check_literal_zero (parser, literal_zero_mask, 0);
  expr = c_parser_expr_no_commas (parser, NULL);
  if (convert_p)
    expr = convert_lvalue_to_rvalue (expr.get_location (), expr, true, true);
  if (fold_p)
    expr.value = c_fully_fold (expr.value, false, NULL);
  ret->quick_push (expr.value);
  if (orig_types)
    orig_types->quick_push (expr.original_type);
  if (locations)
    locations->safe_push (expr.get_location ());
  if (sizeof_arg != NULL
      && (expr.original_code == SIZEOF_EXPR
	  || expr.original_code == PAREN_SIZEOF_EXPR))
    {
      sizeof_arg[0] = c_last_sizeof_arg;
      sizeof_arg_loc[0] = c_last_sizeof_loc;
    }
  while (c_parser_next_token_is (parser, CPP_COMMA))
    {
      c_parser_consume_token (parser);
      if (literal_zero_mask)
	c_parser_check_literal_zero (parser, literal_zero_mask, idx + 1);
      expr = c_parser_expr_no_commas (parser, NULL);
      if (convert_p)
	expr = convert_lvalue_to_rvalue (expr.get_location (), expr, true,
					 true);
      if (fold_p)
	expr.value = c_fully_fold (expr.value, false, NULL);
      vec_safe_push (ret, expr.value);
      if (orig_types)
	vec_safe_push (orig_types, expr.original_type);
      if (locations)
	locations->safe_push (expr.get_location ());
      if (++idx < 3
	  && sizeof_arg != NULL
	  && (expr.original_code == SIZEOF_EXPR
	      || expr.original_code == PAREN_SIZEOF_EXPR))
	{
	  sizeof_arg[idx] = c_last_sizeof_arg;
	  sizeof_arg_loc[idx] = c_last_sizeof_loc;
	}
    }
  if (orig_types)
    *p_orig_types = orig_types;
  return ret;
}

/* Parse a pragma GCC ivdep.  */

static bool
c_parse_pragma_ivdep (c_parser *parser)
{
  c_parser_consume_pragma (parser);
  c_parser_skip_to_pragma_eol (parser);
  return true;
}

/* Parse a pragma GCC unroll.  */

static unsigned short
c_parser_pragma_unroll (c_parser *parser)
{
  unsigned short unroll;
  c_parser_consume_pragma (parser);
  location_t location = c_parser_peek_token (parser)->location;
  tree expr = c_parser_expr_no_commas (parser, NULL).value;
  mark_exp_read (expr);
  expr = c_fully_fold (expr, false, NULL);
  HOST_WIDE_INT lunroll = 0;
  if (!INTEGRAL_TYPE_P (TREE_TYPE (expr))
      || TREE_CODE (expr) != INTEGER_CST
      || (lunroll = tree_to_shwi (expr)) < 0
      || lunroll >= USHRT_MAX)
    {
      error_at (location, "%<#pragma GCC unroll%> requires an"
		" assignment-expression that evaluates to a non-negative"
		" integral constant less than %u", USHRT_MAX);
      unroll = 0;
    }
  else
    {
      unroll = (unsigned short)lunroll;
      if (unroll == 0)
	unroll = 1;
    }

  c_parser_skip_to_pragma_eol (parser);
  return unroll;
}

/* Handle pragmas.  Some OpenMP pragmas are associated with, and therefore
   should be considered, statements.  ALLOW_STMT is true if we're within
   the context of a function and such pragmas are to be allowed.  Returns
   true if we actually parsed such a pragma.  */

static bool
c_parser_pragma (c_parser *parser, enum pragma_context context, bool *if_p)
{
  unsigned int id;
  const char *construct = NULL;

  input_location = c_parser_peek_token (parser)->location;
  id = c_parser_peek_token (parser)->pragma_kind;
  gcc_assert (id != PRAGMA_NONE);

  switch (id)
    {
    case PRAGMA_UNROLL:
      {
	unsigned short unroll = c_parser_pragma_unroll (parser);
	bool ivdep;
	if (c_parser_peek_token (parser)->pragma_kind == PRAGMA_IVDEP)
	  ivdep = c_parse_pragma_ivdep (parser);
	else
	  ivdep = false;
	if (!c_parser_next_token_is_keyword (parser, RID_FOR)
	    && !c_parser_next_token_is_keyword (parser, RID_WHILE)
	    && !c_parser_next_token_is_keyword (parser, RID_DO))
	  {
	    c_parser_error (parser, "for, while or do statement expected");
	    return false;
	  }
	if (c_parser_next_token_is_keyword (parser, RID_FOR))
	  c_parser_for_statement (parser, ivdep, unroll, if_p);
	else if (c_parser_next_token_is_keyword (parser, RID_WHILE))
	  c_parser_while_statement (parser, ivdep, unroll, if_p);
	else
	  c_parser_do_statement (parser, ivdep, unroll);
      }
      return true;

    case PRAGMA_GCC_PCH_PREPROCESS:
      c_parser_error (parser, "%<#pragma GCC pch_preprocess%> must be first");
      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
      return false;

    default:
      if (id < PRAGMA_FIRST_EXTERNAL)
	{
	  if (context != pragma_stmt && context != pragma_compound)
	    {
	    bad_stmt:
	      c_parser_error (parser, "expected declaration specifiers");
	      c_parser_skip_until_found (parser, CPP_PRAGMA_EOL, NULL);
	      return false;
	    }
	  return true;
	}
      break;
    }

  c_parser_consume_pragma (parser);
  c_invoke_pragma_handler (id);

  /* Skip to EOL, but suppress any error message.  Those will have been
     generated by the handler routine through calling error, as opposed
     to calling c_parser_error.  */
  parser->error = true;
  c_parser_skip_to_pragma_eol (parser);

  return false;
}

/* The interface the pragma parsers have to the lexer.  */

enum cpp_ttype
pragma_lex (tree *value, location_t *loc)
{
  c_token *tok = c_parser_peek_token (the_parser);
  enum cpp_ttype ret = tok->type;

  *value = tok->value;
  if (loc)
    *loc = tok->location;

  if (ret == CPP_PRAGMA_EOL || ret == CPP_EOF)
    ret = CPP_EOF;
  else if (ret == CPP_STRING)
    *value = c_parser_string_literal (the_parser, false, false).value;
  else
    {
      if (ret == CPP_KEYWORD)
	ret = CPP_NAME;
      c_parser_consume_token (the_parser);
    }

  return ret;
}

static void
c_parser_pragma_pch_preprocess (c_parser *parser)
{
  tree name = NULL;

  parser->lex_joined_string = true;
  c_parser_consume_pragma (parser);
  if (c_parser_next_token_is (parser, CPP_STRING))
    {
      name = c_parser_peek_token (parser)->value;
      c_parser_consume_token (parser);
    }
  else
    c_parser_error (parser, "expected string literal");
  c_parser_skip_to_pragma_eol (parser);
  parser->lex_joined_string = false;

  if (name)
    c_common_pch_pragma (parse_in, TREE_STRING_POINTER (name));
}

/* Parse a single source file.  */

void
c_parse_file (void)
{
  /* Use local storage to begin.  If the first token is a pragma, parse it.
     If it is #pragma GCC pch_preprocess, then this will load a PCH file
     which will cause garbage collection.  */
  c_parser tparser;

  memset (&tparser, 0, sizeof tparser);
  tparser.translate_strings_p = true;
  tparser.tokens = &tparser.tokens_buf[0];
  the_parser = &tparser;

  if (c_parser_peek_token (&tparser)->pragma_kind == PRAGMA_GCC_PCH_PREPROCESS)
    c_parser_pragma_pch_preprocess (&tparser);
  else
    c_common_no_more_pch ();

  the_parser = ggc_alloc<c_parser> ();
  *the_parser = tparser;
  if (tparser.tokens == &tparser.tokens_buf[0])
    the_parser->tokens = &the_parser->tokens_buf[0];

  /* Initialize EH, if we've been told to do so.  */
  if (flag_exceptions)
    using_eh_for_cleanups ();

  c_parser_translation_unit (the_parser);
  the_parser = NULL;
}

#include "gt-c-c-parser.h"
