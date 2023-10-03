CC = gcc

ifeq ($(DEBUG),y)
CFLAGS += -D__DEBUG__ -O0 -g -ggdb
else
CFLAGS += -O3
endif

SRCS := dpdk_echo.c

PKGCONF ?= pkg-config
PC_FILE := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --cflags libdpdk)

LDFLAGS_SHARED = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --libs libdpdk) -lrte_net_mlx4 -lrte_bus_pci -lrte_bus_vdev -lpthread -lm -lstdc++

CFLAGS += -DALLOW_EXPERIMENTAL_API -lm -lstdc++

dpdk_echo: $(SRCS) Makefile $(PC_FILE)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

clean:
	rm dpdk_echo
