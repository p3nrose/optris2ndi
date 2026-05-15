ifeq ($(NDI_ARCH),)
  CC       ?= gcc
  NDI_ARCH := $(shell perl -e '@_=split/-/,"$(shell $(CC) -dumpmachine)"; $$"="-"; if (scalar @_ > 3) { print "@_[0,2,3]" } else { print "@_" }')
endif

CXX      ?= g++
CXXFLAGS := -std=c++17 -pthread -Wall -Wextra
CXXFLAGS += -I./NDI\ SDK\ for\ Linux/include
CXXFLAGS += -I/usr/include/otcsdk

LDFLAGS  := -L./NDI\ SDK\ for\ Linux/lib/$(NDI_ARCH)
LDFLAGS  += -Wl,-rpath='$$ORIGIN/NDI SDK for Linux/lib/$(NDI_ARCH)'
LDFLAGS  += -Wl,--allow-shlib-undefined -Wl,--as-needed

LDLIBS   := -lndi -ldl -lotcsdk -lSoSCorrectionLib

TARGET   := optris2ndi
SRC      := optris2ndi.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: run
run: $(TARGET)
	./$(TARGET)

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all      - Build optris2ndi application"
	@echo "  clean    - Remove built executable"
	@echo "  run      - Build and run the application"
	@echo "  help     - Show this help message"
