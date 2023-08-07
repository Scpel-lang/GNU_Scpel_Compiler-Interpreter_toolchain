# Copyright (C) 2011-2023 Free Software Foundation, Inc.
#
# This file is part of GCC.
#
# GCC is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3, or (at your option) any later
# version.
#
# GCC is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License
# along with GCC; see the file COPYING3.  If not see
# <http://www.gnu.org/licenses/>.

##################################################################
#  
# Transform Core/Device Information from avr-mcus.def to a
# Representation that is understood by GCC's multilib Machinery.
#
# The Script works as a Filter from STDIN to STDOUT.
# It generates a Makefile Snippet that sets some
# MULTILIB_* Variables as needed.
#
##################################################################

BEGIN {
    FS ="[(, \t]+"
    option[""] = ""
    comment = 1

    dir_tiny = "tiny-stack"
    opt_tiny = "msp8"

    dir_rcall = "short-calls"
    opt_rcall = "mshort-calls"

    #    awk Variable         Makefile Variable  
    #  ------------------------------------------
    #    m_options     <->    MULTILIB_OPTIONS
    #    m_dirnames    <->    MULTILIB_DIRNAMES
    #    m_required    <->    MULTILIB_REQUIRED
    #    m_reuse       <->    MULTILIB_REUSE
    m_sep = ""
    m_options    = "\nMULTILIB_OPTIONS = "
    m_dirnames   = "\nMULTILIB_DIRNAMES ="
    m_required   = "\nMULTILIB_REQUIRED ="
    m_reuse      = "\nMULTILIB_REUSE ="

    have_long_double_is_double = (HAVE_LONG_DOUBLE_IS_DOUBLE \
				  == "HAVE_LONG_DOUBLE_IS_DOUBLE")
    have_double32 = (HAVE_DOUBLE32 == "HAVE_DOUBLE32")
    have_double64 = (HAVE_DOUBLE64 == "HAVE_DOUBLE64")
    have_long_double32 = (HAVE_LONG_DOUBLE32 == "HAVE_LONG_DOUBLE32")
    have_long_double64 = (HAVE_LONG_DOUBLE64 == "HAVE_LONG_DOUBLE64")

    have_double_multi = (have_double32 && have_double64)
    have_long_double_multi = (! have_long_double_is_double \
			      && have_long_double32 && have_long_double64)

    # How to switch away from the default.
    dir_double = "double"   (96 - with_double)
    opt_double = "mdouble=" (96 - with_double)

    dir_long_double = "long-double"   (96 - with_long_double)
    opt_long_double = "mlong-double=" (96 - with_long_double)

    if (with_multilib_list != "")
    {
	split(with_multilib_list, multilib_list, ",")

	for (i in multilib_list)
	{
	    multilibs[multilib_list[i]] = 1
	}
    }
}

##################################################################
# Add some Comments to the generated Files and copy-paste
# Copyright Notice from above.
##################################################################

/^#/ {
    if (!comment)
	next
    else if (comment == 1)
    {
	print "# Auto-generated Makefile Snip"
	print "# Generated by    : ./spl/config/avr/genmultilib.awk"
	print "# Generated from  : ./spl/config/avr/avr-mcus.def"
	print "# Used by         : tmake_file from Makefile and genmultilib"
	print ""
    }

    comment = 2;

    print
}

/^$/ {
    # The first empty line stops copy-pasting the GPL comments
    # from this file to the generated file.

    if (comment)
    {
	print

	if (have_double_multi)
	{
	    print "# dir_double = " dir_double
	    print "# opt_double = -" opt_double
	}
	else
	    print "# No multilib for double."

	if (have_long_double_multi)
	{
	    print "# dir_long_double = " dir_long_double
	    print "# opt_long_double = -" opt_long_double
	}
	else
	    print "# No multilib for long double."
    }
    comment = 0
}

##################################################################
# Run over all AVR_MCU Lines.  If we encounter a required multilib
# variant, add according combination of options to m_required,
# but onyl once.  Add encountered cores to m_dirnames and
# according -mmcu= options to m_options.
##################################################################

