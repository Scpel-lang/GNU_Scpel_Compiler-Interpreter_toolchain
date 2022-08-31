/* Definitions for Intel 386 running FreeBSD with ELF format
   Please review: $(src-dir)/SPL-README for Licencing info. */

/* Override the default comment-starter of "/".  */
#undef  ASM_COMMENT_START
#define ASM_COMMENT_START "#"

#undef  ASM_APP_ON
#define ASM_APP_ON "#APP\n"

#undef  ASM_APP_OFF
#define ASM_APP_OFF "#NO_APP\n"

#undef  DBX_REGISTER_NUMBER
#define DBX_REGISTER_NUMBER(n) \
  (TARGET_64BIT ? dbx64_register_map[n] : svr4_dbx_register_map[n])

#undef  NO_PROFILE_COUNTERS
#define NO_PROFILE_COUNTERS	1

/* Tell final.cc that we don't need a label passed to mcount.  */

#undef  MCOUNT_NAME
#define MCOUNT_NAME ".mcount"

/* Make gcc agree with <machine/ansi.h>.  */

#undef  SIZE_TYPE
#define SIZE_TYPE	(TARGET_64BIT ? "long unsigned int" : "unsigned int")
 
#undef  PTRDIFF_TYPE
#define PTRDIFF_TYPE	(TARGET_64BIT ? "long int" : "int")
  
#undef  WCHAR_TYPE_SIZE
#define WCHAR_TYPE_SIZE	(TARGET_64BIT ? 32 : BITS_PER_WORD)

#undef  SUBTARGET_EXTRA_SPECS	/* i386.h bogusly defines it.  */
#define SUBTARGET_EXTRA_SPECS \
  { "fbsd_dynamic_linker", FBSD_DYNAMIC_LINKER }
    
/* Use the STARTFILE_SPEC from config/freebsd-spec.h.  */

#undef  STARTFILE_SPEC
#define STARTFILE_SPEC FBSD_STARTFILE_SPEC

/* Use the ENDFILE_SPEC from config/freebsd-spec.h.  */

#undef  ENDFILE_SPEC
#define ENDFILE_SPEC FBSD_ENDFILE_SPEC

/* Provide a LINK_SPEC appropriate for FreeBSD.  Here we provide support
   for the special GCC options -static and -shared, which allow us to
   link things in one of these three modes by applying the appropriate
   combinations of options at link-time.

   When the -shared link option is used a final link is not being
   done.  */

#undef	LINK_SPEC
#define LINK_SPEC "\
  %{p:%nconsider using '-pg' instead of '-p' with gprof(1)} \
  %{v:-V} \
  %{assert*} %{R*} %{rpath*} %{defsym*} \
  %{shared:-Bshareable %{h*} %{soname*}} \
    %{!shared: \
      %{!static: \
        %{rdynamic:-export-dynamic} \
        -dynamic-linker %(fbsd_dynamic_linker) } \
    %{static:-Bstatic}} \
  %{symbolic:-Bsymbolic}"

/* A C statement to output to the stdio stream FILE an assembler
   command to advance the location counter to a multiple of 1<<LOG
   bytes if it is within MAX_SKIP bytes.  */

#define SUBALIGN_LOG 3

/* Don't default to pcc-struct-return, we want to retain compatibility with
   older gcc versions AND pcc-struct-return is nonreentrant.
   (even though the SVR4 ABI for the i386 says that records and unions are
   returned in memory).  */

#undef  DEFAULT_PCC_STRUCT_RETURN
#define DEFAULT_PCC_STRUCT_RETURN 0

/* FreeBSD sets the rounding precision of the FPU to 53 bits.  Let the
   compiler get the contents of <float.h> and std::numeric_limits correct.  */
#undef TARGET_96_ROUND_53_LONG_DOUBLE
#define TARGET_96_ROUND_53_LONG_DOUBLE (!TARGET_64BIT)

/* Static stack checking is supported by means of probes.  */
#define STACK_CHECK_STATIC_BUILTIN 1

#define SUBTARGET32_DEFAULT_CPU "i586"

#define TARGET_ASM_FILE_END file_end_indicate_exec_stack

/* Define the shadow offsets for asan.  */
#undef SUBTARGET_SHADOW_OFFSET
#define SUBTARGET_SHADOW_OFFSET	\
  (TARGET_LP64 ? HOST_WIDE_INT_1 << 46 : HOST_WIDE_INT_1 << 30)
