ifeq ($(NDI_ARCH),)
  CC       ?= gcc
  NDI_ARCH := $(shell perl -e '@_=split/-/,"$(shell $(CC) -dumpmachine)"; $$"="-"; if (scalar @_ > 3) { print "@_[0,2,3]" } else { print "@_" }')
endif

CXX      ?= g++
CXXFLAGS := -std=c++17 -pthread -Wall -Wextra
CXXFLAGS += -I./NDI\ SDK\ for\ Linux/include
CXXFLAGS += -I/usr/include/otcsdk
CXXFLAGS += $(shell pkg-config --cflags sdl2 2>/dev/null || true)

LDFLAGS  := -L./NDI\ SDK\ for\ Linux/lib/$(NDI_ARCH)
LDFLAGS  += -Wl,-rpath='$$ORIGIN/NDI SDK for Linux/lib/$(NDI_ARCH)'
LDFLAGS  += -Wl,--allow-shlib-undefined -Wl,--as-needed

LDLIBS   := -lndi -ldl -lotcsdk -lSoSCorrectionLib
LDLIBS   += $(shell pkg-config --libs sdl2 2>/dev/null || true)

TARGET   := optris2ndi
SRC      := optris2ndi.cpp

INTERACTIVE_TARGET := optris2ndi_interactive
INTERACTIVE_SRC    := optris2ndi_interactive.cpp doc/examples/cpp/simpleView/src/display/opengl/Obvious2D.cpp
OBVIOUS2D_CXXFLAGS  := -I./doc/examples/cpp/simpleView/src
OBVIOUS2D_CXXFLAGS  += $(shell pkg-config --cflags glut glew 2>/dev/null || true)
OBVIOUS2D_LIBS      := $(shell pkg-config --libs glut glew 2>/dev/null || echo "-lglut -lGLEW -lGL -lX11 -lGLU")

.PHONY: all clean interactive run-interactive

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(INTERACTIVE_TARGET): $(INTERACTIVE_SRC)
	$(CXX) $(CXXFLAGS) $(OBVIOUS2D_CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) $(OBVIOUS2D_LIBS)

clean:
	rm -f $(TARGET) $(INTERACTIVE_TARGET)

.PHONY: run
run: $(TARGET)
	./$(TARGET)

run-interactive: $(INTERACTIVE_TARGET)
	./$(INTERACTIVE_TARGET)

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all      - Build optris2ndi application"
	@echo "  interactive - Build optris2ndi_interactive application"
	@echo "  clean    - Remove built executable"
	@echo "  run      - Build and run the application"
	@echo "  run-interactive - Build and run the interactive application"
	@echo "  help     - Show this help message"
