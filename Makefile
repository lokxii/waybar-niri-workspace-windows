all:
	gcc -shared -fPIC \
		-g \
		main.c \
		cJSON.c \
		-o workspace_windows.so \
		`pkg-config --cflags --libs gtk+-3.0`
