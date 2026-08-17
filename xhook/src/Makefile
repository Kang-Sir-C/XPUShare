# ============================================================================
# XPUShare Multi-Backend Makefile
# Usage:
#   make                     # default: BACKEND=cuda
#   make BACKEND=cuda        # CUDA backend (Iluvatar CoreX / BiRen / NVIDIA)
#   make BACKEND=musa        # MUSA backend (Moore Threads)
#   make BACKEND=hip         # HIP backend  (Hygon DCU)
#   make BACKEND=ascend      # AscendCL backend (Huawei Ascend) — memory only
#   make clean
# ============================================================================

BACKEND ?= cuda

SRC_DIR  := $(shell pwd)
CORE_DIR := $(SRC_DIR)/core
BACK_DIR := $(SRC_DIR)/backends/$(BACKEND)
PREFIX   ?= $(SRC_DIR)/..

# ============================================================================
# Common flags
# ============================================================================
LDFLAGS  += -ldl -lrt
CXXFLAGS_COMMON := -std=c++11 -fPIC
CFLAGS_COMMON   := -fPIC

ifeq ($(DEBUG),1)
    CXXFLAGS_COMMON += -g -D_DEBUG -Wall -Wno-tautological-pointer-compare
    CFLAGS_COMMON   += -g -D_DEBUG
else
    CXXFLAGS_COMMON += -O2
    CFLAGS_COMMON   += -O2
endif

# Include paths: core headers are always needed
CORE_INC := -I$(CORE_DIR)

# ============================================================================
# Core objects (backend-agnostic, always compiled)
# ============================================================================
CORE_OBJS := $(CORE_DIR)/debug.o \
             $(CORE_DIR)/comm.o \
             $(CORE_DIR)/predictor.o

# Scheduler and pod-manager are standalone executables, compiled from core
SCHD_OBJS := $(CORE_DIR)/scheduler.o \
             $(CORE_DIR)/schd-priority.o \
             $(CORE_DIR)/debug.o \
             $(CORE_DIR)/comm.o

PMGR_OBJS := $(CORE_DIR)/pod-manager.o \
             $(CORE_DIR)/debug.o \
             $(CORE_DIR)/comm.o

# ============================================================================
# Backend-specific configuration
# ============================================================================

# ---------- CUDA (Iluvatar CoreX / BiRen / NVIDIA) ----------
ifeq ($(BACKEND),cuda)
    CUDA_PATH ?= /usr/local/corex-4.3.6
    CXX  := $(CUDA_PATH)/bin/clang++
    NVCC := $(CUDA_PATH)/bin/clang++
    CC   := gcc

    GENCODE_FLAGS += --cuda-gpu-arch=ivcore11

    CUDA_LDFLAGS := -L$(CUDA_PATH)/lib64 \
                    -lcudart -lcuda -lixthunk \
                    -Wl,-rpath,$(CUDA_PATH)/lib64

    CXXFLAGS := $(CXXFLAGS_COMMON) -I$(CUDA_PATH)/include $(CORE_INC)
    CFLAGS   := $(CFLAGS_COMMON)
    SHIM_INC := -I$(CUDA_PATH)/include

    BACKEND_OBJS := $(BACK_DIR)/hook.o $(BACK_DIR)/shim.o
    BACKEND_LDFLAGS := $(CUDA_LDFLAGS)

    # CUDA hook.o needs special NVCC compilation
    $(BACK_DIR)/hook.o: $(BACK_DIR)/hook.cpp $(BACK_DIR)/hook.h \
                        $(CORE_DIR)/debug.h $(CORE_DIR)/comm.h \
                        $(CORE_DIR)/predictor.h $(CORE_DIR)/util.h
	$(NVCC) -m64 -x ivcore $(CXXFLAGS) $(GENCODE_FLAGS) -I$(BACK_DIR) -o $@ -c $<

    $(BACK_DIR)/shim.o: $(BACK_DIR)/shim.c
	$(CC) $(SHIM_INC) $(CFLAGS) -o $@ -c $<

    LINK_SO := $(NVCC) -shared -m64 $(GENCODE_FLAGS) \
               -Wl,-rpath,$(CUDA_PATH)/lib64:-Wl,-rpath,/xpushare/library/:-Wl,-rpath,/usr/lib/aarch64-linux-gnu/:-Wl,-rpath,/usr/lib64/

