#as: -march=rv64i -defsym __64_bit__=1
#source: ext.s
#objdump: -d

.*:[ 	]+file format .*


Disassembly of section .text:

0+000 <target>:
[ 	]+0:[ 	]+0ff57513[ 	]+zext.b[ 	]+a0,a0
[ 	]+4:[ 	]+03051513[ 	]+sll[ 	]+a0,a0,0x30
[ 	]+8:[ 	]+03055513[ 	]+srl[ 	]+a0,a0,0x30
[ 	]+c:[ 	]+03851513[ 	]+sll[ 	]+a0,a0,0x38
[ 	]+10:[ 	]+43855513[ 	]+sra[ 	]+a0,a0,0x38
[ 	]+14:[ 	]+03051513[ 	]+sll[ 	]+a0,a0,0x30
[ 	]+18:[ 	]+43055513[ 	]+sra[ 	]+a0,a0,0x30
[ 	]+1c:[ 	]+0ff67593[ 	]+zext.b[ 	]+a1,a2
[ 	]+20:[ 	]+03061593[ 	]+sll[ 	]+a1,a2,0x30
[ 	]+24:[ 	]+0305d593[ 	]+srl[ 	]+a1,a1,0x30
[ 	]+28:[ 	]+03861593[ 	]+sll[ 	]+a1,a2,0x38
[ 	]+2c:[ 	]+4385d593[ 	]+sra[ 	]+a1,a1,0x38
[ 	]+30:[ 	]+03061593[ 	]+sll[ 	]+a1,a2,0x30
[ 	]+34:[ 	]+4305d593[ 	]+sra[ 	]+a1,a1,0x30
[ 	]+38:[ 	]+02051513[ 	]+sll[ 	]+a0,a0,0x20
[ 	]+3c:[ 	]+02055513[ 	]+srl[ 	]+a0,a0,0x20
[ 	]+40:[ 	]+0005051b[ 	]+sext.w[ 	]+a0,a0
[ 	]+44:[ 	]+02061593[ 	]+sll[ 	]+a1,a2,0x20
[ 	]+48:[ 	]+0205d593[ 	]+srl[ 	]+a1,a1,0x20
[ 	]+4c:[ 	]+0006059b[ 	]+sext.w[ 	]+a1,a2
[ 	]+50:[ 	]+0ff57513[ 	]+zext.b[ 	]+a0,a0
[ 	]+54:[ 	]+1542[ 	]+sll[ 	]+a0,a0,0x30
[ 	]+56:[ 	]+9141[ 	]+srl[ 	]+a0,a0,0x30
[ 	]+58:[ 	]+1562[ 	]+sll[ 	]+a0,a0,0x38
[ 	]+5a:[ 	]+9561[ 	]+sra[ 	]+a0,a0,0x38
[ 	]+5c:[ 	]+1542[ 	]+sll[ 	]+a0,a0,0x30
[ 	]+5e:[ 	]+9541[ 	]+sra[ 	]+a0,a0,0x30
[ 	]+60:[ 	]+0ff67593[ 	]+zext.b[ 	]+a1,a2
[ 	]+64:[ 	]+03061593[ 	]+sll[ 	]+a1,a2,0x30
[ 	]+68:[ 	]+91c1[ 	]+srl[ 	]+a1,a1,0x30
[ 	]+6a:[ 	]+03861593[ 	]+sll[ 	]+a1,a2,0x38
[ 	]+6e:[ 	]+95e1[ 	]+sra[ 	]+a1,a1,0x38
[ 	]+70:[ 	]+03061593[ 	]+sll[ 	]+a1,a2,0x30
[ 	]+74:[ 	]+95c1[ 	]+sra[ 	]+a1,a1,0x30
[ 	]+76:[ 	]+1502[ 	]+sll[ 	]+a0,a0,0x20
[ 	]+78:[ 	]+9101[ 	]+srl[ 	]+a0,a0,0x20
[ 	]+7a:[ 	]+2501[ 	]+sext.w[ 	]+a0,a0
[ 	]+7c:[ 	]+02061593[ 	]+sll[ 	]+a1,a2,0x20
[ 	]+80:[ 	]+9181[ 	]+srl[ 	]+a1,a1,0x20
[ 	]+82:[ 	]+0006059b[ 	]+sext.w[ 	]+a1,a2
#...
