# Makefile for hidepid_kmod - 配合 ddk 使用
# ddk 会设置 KDIR、ARCH、LLVM 等环境变量

obj-m := hidepid_kmod.o

ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable

# 静默模式: 关闭所有 dmesg 日志（生产环境推荐）
# 方式1: 取消下行注释（永久启用）
#ccflags-y += -DHIDE_DEBUG=0
# 方式2: 环境变量 HIDE_STEALTH=1（本地 make）
ifdef HIDE_STEALTH
ccflags-y += -DHIDE_DEBUG=0
endif
# 方式3: 存在 .stealth 文件时启用（兼容 ddk Docker 模式）
ifneq ($(wildcard .stealth),)
ccflags-y += -DHIDE_DEBUG=0
endif

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))

all:
	make -C $(KDIR) M=$(MDIR) modules

clean:
	make -C $(KDIR) M=$(MDIR) clean
