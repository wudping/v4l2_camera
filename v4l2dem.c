#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h> /* getopt_long() */
#include <fcntl.h> /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <asm/types.h> /* for videodev2.h */
#include <linux/videodev2.h>


int main(int argc, char *argv[])
{
    int fd;
    int rel;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_streamparm *setfps;
    struct v4l2_requestbuffers rb;

    fd = open ("/dev/video0", O_RDWR, 0); //以阻塞模式
    rel = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if(rel != 0)
    {
        perror("ioctl VIDIOC_QUERYCAP");
        return -1;
    }    
    
    printf("capabilities = 0x%0x \n", cap.capabilities);
		
		memset(&fmt, 0, sizeof(struct v4l2_format));
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		
		if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0)
		{
			printf("get format failed\n");
			return -1;
		}
		
		
		fmt.fmt.pix.width =  640;
		fmt.fmt.pix.height = 480;
		fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_YUYV;
		rel = ioctl(fd, VIDIOC_S_FMT, &fmt);
		if (rel < 0)
		{
				printf("\nSet format failed\n");
				return -1;
		}

		setfps = (struct v4l2_streamparm *) calloc(1, sizeof(struct v4l2_streamparm));
		memset(setfps, 0, sizeof(struct v4l2_streamparm));
		setfps->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  

    rel = ioctl(fd, VIDIOC_G_PARM, setfps);
    if(rel == 0)
    {
        printf("Frame rate:   %u/%u\n",
               setfps->parm.capture.timeperframe.denominator,
               setfps->parm.capture.timeperframe.numerator
               );
    }
    else
    {
        perror("Unable to read out current frame rate");
        return -1;
    }
		setfps->parm.capture.timeperframe.numerator=1;
		setfps->parm.capture.timeperframe.denominator= 60;
		    rel = ioctl(fd, VIDIOC_S_PARM, setfps);
		if(rel != 0)
    {
        printf("\nUnable to Set FPS");
        return -1;
    }
    memset(&rb, 0, sizeof(struct v4l2_requestbuffers));
		rb.count = 3;
		rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		rb.memory = V4L2_MEMORY_MMAP;
		rel = ioctl(fd, VIDIOC_REQBUFS, &rb);
		if (rel < 0)
		{
				printf("Unable to allocate buffers: %d.\n", errno);
				return -1;
		}

		free(setfps);
    close(fd);

    return 0;
}
