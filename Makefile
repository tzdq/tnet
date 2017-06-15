#��������ļ���
SUBDIRS_EXCLUDE =  
#���ȱ�����ļ���
HIGH_PRIORITY_DIR = 

#============= SUB MODULE===============
#�����������ļ���������ȥ������Ҫ�����
SUBDIRS_ALL := $(sort $(subst /,,$(dir $(wildcard */*))))
SUBDIRS = $(filter-out $(SUBDIRS_EXCLUDE) $(HIGH_PRIORITY_DIR), $(SUBDIRS_ALL))

#��ֹmake �� clean �����ֳ�ͻ
SRC_MODULES =$(HIGH_PRIORITY) $(SUBDIRS)
CLEAN_MODULES = $(addprefix _clean_,$(SRC_MODULES))

#=============== APP INC ================
APP_INC += $(foreach dir,$(SRC_MODULES),-I $(dir))
#APP_INC += $(foreach dir,$(EXTDIRS),-I $(dir))

#=============== SRCS ===================
TARGET_SRC =  $(foreach dir, $(SRC_MODULES), $(wildcard $(dir)/*.c))
TARGET_SRC += $(wildcard *.c)

TARGET_EXE = libtnet.a
TARGET_OBJ = $(TARGET_SRC:.c=.o)

#��������
CC = gcc
AR = ar crs
RM = /bin/rm -rvf
CFLAGS += -g -fPIC -Wall
#CFLAGS += -pipe -D_NEW_LIC -D_GNU_SOURCE -D_REENTRANT -fno-strict-aliasing
CFLAGS += $(APP_INC)
LIBS += -Wl,-Bstatic -Wl,-Bdynamic -lnsl -lpthread -ldl -lrt -lz 

.PHONY: install uninstall clean cleanall

install: PRE_MAKE $(TARGET_EXE)
	cp -fr event/ /usr/local/include/
	cp -f ${TARGET_EXE} /usr/local/lib/

uninstall:
	${RM} /usr/local/include/event /usr/local/lib/libtnet.a	

PRE_MAKE:
	@echo $(SUBDIRS_ALL)
	@echo $(TARGET_OBJ)
	@echo $(TARGET_EXE)
	@date;

$(TARGET_OBJ):$(TARGET_SRC)
	$(CC) $(CFLAGS)  -c $^ $(LIBS)

$(TARGET_EXE): $(TARGET_OBJ)
	$(AR) $@  $^
	ranlib $(TARGET_EXE)

clean: PRE_CLEAN
	$(RM) $(TARGET_OBJ)

cleanall:PRE_CLEAN
	$(RM) $(TARGET_OBJ)
	$(RM) $(TARGET_EXE)

PRE_CLEAN:
	@date;
	
