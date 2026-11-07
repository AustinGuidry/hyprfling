# hyprfling -- throw floating windows around.
#
# Plugins share C++ objects with the compositor, so this has to be built with
# the same compiler against the exact Hyprland headers that will load it.
# Rebuild after every Hyprland update.

# Link the Lua that Hyprland itself links (its direct NEEDED entry), so the
# plugin's Lua calls bind to the interpreter the config actually runs in. Other
# libraries can pull in a second Lua, so ldd isn't used. Arch builds against
# 5.5 (liblua.so.5.5), Gentoo/Debian against 5.4 (liblua5.4.so.0); 5.5 is the
# fallback when the binary can't be inspected. Distros name the .pc differently.
HYPRLAND_BIN := $(or $(shell command -v Hyprland),$(shell command -v hyprland))
LUA_VER := $(shell readelf -d $(HYPRLAND_BIN) 2>/dev/null | \
             sed -nE 's/.*\[liblua-?([0-9]\.[0-9])?\.so(\.([0-9]\.[0-9]))?.*/\1\3/p' | head -1)
LUA_VER := $(or $(LUA_VER),5.5)
LUA_NODOT := $(subst .,,$(LUA_VER))
LUA := $(firstword $(foreach p,lua$(LUA_VER) lua$(LUA_NODOT) lua-$(LUA_VER),$(if $(shell pkg-config --exists $(p) && echo y),$(p))))
ifeq ($(LUA),)
$(error Lua $(LUA_VER) (the one Hyprland links) not found by pkg-config (tried lua$(LUA_VER), lua$(LUA_NODOT), lua-$(LUA_VER)))
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
