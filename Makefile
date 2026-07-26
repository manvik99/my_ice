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
ANALYSIS ?= 0
CALLGRIND ?= 0

ifeq ($(ANALYSIS),1)
ANALYSIS_CFLAGS := -DMY_ICE_ANALYSIS=1 -rdynamic
ANALYSIS_SRCS := ../analysis/ice_analysis_markers.c
else
ANALYSIS_CFLAGS :=
ANALYSIS_SRCS :=
endif

ifeq ($(CALLGRIND),1)
ifneq ($(ANALYSIS),1)
$(error CALLGRIND=1 requires ANALYSIS=1)
endif
ANALYSIS_CFLAGS += -DMY_ICE_ANALYSIS_CALLGRIND=1
endif

ALL_CFLAGS = $(BASE_CFLAGS) $(PROFILE_CFLAGS) $(ANALYSIS_CFLAGS) $(CFLAGS)

TARGET = my_ice
SRCS = main.c ice_vfio.c ice_pci.c ice_dma.c ice_adminq.c ice_controlq.c ice_lanq.c ice_utils.c $(ANALYSIS_SRCS)
HDRS = ice_vfio.h ice_regs.h ice_types.h ice_pci.h ice_dma.h ice_adminq.h ice_controlq.h ice_lanq.h ice_utils.h ice_min.h ice_analysis.h
TEST_TARGET = tests/test_pkt_len

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) FORCE
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

FORCE:

.PHONY: all clean debug release test FORCE
