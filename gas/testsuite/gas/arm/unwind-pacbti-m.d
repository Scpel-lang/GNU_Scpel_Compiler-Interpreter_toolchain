#objdump: -j ".ARM.extab" -s
#name: Unwind information for Armv8.1-M.Mainline PACBTI extension
# This test is only valid on ELF based ports.
#notarget: *-*-pe *-*-wince
# VxWorks needs a special variant of this file.
#skip: *-*-vxworks*

.*:     file format.*

Contents of section .ARM.extab:
 0000 (00840281 b40084b4 b0a8b4a3|81028400 b48400b4 a3b4a8b0) 00000000  .*
