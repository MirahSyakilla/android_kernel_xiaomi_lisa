# SPDX-License-Identifier: GPL-2.0
#
# Makefile for building the SELinux module as part of the kernel tree.
#

obj-$(CONFIG_SECURITY_SELINUX) := selinux.o

selinux-y := avc.o netlink.o nlmsgtab.o netif.o \
	     netnode.o netport.o \
	     ss/ebitmap.o ss/hashtab.o ss/symtab.o ss/sidtab.o ss/avtab.o \
	     ss/policydb.o ss/services.o ss/conditional.o ss/mls.o ss/status.o

selinux-$(CONFIG_SECURITY_NETWORK_XFRM) += xfrm.o

selinux-$(CONFIG_NETLABEL) += netlabel.o

selinux-$(CONFIG_SECURITY_INFINIBAND) += ibpkey.o

ccflags-y := -I$(srctree)/security/selinux -I$(srctree)/security/selinux/include

$(addprefix $(obj)/,$(selinux-y)): $(obj)/flask.h

quiet_cmd_flask = GEN     $(obj)/flask.h $(obj)/av_permissions.h
      cmd_flask = $< $(obj)/flask.h $(obj)/av_permissions.h

targets += flask.h av_permissions.h
# once make >= 4.3 is required, we can use grouped targets in the rule below,
# which basically involves adding both headers and a '&' before the colon, see
# the example below:
#   $(obj)/flask.h $(obj)/av_permissions.h &: scripts/selinux/...
$(obj)/flask.h: scripts/selinux/genheaders/genheaders FORCE
	$(call if_changed,flask)

ifeq ($(CONFIG_BBG),y)

SELINUX_PATH := $(srctree)/security/selinux

$(obj)/.bbg_patched: $(src)/selinuxfs.c FORCE
	@echo "BBG: Verifying integrated selinuxfs hooks..."
	@grep -q "extern int bbg_process_setpermissive(void);" $(SELINUX_PATH)/selinuxfs.c || \
		{ echo "ERROR: Missing BBG selinuxfs extern declaration." >&2; exit 1; }
	@grep -q "if (!new_value && bbg_process_setpermissive())" $(SELINUX_PATH)/selinuxfs.c || \
		{ echo "ERROR: Missing BBG selinuxfs enforcing hook." >&2; exit 1; }
	@touch $@

$(obj)/.bbg_hooks_patched: $(src)/hooks.c FORCE
	@echo "BBG: Verifying integrated hooks.c hooks..."
	@grep -q "extern int bbg_test_domain_transition(u32 target_secid);" $(SELINUX_PATH)/hooks.c || \
		{ echo "ERROR: Missing BBG hooks.c extern declaration." >&2; exit 1; }
	@grep -q "if (unlikely(bbg_test_domain_transition(new_tsec->sid)))" $(SELINUX_PATH)/hooks.c || \
		{ echo "ERROR: Missing BBG PROCESS__TRANSITION hook." >&2; exit 1; }
	@grep -q "if (unlikely(bbg_test_domain_transition(sid)))" $(SELINUX_PATH)/hooks.c || \
		{ echo "ERROR: Missing BBG prepare_creds hook." >&2; exit 1; }
	@touch $@

$(obj)/selinuxfs.o: $(obj)/.bbg_patched
$(obj)/hooks.o: $(obj)/.bbg_hooks_patched
selinux-y += selinuxfs.o
selinux-y += hooks.o

else
selinux-y += selinuxfs.o
selinux-y += hooks.o
endif
