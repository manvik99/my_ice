CC ?= cc
BUILD ?= release
BASE_CFLAGS ?= -march=native -mtune=native -Wall -Wextra -std=c11 -fno-omit-frame-pointer

ifeq ($(BUILD),debug)
PROFILE_CFLAGS := -O0 -g -DMY_ICE_ENABLE_INFO=1
else ifeq ($(BUILD),release)
PROFILE_CFLAGS := -O3 -DNDEBUG -DMY_ICE_ENABLE_INFO=0
else
$(error Unsupported BUILD '$(BUILD)' (expected release or debug))
endif

CFLAGS ?=
ALL_CFLAGS = $(BASE_CFLAGS) $(PROFILE_CFLAGS) $(CFLAGS)

TARGET = my_ice
SRCS = main.c ice_vfio.c ice_pci.c ice_dma.c ice_adminq.c ice_controlq.c ice_lanq.c ice_utils.c
HDRS = ice_vfio.h ice_regs.h ice_types.h ice_pci.h ice_dma.h ice_adminq.h ice_controlq.h ice_lanq.h ice_utils.h ice_min.h
TEST_TARGET = tests/test_pkt_len

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(ALL_CFLAGS) -o $@ $(SRCS)

debug:
	$(MAKE) BUILD=debug all

release:
	$(MAKE) BUILD=release all

$(TEST_TARGET): tests/test_pkt_len.c ice_types.h ice_min.h
	$(CC) $(ALL_CFLAGS) -I. -o $@ $<

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)

.PHONY: all clean debug release test
