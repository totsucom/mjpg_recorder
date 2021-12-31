gcc -c mjpg_recorder2.1.c
gcc -c image.c
gcc -c jpeg.c
gcc -c bitmapfont.c
gcc -c mem.c
gcc mjpg_recorder2.1.o image.o jpeg.o bitmapfont.o mem.o -o ../mjpg_recorder2.1 -ljpeg
