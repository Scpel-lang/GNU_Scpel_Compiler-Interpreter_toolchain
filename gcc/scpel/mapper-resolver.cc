/* Scpel modules.  Experimental!	-*- c++ -*- */

/* Forward to the resolver in c++tools.  */

#include "config.h"
#define INCLUDE_STRING
#define INCLUDE_VECTOR
#define INCLUDE_ALGORITHM
#define INCLUDE_MAP
#define INCLUDE_MEMORY
#include "system.h"

// We don't want or need to be aware of networking
#define CODY_NETWORKING 0
#include "../../c++tools/resolver.cc"
