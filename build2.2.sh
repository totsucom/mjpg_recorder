gcc -c mjpg_recorder2.2.c
gcc -c image.c
gcc -c jpeg.c
gcc -c bitmapfont.c
gcc -c mem.c
gcc -c mytime.c
gcc mjpg_recorder2.2.o image.o jpeg.o bitmapfont.o mem.o mytime.o -o ../mjpg_recorder2.2 -ljpeg -pthread
