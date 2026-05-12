CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -g -fno-omit-frame-pointer

TARGET = my_ice
SRCS = main.c ice_vfio.c ice_pci.c ice_dma.c ice_adminq.c ice_controlq.c ice_lanq.c ice_utils.c
HDRS = ice_vfio.h ice_regs.h ice_types.h ice_pci.h ice_dma.h ice_adminq.h ice_controlq.h ice_lanq.h ice_utils.h ice_min.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