# ---------- MUSA (Moore Threads 摩尔线程) ----------
else ifeq ($(BACKEND),musa)
    MUSA_PATH ?= /usr/local/musa
    CXX  := mcc
    CC   := gcc

    CXXFLAGS := $(CXXFLAGS_COMMON) -I$(MUSA_PATH)/include $(CORE_INC)
    CFLAGS   := $(CFLAGS_COMMON)
    SHIM_INC := -I$(MUSA_PATH)/include

    MUSA_LDFLAGS := -L$(MUSA_PATH)/lib \
                    -lmusart -lmusa \
                    -Wl,-rpath,$(MUSA_PATH)/lib

    BACKEND_OBJS := $(BACK_DIR)/hook_musa.o $(BACK_DIR)/shim_musa.o
    BACKEND_LDFLAGS := $(MUSA_LDFLAGS)

    $(BACK_DIR)/hook_musa.o: $(BACK_DIR)/hook_musa.cpp $(BACK_DIR)/hook_musa.h \
                             $(CORE_DIR)/debug.h $(CORE_DIR)/comm.h \
                             $(CORE_DIR)/predictor.h $(CORE_DIR)/util.h
	$(CXX) $(CXXFLAGS) -I$(BACK_DIR) -o $@ -c $<

    $(BACK_DIR)/shim_musa.o: $(BACK_DIR)/shim_musa.c
	$(CC) $(SHIM_INC) $(CFLAGS) -o $@ -c $<

    LINK_SO := $(CXX) -shared \
               -Wl,-rpath,$(MUSA_PATH)/lib:-Wl,-rpath,/xpushare/library/

# ---------- HIP (Hygon DCU 海光) ----------
else ifeq ($(BACKEND),hip)
    HIP_PATH ?= /opt/dtk
    CXX  := hipcc
    CC   := gcc

    CXXFLAGS := $(CXXFLAGS_COMMON) -I$(HIP_PATH)/include $(CORE_INC)
    CFLAGS   := $(CFLAGS_COMMON)
    SHIM_INC := -I$(HIP_PATH)/include

    HIP_LDFLAGS := -L$(HIP_PATH)/lib \
                   -lamdhip64 \
                   -Wl,-rpath,$(HIP_PATH)/lib

    BACKEND_OBJS := $(BACK_DIR)/hook_hip.o $(BACK_DIR)/shim_hip.o
    BACKEND_LDFLAGS := $(HIP_LDFLAGS)

    $(BACK_DIR)/hook_hip.o: $(BACK_DIR)/hook_hip.cpp $(BACK_DIR)/hook_hip.h \
                            $(CORE_DIR)/debug.h $(CORE_DIR)/comm.h \
                            $(CORE_DIR)/predictor.h $(CORE_DIR)/util.h
	$(CXX) $(CXXFLAGS) -I$(BACK_DIR) -o $@ -c $<

    $(BACK_DIR)/shim_hip.o: $(BACK_DIR)/shim_hip.c
	$(CC) $(SHIM_INC) $(CFLAGS) -o $@ -c $<

    LINK_SO := $(CXX) -shared \
               -Wl,-rpath,$(HIP_PATH)/lib:-Wl,-rpath,/xpushare/library/

