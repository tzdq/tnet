# 不编译的文件夹
SUBDIRS_EXCLUDE = 
# 优先编译的文件夹
HIGH_PRIORITY_DIR = 

#============= SUB MODULE===============
# 遍历所有子文件夹名，并去除不需要编译的和高优先级的
SUBDIRS_ALL := $(sort $(subst /,,$(dir $(wildcard */*))))
SUBDIRS = $(filter-out $(SUBDIRS_EXCLUDE) $(HIGH_PRIORITY_DIR), $(SUBDIRS_ALL))

# 防止make 和 clean 有名字冲突 高优先级的先编译
SRC_MODULES =$(HIGH_PRIORITY) $(SUBDIRS)
CLEAN_MODULES = $(addprefix _clean_,$(SRC_MODULES))

# 源文件和OBJ文件
SRC_FILES =  $(foreach dir, $(SRC_MODULES), $(wildcard $(dir)/*.c))
SRC_FILES += $(wildcard *.c)
OBJ_FILES = $(SRC_FILES:.c=.o)

# 目标定义
TARGETNAME = libtnet.a

# 编译选项参数
CC = gcc
CFLAGS = -Wall -O3 -g -fPIC -fpermissive
CXX = g++
CXXFLAGS = -Wall -O3 -g -fPIC -fpermissive
RM = rm -rvf
CP = cp -fr
AR = ar cru
ARFLAGS =
LDFLAGS = 
RANLIB = ranlib

# 编译需要包含的头文件
INCLUDE = -I . 
INCLUDE += $(foreach dir,$(SRC_MODULES),-I $(dir))

# 编译需要依赖的库
LIBS += -lpthread

# 需要拷贝到系统include的头文件目录
USER_NEED_COPY_HEADER_FILES = event2/

# 系统库和include目录
SYS_LIB_PATH = /usr/local/lib
SYS_INCLUDE_PATH = /usr/local/include

# 用户头文件在include和lib中的全路径
USER_INCLUDE_PATH = $(SYS_INCLUDE_PATH)/$(USER_NEED_COPY_HEADER_FILES)
USER_LIB_PATH = $(SYS_LIB_PATH)/$(TARGETNAME)

# 终极目标
.PHONY:all install uninstall clean cleanall 


all:PRE_MAKE $(TARGETNAME)
	$(CP) $(USER_NEED_COPY_HEADER_FILES) $(SYS_INCLUDE_PATH)
	$(CP) $(TARGETNAME) $(SYS_LIB_PATH)	

install:
	$(CP) $(USER_NEED_COPY_HEADER_FILES) $(SYS_INCLUDE_PATH)
	$(CP) $(TARGETNAME) $(SYS_LIB_PATH)
	
uninstall:
	$(RM) $(USER_INCLUDE_PATH) $(USER_LIB_PATH)

clean:PRE_CLEAN
	$(RM) $(OBJ_FILES)

cleanall:PRE_CLEAN
	$(RM) $(OBJ_FILES)
	$(RM) $(TARGETNAME)

PRE_CLEAN:
	@echo "Removing linked and complied files ...."
	@date;

PRE_MAKE:
	@echo "compling and linking files ...."
	@date;

$(OBJ_FILES):$(SRC_FILES)
ifneq ($(SRC_C_FILES),)
	$(CC) $(CFLAGS) $(INCLUDE) -c $^ $(LIBS)
else
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $^ $(LIBS)
endif

$(TARGETNAME):$(OBJ_FILES)
	$(AR) $@ $^
	$(RANLIB) $(TARGETNAME)
