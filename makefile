TERM_EXEC = cat main.c
COMP_FLAGS = 
ifeq ($(OS), Winows_NT)
	COMP_FLAGS += -s -lopengl32 -lgdi32
else
	UNAME_SYS := $(shell uname -s)
	ifeq ($(UNAME_SYS), Darwin)
		COMP_FLAGS += -framework OpenGL -framework Cocoa
	else
		COMP_FLAGS += -s `pkg-config --cflags --libs glu gl` -lX11
	endif
endif

make:
	make clean
	gcc -o main -Itigr $(COMP_FLAGS) tigr/tigr.c main.c
	./main $(TERM_EXEC)

debug:
	make clean
	gcc -o main -g -fsanitize=address -Itigr $(COMP_FLAGS) tigr/tigr.c main.c
	./main $(TERM_EXEC)

clean:
	clear
	rm -f main
