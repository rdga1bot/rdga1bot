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
            $(SRC)/Stats.cpp

MEMORY_SRCS := memory_test.cpp

# Shared sources for Director/Blackboard tests (no BehaviorTree, no Brain)
SUPPORT_SRCS := $(SRC)/Eyes.cpp \
                $(SRC)/Utils.cpp \
                $(SRC)/Input.cpp \
                $(SRC)/Window.cpp \
                $(SRC)/Window_Linux.cpp \
                $(SRC)/Capture.cpp \
                $(SRC)/Capture_Linux.cpp \
                $(SRC)/Intercept.cpp \
                $(SRC)/Intercept_Linux.cpp \
                $(SRC)/Config.cpp \
                $(SRC)/Stats.cpp

BB_SRCS  := test_blackboard.cpp $(SRC)/Blackboard.cpp $(SUPPORT_SRCS)
DIR_SRCS := test_director.cpp   $(SRC)/Blackboard.cpp $(SRC)/DirectorSystem.cpp $(SUPPORT_SRCS)

# ── Sanitizer flags (ASan + UBSan + libstdc++ assertions) ────────────────────
SAN_FLAGS := -O1 -g -fno-omit-frame-pointer \
             -D_GLIBCXX_ASSERTIONS \
             -fsanitize=address,undefined
SAN_ENV   := ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
             UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

.PHONY: all run run-san clean

all: bt_test memory_test test_blackboard test_director

bt_test: $(BT_SRCS)
	g++ $(CFLAGS) $^ -o $@ $(LDFLAGS)

memory_test: $(MEMORY_SRCS)
	g++ -std=c++17 -O0 -g $^ -o $@ $(shell pkg-config --libs gtest_main)

test_blackboard: $(BB_SRCS)
	g++ $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_director: $(DIR_SRCS)
	g++ $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ── Sanitizer builds (окремі бінарники *_san) ─────────────────────────────────
test_blackboard_san: $(BB_SRCS)
	g++ $(CFLAGS) $(SAN_FLAGS) $^ -o $@ $(LDFLAGS)

test_director_san: $(DIR_SRCS)
	g++ $(CFLAGS) $(SAN_FLAGS) $^ -o $@ $(LDFLAGS)

run: all
	./bt_test --gtest_color=yes
	./memory_test --gtest_color=yes
	./test_blackboard --gtest_color=yes
	./test_director --gtest_color=yes

run-san: test_blackboard_san test_director_san
	$(SAN_ENV) ./test_blackboard_san --gtest_color=yes
	$(SAN_ENV) ./test_director_san   --gtest_color=yes

clean:
	rm -f bt_test memory_test test_blackboard test_director \
	      test_blackboard_san test_director_san