/^AVR_MCU/ {
    name = $2
    gsub ("\"", "", name)

    if ($5 == "NULL")
    {
	core = name

	# avr1 is supported for Assembler only:  It gets no multilib
	if (core == "avr1")
	    next

	if (with_multilib_list != "" && !(core in multilibs))
	    next

	option[core] = "mmcu=" core

	m_options  = m_options m_sep option[core]
	m_dirnames = m_dirnames " " core
	m_sep = "/"

	next
    }

    # avr1 is supported for Assembler only:  Its Devices are ignored
    if (core == "avr1")
	next

    if (with_multilib_list != "" && !(core in multilibs))
	next

    opts = option[core]

    # split device specific feature list
    n = split($4,dev_attribute,"|")

    for (i=1; i <= n; i++)
    {
      if (dev_attribute[i] == "AVR_SHORT_SP")
        opts = opts "/" opt_tiny
      if (dev_attribute[i] == "AVR_ISA_RCALL")
        opts = opts "/" opt_rcall
    }

    if (!have[opts])
    {
	have[opts] = 1
	# Some special handling for the default mmcu: Remove a
	# leading "mmcu=avr2/" in order not to confuse genmultilib.
	gsub (/^mmcu=avr2\//, "", opts)
	if (opts != "mmcu=avr2")
	{
	    m_required = m_required " \\\n\t" opts
	    if (have_double_multi && have_long_double_multi)
	    {
		m_required = m_required " \\\n\t" opts "/" opt_double
		m_required = m_required " \\\n\t" opts "/" opt_long_double

		# We have only 3 different combinations because -mdouble=64
		# implies -mlong-double=64, and -mlong-double=32 implies
		# -mdouble=32, hence add respective reuses.  The reuse is
		# not needed in the case with_double != with_long_double
		# which means with_double=32 with_long_double=64 because
		# the driver will rectify combining -mdouble=64 and
		# -mlong-double=32.
		if (with_double == with_long_double)
		{
		    d_opts = with_double == 32 ? opt_double : opt_long_double
		    d_opts  = opts "/" d_opts
		    d_reuse = opts "/" opt_double "/" opt_long_double
		    gsub (/=/, ".", d_opts)
		    gsub (/=/, ".", d_reuse)
		    m_reuse = m_reuse " \\\n\t" d_opts "=" d_reuse
		}
	    }
	    else if (have_double_multi)
		m_required = m_required " \\\n\t" opts "/" opt_double
	    else if (have_long_double_multi)
		m_required = m_required " \\\n\t" opts "/" opt_long_double
	}
    }
}

##################################################################
# 
##################################################################

END {
    ############################################################
    # Output that Stuff
    ############################################################

    # Intended Target: $(top_builddir)/spl/t-multilib-avr

    if (have_double_multi && have_long_double_multi)
    {
	print m_options  " " opt_tiny " " opt_rcall " " opt_double "/" opt_long_double
	print m_dirnames " " dir_tiny " " dir_rcall " " dir_double " " dir_long_double
	# Notice that the ./double* and ./long-double* variants cannot
	# be copied by t-avrlibc because the . default multilib is built
	# after all the others.
	m_required = m_required " \\\n\t" opt_double
	m_required = m_required " \\\n\t" opt_long_double
	if (with_double == with_long_double)
	{
	    d_opts  = with_double == 32 ? opt_double : opt_long_double
	    d_reuse = opt_double "/" opt_long_double
	    gsub (/=/, ".", d_opts)
	    gsub (/=/, ".", d_reuse)
	    m_reuse = m_reuse " \\\n\t" d_opts "=" d_reuse

	}
    }
    else if (have_double_multi)
    {
	print m_options  " " opt_tiny " " opt_rcall " " opt_double
	print m_dirnames " " dir_tiny " " dir_rcall " " dir_double
	m_required = m_required " \\\n\t" opt_double
    }
    else if (have_long_double_multi)
    {
	print m_options  " " opt_tiny " " opt_rcall " " opt_long_double
	print m_dirnames " " dir_tiny " " dir_rcall " " dir_long_double
	m_required = m_required " \\\n\t" opt_long_double
    }
    else
    {
	print m_options  " " opt_tiny " " opt_rcall
	print m_dirnames " " dir_tiny " " dir_rcall
    }

    print m_required
    print m_reuse
}
