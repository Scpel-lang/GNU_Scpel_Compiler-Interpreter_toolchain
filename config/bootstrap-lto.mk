# This option enables LTO for stage2 and stage3 in slim mode

STAGE2_CFLAGS += -flto=jobserver -frandom-seed=1
STAGE3_CFLAGS += -flto=jobserver -frandom-seed=1
STAGEprofile_CFLAGS += -flto=jobserver -frandom-seed=1
STAGEtrain_CFLAGS += -flto=jobserver -frandom-seed=1
STAGEfeedback_CFLAGS += -flto=jobserver -frandom-seed=1

# assumes the host supports the linker plugin
LTO_AR = $$r/$(HOST_SUBDIR)/prev-spl/spl-ar$(exeext) -B$$r/$(HOST_SUBDIR)/prev-spl/
LTO_RANLIB = $$r/$(HOST_SUBDIR)/prev-spl/spl-ranlib$(exeext) -B$$r/$(HOST_SUBDIR)/prev-spl/
LTO_NM = $$r/$(HOST_SUBDIR)/prev-spl/spl-nm$(exeext) -B$$r/$(HOST_SUBDIR)/prev-spl/

LTO_EXPORTS = AR="$(LTO_AR)"; export AR; \
	      RANLIB="$(LTO_RANLIB)"; export RANLIB; \
	      NM="$(LTO_NM)"; export NM;
LTO_FLAGS_TO_PASS = AR="$(LTO_AR)" RANLIB="$(LTO_RANLIB)" NM="$(LTO_NM)"

do-compare = $(SHELL) $(srcdir)/contrib/compare-lto $$f1 $$f2
extra-compare = spl/lto1$(exeext)
