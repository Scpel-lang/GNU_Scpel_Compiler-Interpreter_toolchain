/* LTO driver specs. */

/* LTO contributions to the "compilers" array in gcc.cc.  */
  
  {"@lto", "lto1 %(cc1_options) %i %{!fsyntax-only:%(invoke_as)}",
   /*cpp_spec=*/NULL, /*combinable=*/1, /*needs_preprocessing=*/0},