# ---------- Ascend (Huawei 华为昇腾) ----------
else ifeq ($(BACKEND),ascend)
    ASCEND_PATH ?= /usr/local/Ascend/ascend-toolkit/latest
    CXX  := g++
    CC   := gcc

    CXXFLAGS := $(CXXFLAGS_COMMON) -I$(ASCEND_PATH)/include $(CORE_INC)
    CFLAGS   := $(CFLAGS_COMMON)
    SHIM_INC := -I$(ASCEND_PATH)/include

    ASCEND_LDFLAGS := -L$(ASCEND_PATH)/lib64 \
                      -lascendcl \
                      -Wl,-rpath,$(ASCEND_PATH)/lib64

    # Ascend has no hook.cpp — memory isolation only via shim
    BACKEND_OBJS := $(BACK_DIR)/shim_ascend.o
    BACKEND_LDFLAGS := $(ASCEND_LDFLAGS)

    $(BACK_DIR)/shim_ascend.o: $(BACK_DIR)/shim_ascend.c
	$(CC) $(SHIM_INC) $(CFLAGS) -o $@ -c $<

    LINK_SO := $(CXX) -shared \
               -Wl,-rpath,$(ASCEND_PATH)/lib64:-Wl,-rpath,/xpushare/library/

else
    $(error Unknown BACKEND=$(BACKEND). Supported: cuda, musa, hip, ascend)
endif

# ============================================================================
# For scheduler/pod-manager, always use system g++ — these are pure C++ with
# no GPU SDK dependency.  Using the backend CXX (e.g. CoreX clang++) would
# fail on machines without the GPU SDK installed.
# ============================================================================
CORE_CXX := g++

# ============================================================================
# Targets
# ============================================================================
all: libxhook.so.1 xhook-schd xhook-pmgr

# --- Core object rules ---
$(CORE_DIR)/debug.o: $(CORE_DIR)/debug.cpp $(CORE_DIR)/debug.h
	$(CORE_CXX) $(CXXFLAGS) -o $@ -c $<

$(CORE_DIR)/comm.o: $(CORE_DIR)/comm.cpp $(CORE_DIR)/comm.h $(CORE_DIR)/debug.h
	$(CORE_CXX) $(CXXFLAGS) -o $@ -c $<

$(CORE_DIR)/predictor.o: $(CORE_DIR)/predictor.cpp $(CORE_DIR)/predictor.h $(CORE_DIR)/debug.h
	$(CORE_CXX) $(CXXFLAGS) -o $@ -c $<

$(CORE_DIR)/scheduler.o: $(CORE_DIR)/scheduler.cpp $(CORE_DIR)/scheduler.h \
                         $(CORE_DIR)/debug.h $(CORE_DIR)/comm.h $(CORE_DIR)/util.h
	$(CORE_CXX) $(CXXFLAGS) -o $@ -c $<

$(CORE_DIR)/schd-priority.o: $(CORE_DIR)/schd-priority.cpp $(CORE_DIR)/scheduler.h
	$(CORE_CXX) $(CXXFLAGS) -o $@ -c $<

$(CORE_DIR)/pod-manager.o: $(CORE_DIR)/pod-manager.cpp \
                           $(CORE_DIR)/debug.h $(CORE_DIR)/comm.h $(CORE_DIR)/util.h
	$(CORE_CXX) $(CXXFLAGS) -o $@ -c $<

# --- Shared library (backend-specific + core) ---
libxhook.so.1: $(BACKEND_OBJS) $(CORE_OBJS)
	$(LINK_SO) -o $@ $+ $(BACKEND_LDFLAGS) $(LDFLAGS)
	mkdir -p $(PREFIX)/lib
	cp $@ $(PREFIX)/lib

# --- Scheduler executable ---
xhook-schd: $(SCHD_OBJS)
	$(CORE_CXX) $(LDFLAGS) -pthread -rdynamic $+ -o $@
	mkdir -p $(PREFIX)/bin
	cp $@ $(PREFIX)/bin

# --- Pod manager executable ---
xhook-pmgr: $(PMGR_OBJS)
	$(CORE_CXX) $(LDFLAGS) -pthread -rdynamic $+ -o $@
	mkdir -p $(PREFIX)/bin
	cp $@ $(PREFIX)/bin

# --- Clean ---
clean:
	rm -f $(CORE_DIR)/*.o
	rm -f $(SRC_DIR)/backends/cuda/*.o
	rm -f $(SRC_DIR)/backends/musa/*.o
	rm -f $(SRC_DIR)/backends/hip/*.o
	rm -f $(SRC_DIR)/backends/ascend/*.o
	rm -f libxhook.so.1 xhook-schd xhook-pmgr

.PHONY: all clean
