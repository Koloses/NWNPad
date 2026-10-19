CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
MH=third_party/minhook
CFLAGS=-O2 -I$(MH)/include -Ithird_party -Isrc
CXXFLAGS=$(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti -Wall -Wno-unused-function -Wno-unused-variable
LDFLAGS=-shared -static -static-libgcc -static-libstdc++ -s -Wl,-Map,build/version.map -Wl,--enable-stdcall-fixup
LIBS=-lopengl32 -lgdi32 -luser32 -lkernel32
SRC=$(wildcard src/*.cpp)
CSRC=src/fonts_data.c
MHSRC=$(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c $(MH)/src/hde/hde64.c
OBJ=$(patsubst src/%.cpp,build/%.o,$(SRC)) build/fonts_data.o $(patsubst $(MH)/src/%.c,build/mh_%.o,$(subst hde/,hde_,$(MHSRC)))
all: build/version.dll
build/%.o: src/%.cpp src/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build/fonts_data.o: src/fonts_data.c
	@mkdir -p build
	$(CC) -O2 -c $< -o $@
build/mh_%.o:
	@mkdir -p build
	$(CC) $(CFLAGS) -c $(MH)/src/$(subst hde_,hde/,$*).c -o $@
# signatures of the game functions used (needs the supported nwmain.exe; kept as is when it is missing)
src/sigs.inc: $(SRC) src/game.h tools_gen_sigs.py
	@if [ -f "$(GAMEEXE)" ]; then python3 tools_gen_sigs.py $(GAMEEXE) src/sigs.inc src/game.h src/*.cpp; \
	else echo "nwmain.exe not found ($(GAMEEXE)), keeping src/sigs.inc"; touch src/sigs.inc; fi
build/version.dll: $(OBJ) src/version.def
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) src/version.def $(LIBS)
clean:
	rm -rf build

release: build/version.dll
	rm -rf build/release && mkdir -p build/release
	cp build/version.dll NWNPad.ini README.md build/release/
	cp fonts/OFL.txt build/release/OFL-fonts.txt
	cd build/release && zip -q -9 ../NWNPad.zip *
GAMEEXE?=nwmain.exe

build/hooks.o: src/sigs.inc
