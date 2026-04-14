# Tests build — requires gtest (pkg-config gtest_main)
# Usage: make && make run

DIR    := $(shell cd .. && pwd)
SRC    := $(DIR)/src
EIGEN  := $(DIR)/third_party/eigen

OPENCV_PKG := $(shell pkg-config --exists opencv4 && echo opencv4 || echo opencv)

CFLAGS := -std=c++17 -O0 -g \
           -I$(SRC) -I$(EIGEN) \
           -DEIGEN_NO_DEBUG \
           $(shell pkg-config --cflags x11 xtst xext $(OPENCV_PKG) ncursesw 2>/dev/null)

LDFLAGS := $(shell pkg-config --libs x11 xtst xext $(OPENCV_PKG) ncursesw 2>/dev/null) \
            -lpthread \
            $(shell pkg-config --libs gtest_main)

# Source files needed for BT tests (minimal subset — no Brain/UI)
BT_SRCS := bt_test.cpp \
            $(SRC)/BehaviorTree.cpp \
            $(SRC)/Eyes.cpp \
            $(SRC)/Utils.cpp \
            $(SRC)/Input.cpp \
            $(SRC)/Window.cpp \
            $(SRC)/Window_Linux.cpp \
            $(SRC)/Capture.cpp \
            $(SRC)/Capture_Linux.cpp \
            $(SRC)/Intercept.cpp \
            $(SRC)/Intercept_Linux.cpp \
            $(SRC)/Config.cpp \
            $(SRC)/Stats.cpp \
            $(SRC)/RandomDelay.cpp

MEMORY_SRCS := memory_test.cpp

.PHONY: all run clean

all: bt_test memory_test

bt_test: $(BT_SRCS)
	g++ $(CFLAGS) $^ -o $@ $(LDFLAGS)

memory_test: $(MEMORY_SRCS)
	g++ -std=c++17 -O0 -g $^ -o $@ $(shell pkg-config --libs gtest_main)

run: all
	./bt_test --gtest_color=yes
	./memory_test --gtest_color=yes

clean:
	rm -f bt_test memory_test
