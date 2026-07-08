TARGET = coroutine-demo-2
TYPE = ps-exe

SRCS = \
coroutine-demo-2.cpp \

ifeq ($(TEST),true)
CPPFLAGS = -Werror
endif
CXXFLAGS = -std=c++2b -fcoroutines

include ../../psyqo.mk
