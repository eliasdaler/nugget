TARGET = coroutine-demo
TYPE = ps-exe

SRCS = \
coroutine-demo.cpp \

ifeq ($(TEST),true)
CPPFLAGS = -Werror
endif
CXXFLAGS = -std=c++2b -fcoroutines

include ../../psyqo.mk
