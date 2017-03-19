

all:
	gcc -o vcapture  v4l2cam.c
	gcc -o vplay v4l2cam.c
clean:
	rm -rf *.o vcaputre vplay
