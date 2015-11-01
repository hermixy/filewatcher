
#ARCH: linux/pi/android/ios/
ARCH		?= linux
CROSS_PREFIX	?=
OUTPUT		?= /usr/local
BUILD_DIR	:= $(shell pwd)/../build/
ARCH_INC	:= $(BUILD_DIR)/$(ARCH).inc
COLOR_INC	:= $(BUILD_DIR)/color.inc

ifeq ($(ARCH_INC), $(wildcard $(ARCH_INC)))
include $(ARCH_INC)
endif

CC	= $(CROSS_PREFIX)gcc
CXX	= $(CROSS_PREFIX)g++
LD	= $(CROSS_PREFIX)ld
AR	= $(CROSS_PREFIX)ar

ifeq ($(COLOR_INC), $(wildcard $(COLOR_INC)))
include $(COLOR_INC)
else
CC_V	= $(CC)
CXX_V	= $(CXX)
LD_V	= $(LD)
AR_V	= $(AR)
CP_V	= $(CP)
RM_V	= $(RM)
endif

########
TGT_APP 	= filewatcher
OBJS_APP    	= $(TGT_APP).o

CFLAGS	:= -g -Wall -fPIC
CFLAGS	+= -Werror

LDFLAGS	:= $($(ARCH)_LDFLAGS)
LDFLAGS	+= -pthread
LDFLAGS	+= -llog
LDFLAGS	+= -ldict
LDFLAGS	+= -lgevent

.PHONY : all clean

TGT	:= $(TGT_APP)

OBJS	:= $(OBJS_APP)

all: $(TGT)

%.o:%.c
	$(CC_V) -c $(CFLAGS) $< -o $@

$(TGT_APP): $(OBJS)
	$(CC_V) -o $@ $^ $(LDFLAGS)

install:
	$(CP_V) -f $(TGT_APP) ${OUTPUT}/bin
clean:
	$(RM_V) -f $(OBJS)
	$(RM_V) -f $(TGT)

uninstall:
	$(RM_V) -f ${OUTPUT}/bin/$(TGT_APP)
