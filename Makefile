# hyprfling -- throw floating windows around.
#
# Plugins share C++ objects with the compositor, so this has to be built with
# the same compiler against the exact Hyprland headers that will load it.
# Rebuild after every Hyprland update.

# Hyprland can pull in Lua 5.4 through a dependency; link 5.5 explicitly so the
# plugin's Lua calls bind to the interpreter the config actually runs in.
# Distros name its pkg-config file differently.
LUA := $(firstword $(foreach p,lua5.5 lua55 lua-5.5,$(if $(shell pkg-config --exists $(p) && echo y),$(p))))
ifeq ($(LUA),)
$(error Lua 5.5 not found by pkg-config (tried lua5.5, lua55, lua-5.5))
endif

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++2c -shared -fPIC -Wall -Wextra -Wno-unused-parameter \
            $(shell pkg-config --cflags hyprland pixman-1 libdrm $(LUA))
LDLIBS   += $(shell pkg-config --libs $(LUA))

hyprfling.so: main.cpp
	$(CXX) $(CXXFLAGS) $< $(LDLIBS) -o $@

# dlclose() can't unmap a library that has STB_GNU_UNIQUE symbols (C++ inline
# variables -- every Hyprland global is one), so unloading and reloading the
# same path just re-runs the old image. Each load goes through a fresh copy.
STAGE := $(or $(XDG_RUNTIME_DIR),/tmp)/hyprfling

load: hyprfling.so
	@mkdir -p $(STAGE)
	-@[ -f .loaded ] && hyprctl plugin unload "$$(cat .loaded)"
	cp hyprfling.so $(STAGE)/hyprfling-$$(date +%s%N).so
	ls -t $(STAGE)/hyprfling-*.so | head -1 > .loaded
	hyprctl plugin load "$$(cat .loaded)"

unload:
	-@[ -f .loaded ] && hyprctl plugin unload "$$(cat .loaded)" && rm .loaded

clean:
	rm -f hyprfling.so .loaded

.PHONY: load unload clean
