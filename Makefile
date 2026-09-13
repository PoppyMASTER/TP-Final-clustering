CC=gcc
CFLAGS=-Wall -Wextra -g
LIBS_GUI=-lSDL2 -lSDL2_ttf -lpthread
LIBS_CLI=-lreadline

all: minihv_cli minihv_gui minihv_fm mini_os.bin mini_os_shell.bin
	@echo ""
	@echo "  MiniHV compile — utilisez : make run-cli | run-gui | run-term | run-fm"
	@echo ""

minihv_cli: minihv_j2.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_CLI)

minihv_gui: minihv_gui.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_GUI)

minihv_fm: minihv_fm.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS_GUI)

mini_os.bin: mini_os_v2.asm
	nasm -f bin -o $@ $<

mini_os_shell.bin: mini_os_shell.asm
	nasm -f bin -o $@ $<

run-cli: minihv_cli mini_os.bin
	sudo ./minihv_cli mini_os.bin

run-gui: minihv_gui mini_os_shell.bin
	sudo SDL_VIDEODRIVER=wayland XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ./minihv_gui --gui mini_os_shell.bin

run-term: minihv_gui mini_os_shell.bin
	sudo SDL_VIDEODRIVER=wayland XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ./minihv_gui --term mini_os_shell.bin

run-fm: minihv_fm mini_os_shell.bin
	sudo SDL_VIDEODRIVER=wayland XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ./minihv_fm mini_os_shell.bin

clean:
	rm -f minihv_cli minihv_gui minihv_fm *.bin kvm.log

.PHONY: all clean run-cli run-gui run-term run-fm
