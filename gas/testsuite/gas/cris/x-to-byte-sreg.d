#objdump: -dr
#name: x-to-byte-sreg-@OR@

.*:     file format .*-cris

Disassembly of section \.text:
0+ <notstart>:
[	 ]+0:[	 ]+0000[ 	]+bcc ( 0x2|\.\+2)
^[ 	]+\.\.\.
0+4 <start>:
[	 ]+4:[	 ]+@IR+3306@[ 	]+move[ ]+\$?r3,.*
[	 ]+6:[	 ]+@IM+300a@[ 	]+move[ ]+\[\$?r0\],.*
[	 ]+8:[	 ]+@IM+300e@[ 	]+move[ ]+\[\$?r0\+\],.*
[	 ]+a:[	 ]+@IM+3f0e@ 0000[ 	]+move[ ]+0x0,.*
[	 ]+e:[	 ]+@IM+3f0e@ 0100[ 	]+move[ ]+0x1,.*
[	 ]+12:[	 ]+@IM+3f0e@ 7f00[ 	]+move[ ]+0x7f,.*
[	 ]+16:[	 ]+@IM+3f0e@ 8000[ 	]+move[ ]+0x80,.*
[	 ]+1a:[	 ]+@IM+3f0e@ ffff[ 	]+move[ ]+0xffff,.*
[	 ]+1e:[	 ]+@IM+3f0e@ 81ff[ 	]+move[ ]+0xff81,.*
[	 ]+22:[	 ]+@IM+3f0e@ 80ff[ 	]+move[ ]+0xff80,.*
[	 ]+26:[	 ]+@IM+3f0e@ ff00[ 	]+move[ ]+0xff,.*
[	 ]+2a:[	 ]+@IM+3f0e@ 2a00[ 	]+move[ ]+0x2a,.*
[	 ]+2e:[	 ]+@IM+3f0e@ d6ff[ 	]+move[ ]+0xffd6,.*
[	 ]+32:[	 ]+@IM+3f0e@ 2a00[ 	]+move[ ]+0x2a,.*
[	 ]+36:[	 ]+@IM+3f0e@ d6ff[ 	]+move[ ]+0xffd6,.*
[	 ]+3a:[	 ]+@IM+3f0e@ d6ff[ 	]+move[ ]+0xffd6,.*
[	 ]+3e:[	 ]+@IM+3f0e@ 2a00[ 	]+move[ ]+0x2a,.*
[	 ]+42:[	 ]+@IM+3f0e@ 0000[ 	]+move[ ]+0x0,.*
[ 	]+44:[ 	]+(R_CRIS_)?16[ 	]+externalsym
[	 ]+46:[	 ]+4205 @IM+300a@[ 	]+move[ ]+\[\$?r2\+\$?r0\.b\],.*
[	 ]+4a:[	 ]+4029 @IM+300a@[ 	]+move[ ]+\[\$?r2\+\[\$?r0\]\.b\],.*
[	 ]+4e:[	 ]+402d @IM+300a@[ 	]+move[ ]+\[\$?r2\+\[\$?r0\+\]\.b\],.*
[	 ]+52:[	 ]+5205 @IM+300a@[ 	]+move[ ]+\[\$?r2\+\$?r0\.w\],.*
[	 ]+56:[	 ]+5029 @IM+300a@[ 	]+move[ ]+\[\$?r2\+\[\$?r0\]\.w\],.*
[	 ]+5a:[	 ]+502d @IM+300a@[ 	]+move[ ]+\[\$?r2\+\[\$?r0\+\]\.w\],.*
[	 ]+5e:[	 ]+6205 @IM+300a@[ 	]+move[ ]+\[\$?r2\+\$?r0\.d\],.*
[	 ]+62:[	 ]+6029 @IM+300a@[ 	]+move[ ]+\[\$?r2\+\[\$?r0\]\.d\],.*
[	 ]+66:[	 ]+602d @IM+300a@[ 	]+move[ ]+\[\$?r2\+\[\$?r0\+\]\.d\],.*
[	 ]+6a:[	 ]+0021 @IM+300a@[ 	]+move[ ]+\[\$?r2\+0\],.*
[	 ]+6e:[	 ]+0121 @IM+300a@[ 	]+move[ ]+\[\$?r2\+1\],.*
[	 ]+72:[	 ]+7f21 @IM+300a@[ 	]+move[ ]+\[\$?r2\+127\],.*
[	 ]+76:[	 ]+5f2d 8000 @IM+300a@[ 	]+move[ ]+\[\$?r2\+128\],.*
[	 ]+7c:[	 ]+ff21 @IM+300a@[ 	]+move[ ]+\[\$?r2-1\],.*
[	 ]+80:[	 ]+8121 @IM+300a@[ 	]+move[ ]+\[\$?r2-127\],.*
[	 ]+84:[	 ]+8021 @IM+300a@[ 	]+move[ ]+\[\$?r2-128\],.*
[	 ]+88:[	 ]+5f2d ff00 @IM+300a@[ 	]+move[ ]+\[\$?r2\+255\],.*
[	 ]+8e:[	 ]+2a21 @IM+300a@[ 	]+move[ ]+\[\$?r2\+42\],.*
[	 ]+92:[	 ]+d621 @IM+300a@[ 	]+move[ ]+\[\$?r2-42\],.*
[	 ]+96:[	 ]+d621 @IM+300a@[ 	]+move[ ]+\[\$?r2-42\],.*
[	 ]+9a:[	 ]+2a21 @IM+300a@[ 	]+move[ ]+\[\$?r2\+42\],.*
[	 ]+9e:[	 ]+d621 @IM+300a@[ 	]+move[ ]+\[\$?r2-42\],.*
[	 ]+a2:[	 ]+d621 @IM+300a@[ 	]+move[ ]+\[\$?r2-42\],.*
[	 ]+a6:[	 ]+2a21 @IM+300a@[ 	]+move[ ]+\[\$?r2\+42\],.*
[	 ]+aa:[	 ]+d621 @IM+300a@[ 	]+move[ ]+\[\$?r2-42\],.*
[	 ]+ae:[	 ]+2a21 @IM+300a@[ 	]+move[ ]+\[\$?r2\+42\],.*
[	 ]+b2:[	 ]+6f2d 0000 0000 @IM+300a@[ 	]+move[ ]+\[\$?r2\+0( <notstart>)?\],.*
[ 	]+b4:[ 	]+(R_CRIS_)?32[ 	]+externalsym
[	 ]+ba:[	 ]+4205 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\$?r0\.b\],.*
[	 ]+be:[	 ]+4029 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\[\$?r0\]\.b\],.*
[	 ]+c2:[	 ]+402d @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\[\$?r0\+\]\.b\],.*
[	 ]+c6:[	 ]+5205 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\$?r0\.w\],.*
[	 ]+ca:[	 ]+5029 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\[\$?r0\]\.w\],.*
[	 ]+ce:[	 ]+502d @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\[\$?r0\+\]\.w\],.*
[	 ]+d2:[	 ]+6205 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\$?r0\.d\],.*
[	 ]+d6:[	 ]+6029 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\[\$?r0\]\.d\],.*
[	 ]+da:[	 ]+602d @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+\[\$?r0\+\]\.d\],.*
[	 ]+de:[	 ]+0021 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+0\],.*
[	 ]+e2:[	 ]+0121 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+1\],.*
[	 ]+e6:[	 ]+7f21 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+127\],.*
[	 ]+ea:[	 ]+5f2d 8000 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+128\],.*
[	 ]+f0:[	 ]+ff21 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-1\],.*
[	 ]+f4:[	 ]+8121 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-127\],.*
[	 ]+f8:[	 ]+8021 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-128\],.*
[	 ]+fc:[	 ]+5f2d ff00 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+255\],.*
[	 ]+102:[	 ]+2a21 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+42\],.*
[	 ]+106:[	 ]+d621 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-42\],.*
[	 ]+10a:[	 ]+d621 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-42\],.*
[	 ]+10e:[	 ]+2a21 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+42\],.*
[	 ]+112:[	 ]+d621 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-42\],.*
[	 ]+116:[	 ]+d621 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-42\],.*
[	 ]+11a:[	 ]+2a21 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+42\],.*
[	 ]+11e:[	 ]+d621 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2-42\],.*
[	 ]+122:[	 ]+2a21 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+42\],.*
[	 ]+126:[	 ]+6f2d 0000 0000 @IM+3c0e@[ 	]+move[ ]+\[\$?r12=\$?r2\+0( <notstart>)?\],.*
[ 	]+128:[ 	]+(R_CRIS_)?32[ 	]+externalsym
[	 ]+12e:[	 ]+7309 @IM+300a@[ 	]+move[ ]+\[\[\$?r3\]\],.*
[	 ]+132:[	 ]+790d @IM+300a@[ 	]+move[ ]+\[\[\$?r9\+\]\],.*
[	 ]+136:[	 ]+7f0d 0000 0000 @IM+300a@[ 	]+move[ ]+\[(0x0|0 <notstart>)\],.*
[ 	]+138:[ 	]+(R_CRIS_)?32[ 	]+externalsym
[	 ]+13e:[	 ]+7f0d 0000 0000 @IM+300a@[ 	]+move[ ]+\[(0x0|0 <notstart>)\],.*
[ 	]+140:[ 	]+(R_CRIS_)?32[ 	]+\.text
0+146 <end>:
^[ 	]+\.\.\.
