//#./vcapture -n 20 -m -C -d /dev/video0 cam_yuyv_320x240.yuv 


/* Video For Linux 2 based camera capture
 * application.
 * 
 * Author: Komal Shah <koma...@yahoo.com>
 *
 * Part of code derived from xawtv
 * License of xawtv applies. 
 */
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

/* TODO:
 *
 * -- Add direct rendering captured buffers to videoout
 * -- Add option for the above.
 * -- Add notice if driver supported preview is enabled. Drops the FPS.
 * -- Make scripts to simplify execution and corner testing
 * -- Add sensor capture format selection strings on command line
 * -- Add better parameter checking supplied by user on command line.
 */

#define DEFAULT_WIDTH 320
#define DEFAULT_HEIGHT 240
#define DEFAULT_NFRAMES 10
#define DEFAULT_SIZE (DEFAULT_WIDTH*DEFAULT_HEIGHT*2)

#define WANTED_BUFFERS 32
#define MAX_CTRL 32
#define MAX_FORMAT 32

#define ALIGN 1

#define CAN_READWRITE 		0x00000001
#define CAN_STREAM 		0x00000002
#define CAN_OVERLAY 		0x00000004

#define IO_METHOD_MMAP 		0x00000001
#define IO_METHOD_READ 		0x00000002
#define IO_METHOD_USERPTR 	0x00000004

#define CLEAR(x) memset (&(x), 0, sizeof (x))

struct app_video_format {
	unsigned int width;
	unsigned int height;
	unsigned int pixelformat;
	enum v4l2_buf_type type;
	unsigned int bytesperline;
	unsigned int memory;
};

struct app_video_buf {
    	struct app_video_format fmt;
	size_t	size;
	unsigned char *data;
};

struct v4l2_handle {
	int	fd;
	char	*device;

	struct v4l2_capability	cap;
	struct v4l2_streamparm	streamparm;
	struct v4l2_queryctrl	ctl[MAX_CTRL*2];	
	struct v4l2_fmtdesc	fmtdesc[MAX_FORMAT];	

	int	nfmts;
	int	flags;

	int	fps,first;
	long long   start;
	struct v4l2_format      fmt;

	int 	capture;
	int 	player;

	struct v4l2_requestbuffers     req;
	/* Internal v4l2_buffers */
	struct v4l2_buffer             buf_v4l2[WANTED_BUFFERS];
	/* mapped buffers in user space*/
	struct app_video_buf           buf_me[WANTED_BUFFERS];

	struct v4l2_framebuffer        ov_fb;
	struct v4l2_format             ov_win;

	int    ov_error;
	int    ov_enabled;
	int    ov_on;
	struct app_video_format *app_fmt;
};

static int (*init_userp)(struct v4l2_handle *handle, unsigned int buffer_size);
static int (*init_mmap)(struct v4l2_handle *handle, int bcount);
static int (*init_read)(struct v4l2_handle *handle, unsigned int buffer_size);
static void* (*init_device)(char *device);

struct _buffer
{
    void *start;
    int length;
};

struct _buffer *buffers = NULL;
unsigned int n_buffers = 0;
unsigned int bcount = 2;

void errno_exit(char *msg);
static int vid_capture(void *h, char *filename);
static int read_frame(struct v4l2_handle *h);

int	width = DEFAULT_WIDTH;
int	height = DEFAULT_HEIGHT;    
char	*filename = "dump";
int	nframes = 0; /*DEFAULT_NFRAMES;*/
int	open_mode = O_RDWR | O_NONBLOCK; /* O_NONBLOCK is must for camera*/
static char	*command;
static char 	*device = "/dev/v4l/video0" ; /* default */
int 	io = IO_METHOD_READ; /*default */
int	capture = 0;
int preview = 0;
int vplayer = 0;
int vcapture = 0;
int fps = 0;

FILE *outfd = NULL;
FILE *infd = NULL;

/* NOTE: Let the few common function between vplay and
 * vcapture separate...once I finish the testing with
 * all the features, I can merge them based on the 
 * difference and the amount of code duplication.
 * ---Komal Shah
 */

static void
print_bufinfo(struct v4l2_buffer *buf)
{
    static char *type[] = {
	[V4L2_BUF_TYPE_VIDEO_CAPTURE] = "video-cap",
	[V4L2_BUF_TYPE_VIDEO_OVERLAY] = "video-over",
	[V4L2_BUF_TYPE_VIDEO_OUTPUT]  = "video-out",
	[V4L2_BUF_TYPE_VBI_CAPTURE]   = "vbi-cap",
	[V4L2_BUF_TYPE_VBI_OUTPUT]    = "vbi-out",
    };

    fprintf(stderr,"v4l2: buf %d: %s 0x%x+%d, used %d\n",
	    buf->index,
	    buf->type < sizeof(type)/sizeof(char*)
	    ? type[buf->type] : "unknown",
	    buf->m.offset,buf->length,buf->bytesused);
}

int xioctl(int fd, int request, void *arg)
{
	int r;
	do r = ioctl(fd, request, arg);
	while( -1 == r && EINTR == errno);

	return r;
}

void errno_exit(char  *msg)
{
	printf("Exit : %s\n", msg);
	exit(0);
}

static int v4l2_open(void *handle)
{
	struct v4l2_handle *h = handle;

	printf("device name is %s\n", h->device);
	h->fd = open(h->device, open_mode, 0);

	if (h->fd == -1)
	    return -1;

	return 0;
}

static int v4l2_close(void *handle)
{
	struct v4l2_handle *h = handle;

	close(h->fd);
	h->fd = -1;	
}

static void
get_device_capabilities(struct v4l2_handle *h)
{
    if (xioctl(h->fd, VIDIOC_QUERYCAP, &h->cap) == -1) {
	    close(h->fd);
	    return -1;
    }

}

static void
enum_fmts(struct v4l2_handle *h)
{
    for (h->nfmts = 0; h->nfmts < MAX_FORMAT; h->nfmts++) {
	h->fmtdesc[h->nfmts].index = h->nfmts;
	h->fmtdesc[h->nfmts].type  = h->app_fmt->type;
	if (-1 == xioctl(h->fd, VIDIOC_ENUM_FMT, &h->fmtdesc[h->nfmts]))
	    break;
    }

    h->streamparm.type = h->app_fmt->type;
    ioctl(h->fd,VIDIOC_G_PARM,&h->streamparm);
}

static int
v4l2_setformat(void *handle)
{
	unsigned int min;
	int ret;

	struct v4l2_handle *h = handle;

	h->fmt.type = h->app_fmt->type;
	h->fmt.fmt.pix.width = h->app_fmt->width;
	h->fmt.fmt.pix.height= h->app_fmt->height;
	h->fmt.fmt.pix.pixelformat = h->app_fmt->pixelformat;
    	h->fmt.fmt.pix.field  = V4L2_FIELD_NONE;

	ret = xioctl(h->fd, VIDIOC_S_FMT, &h->fmt);
	if (ret < 0)
	{
	    fprintf(stderr, "ioctl VIDIOC_S_FMT failed: %s", strerror(errno));
	    return -1;
	}

	if (h->fmt.fmt.pix.pixelformat != h->app_fmt->pixelformat)
	    return -1;

	h->app_fmt->width        = h->fmt.fmt.pix.width;
	h->app_fmt->height       = h->fmt.fmt.pix.height;
	h->app_fmt->bytesperline = h->fmt.fmt.pix.bytesperline;

	min = h->fmt.fmt.pix.width*2;
	if(h->fmt.fmt.pix.bytesperline < min)
	    h->fmt.fmt.pix.bytesperline = min;
	min = h->fmt.fmt.pix.bytesperline*h->fmt.fmt.pix.height;
	if(h->fmt.fmt.pix.sizeimage < min)
	    h->fmt.fmt.pix.sizeimage = min;

	fprintf(stderr,"v4l2: new params (%dx%d, %c%c%c%c, %d byte)\n",
		h->app_fmt->width,h->app_fmt->height,
		h->fmt.fmt.pix.pixelformat & 0xff,
		(h->fmt.fmt.pix.pixelformat >>  8) & 0xff,
		(h->fmt.fmt.pix.pixelformat >> 16) & 0xff,
		(h->fmt.fmt.pix.pixelformat >> 24) & 0xff,
		h->fmt.fmt.pix.sizeimage);

	return 0;
}

void* vcapture_init_device(char *device)
{
	struct v4l2_capability  vcap;
	struct v4l2_format      fmt;
	int ret;

	struct v4l2_handle *h;

	if(device && 0 != strncmp(device, "/dev/", 5))
	    return NULL;

	h = (struct v4l2_handle*) malloc(sizeof(*h));
	if ( h == NULL)
	    return NULL;

	memset(h, 0, sizeof(*h));

	h->fd = -1;
	h->device = strdup(device ? device : "/dev/v4l/video0");

	if (v4l2_open(h) != 0)
	    goto err;

	h->app_fmt = (struct app_video_format*) malloc(sizeof(*h->app_fmt));
	memset(h->app_fmt, 0, sizeof(*h->app_fmt));

	h->app_fmt->width = width;
	h->app_fmt->height = height;
	h->app_fmt->pixelformat = V4L2_PIX_FMT_UYVY;
	/*h->app_fmt->pixelformat = V4L2_PIX_FMT_SBGGR8;*/

	get_device_capabilities(h);

	fprintf(stderr, "v4l2: init\nv4l2: device info:\n"
		"  %s %d.%d.%d / %s @ %s\n",
		h->cap.driver,
		(h->cap.version >> 16) & 0xff,
		(h->cap.version >>  8) & 0xff,
		h->cap.version         & 0xff,
		h->cap.card,h->cap.bus_info);

	if (h->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
	{
		h->capture = 1;
	}
	else if (h->cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)
	{
		h->player = 1;
	}
	else {
		fprintf(stderr, "Driver doesn't support capture/video"
			"output capabilities\n");
		goto err;
	}

	if (h->cap.capabilities & V4L2_CAP_READWRITE)
	    h->flags |=  CAN_READWRITE;
	if (h->cap.capabilities && V4L2_CAP_STREAMING)
	    h->flags |= CAN_STREAM;
	if (h->cap.capabilities && V4L2_CAP_VIDEO_OVERLAY)
	    h->flags |= CAN_OVERLAY;

	if (h->capture)
	    h->app_fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (h->player)
	    h->app_fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	enum_fmts(h);

	v4l2_setformat(h);

	return h;
err:
	if (h->fd != -1)
	    close(h->fd);
	if (h)
	    free(h);
	return NULL;
}

static void 
v4l_reqbufs(struct v4l2_handle *h, int bcount)
{
	CLEAR(h->req);

	h->req.count= bcount;
	h->req.type = h->fmt.type;
	h->req.memory = h->app_fmt->memory;

	if(-1 == xioctl(h->fd,VIDIOC_REQBUFS,&h->req)){
	    if(EINVAL == errno){
		fprintf(stderr,"%s does not allow to request buffers\n",h->device);
		exit(-1);
	    }else{
		errno_exit("VIDIOC_REQBUFS");
	    }
	}
	if(h->req.count < 2){
	    fprintf(stderr,"Insufficient buffer memory on %s\n",h->device);
	    exit(-1);
	}

	printf("Driver provided buffers are %d\n", h->req.count);
}

int vcapture_init_userp(struct v4l2_handle *h, unsigned int buffer_size)
{
	struct v4l2_requestbuffers req;

	h->app_fmt->memory = V4L2_MEMORY_USERPTR;
	v4l_reqbufs(h, bcount);

	for(n_buffers = 0; n_buffers < h->req.count; ++n_buffers){

	    struct v4l2_buffer buf;

#ifdef ALIGN
	    buffer_size = buffer_size + 0x20;
#endif
	    if(buffer_size & 0xfff)
		buffer_size = (buffer_size & 0xffffe000) + 4096;

	    h->buf_me[n_buffers].size = buffer_size;
	    h->buf_me[n_buffers].data = malloc(buffer_size);

	    printf("User Buffers[%d].start = %x  length = %d\n",
		  n_buffers,h->buf_me[n_buffers].data,
		  h->buf_me[n_buffers].size); 

	    if(!h->buf_me[n_buffers].data){
		fprintf(stderr,"Out of memory\n");
		exit(-1);
	    }

	    h->buf_v4l2[n_buffers].type = h->req.type;
	    h->buf_v4l2[n_buffers].memory = V4L2_MEMORY_USERPTR;
	    h->buf_v4l2[n_buffers].index = n_buffers;

#ifdef ALIGN
	    h->buf_v4l2[n_buffers].m.userptr = ((unsigned
						 int)h->buf_me[n_buffers].data &
						   0xffffffe0) + 0x20;
#else
	    h->buf_v4l2[n_buffers].m.userptr = (unsigned
						int)h->buf_me[n_buffers].data;
#endif

	    if (xioctl(h->fd, VIDIOC_QUERYBUF, &h->buf_v4l2[n_buffers]) == -1) {
		errno_exit("VIDIOC_QUERYBUF");
	    }
	}
	return 0;
}
int vcapture_init_mmap(struct v4l2_handle *h, int bcount) 
{
	int i;

	h->app_fmt->memory = V4L2_MEMORY_MMAP;
	v4l_reqbufs(h, bcount);	

	for(n_buffers = 0; n_buffers < h->req.count; ++n_buffers)
	{
		h->buf_v4l2[n_buffers].type = h->fmt.type;
		h->buf_v4l2[n_buffers].memory= V4L2_MEMORY_MMAP;
		h->buf_v4l2[n_buffers].index = n_buffers;

		if(-1 == xioctl(h->fd, VIDIOC_QUERYBUF,
				&h->buf_v4l2[n_buffers])){
		    	printf("error is %d\n", strerror(errno));
			errno_exit("QUERYBUF");
		}
		h->buf_me[n_buffers].size = h->buf_v4l2[n_buffers].length;
		h->buf_me[n_buffers].data = mmap(NULL,
				   h->buf_me[n_buffers].size,/* + 64,*/
				   PROT_READ|PROT_WRITE,
				   MAP_SHARED,
				   h->fd,
				   h->buf_v4l2[n_buffers].m.offset);

		if(h->buf_me[n_buffers].data == MAP_FAILED)
			errno_exit("MMAP");
	}
	return 0;
}
int vcapture_init_read(struct v4l2_handle *h, unsigned int buffer_size)
{

	h->buf_me[0].data = malloc(buffer_size);
	h->buf_me[0].size = buffer_size;

	if(!h->buf_me[0].data)
	{
            fprintf(stderr, "Error allocation memory: malloc\n");
	    return -1;
	}

	return 0;
}

int vcapture_set_streamparm(struct v4l2_handle *h, int fps)
{
	memset(&h->streamparm, 0, sizeof(h->streamparm));

	h->streamparm.type = h->app_fmt->type;
	h->streamparm.parm.capture.capturemode = 0;
	h->streamparm.parm.capture.timeperframe.numerator = 1;
	h->streamparm.parm.capture.timeperframe.denominator = fps;

	if(xioctl(h->fd, VIDIOC_S_PARM, &h->streamparm) == -1){
	    fprintf(stderr, "Unable to set %d fps: \n", fps,
		    strerror(errno));
	    return -1;
	}
	return 0;
}

static int read_frame(struct v4l2_handle *h)
{
	struct v4l2_buffer buf;
	unsigned int i;

	switch (io) {
	  case IO_METHOD_READ:
		if (-1 == read(h->fd, h->buf_me[0].data,
			       h->buf_me[0].size)) {
		    switch (errno) {
		      case EAGAIN:
			  return 0;
		      case EIO:
			  assert(0);
			  /* Could ignore EIO, see spec. */
			  /* fall through */
		      default:
			  errno_exit("read_frame");
		    }
		}
		if(capture) { /*this can slow down a bit over NFS*/
		    fwrite(h->buf_me[0].data, 1,h->buf_me[0].size, outfd);
		}
	  break;

	  case IO_METHOD_MMAP:

		  CLEAR(buf);

		  buf.type = h->fmt.type;
		  buf.memory = V4L2_MEMORY_MMAP;

		  if(-1 == xioctl(h->fd,VIDIOC_DQBUF,&buf)){
		      switch(errno){
			case EAGAIN:
				return 0;
			case EIO:
			 /*Could ignore EIO,see spec.*/
			 /*fall through*/
			default:
			 errno_exit("VIDIOC_DQBUF");
		      }
		  }
		  assert(buf.index < n_buffers);

		  h->buf_v4l2[buf.index] = buf;

		  if(capture) { 
		     fwrite(h->buf_me[buf.index].data, 1,
			    h->buf_me[buf.index].size, outfd);
	          } 

		  if(-1==xioctl(h->fd,VIDIOC_QBUF,&buf))
		      errno_exit("VIDIOC_QBUF");

	  break;
	  case IO_METHOD_USERPTR:
	  {
	      	  char *src_buf;
		  CLEAR(buf);

		  buf.type = h->fmt.type;
		  buf.memory = V4L2_MEMORY_USERPTR;

		  if(-1 == xioctl(h->fd,VIDIOC_DQBUF,&buf)){
		      switch(errno){
			case EAGAIN:
				return 0;
			case EIO:
			 /*Could ignore EIO,see spec.*/
			 /*fall through*/
			default:
			 errno_exit("VIDIOC_DQBUF");
		      }
		  }

		  h->buf_v4l2[buf.index] = buf;
#ifdef ALIGN
		  src_buf = (char*)(((unsigned
				     int)h->buf_me[buf.index].data &
					  0xffffffe0) + 0x20);
#endif
		  if(-1 == xioctl(h->fd,VIDIOC_QBUF,&buf))
		      errno_exit("VIDIOC_QBUF");
	}
	 break;
	}
	return 1;
}

static void
start_capturing(struct v4l2_handle *h)
{
	unsigned int i;
	enum v4l2_buf_type type;
  struct v4l2_buffer buf;

	switch(io){
	  case IO_METHOD_READ:
	      /*Nothing to do.*/
	      break;
	  case IO_METHOD_MMAP:
	  case IO_METHOD_USERPTR:

	      for( i = 0; i < n_buffers; ++i){
					  print_bufinfo(&h->buf_v4l2[i]);
					  if(-1 == xioctl(h->fd,VIDIOC_QBUF,&h->buf_v4l2[i]))
					      errno_exit("VIDIOC_QBUF");
	      }

	      type = h->fmt.type;

	      if(-1 == xioctl(h->fd,VIDIOC_STREAMON,&type))
		  			errno_exit("VIDIOC_STREAMON");

	      break;
	}
}

static void
stop_capturing(struct v4l2_handle *h)
{
	enum v4l2_buf_type type;

	switch(io){
	  case IO_METHOD_READ:
	      /*Nothing to do.*/
	      break;
	  case IO_METHOD_MMAP:
	  case IO_METHOD_USERPTR:
	      type = h->fmt.type;
	      if(-1 == xioctl(h->fd, VIDIOC_STREAMOFF, &type))
		  errno_exit("VIDIOC_STREAMOFF");
	      break;
	}
}

/* do capture   */
int mainloop(struct v4l2_handle *h)
{
	int i, type;
	unsigned int count;

	fd_set rdfds;
	struct timeval tv;
	int ret;

	count = nframes;
	printf("capture\n");
	while (count-- > 0) {
	    printf("wdp %s %s %d, count = %d  =========\n", __FILE__, __FUNCTION__, __LINE__, count);

	    for (;;) {

		FD_ZERO (&rdfds);
		FD_SET (h->fd, &rdfds);

		/* Timeout. */
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		ret = select (h->fd + 1, &rdfds, NULL, NULL, &tv);
		if (ret < 0) {
		    if (EINTR == errno)
		    {
			fprintf(stderr, "EINTR\n");
			continue;
		    }
		    errno_exit ("select");
		}
		if (ret == 0) {
		    fprintf (stderr, "select timeout\n");
		    exit(0);
		}
		fflush(0);    
		printf(".");
		if (read_frame(h))
		    break;

		/* EAGAIN - continue select loop. */
	    }
	}
	printf("\n");
	return 0;
}

static void
uninit_device(struct v4l2_handle *h)
{
	unsigned int i;

	switch(io){
	case IO_METHOD_READ:
		free(h->buf_me[0].data);
	break;
	case IO_METHOD_MMAP:
		for(i = 0;i < n_buffers; ++i){
		    if(-1 == munmap(h->buf_me[i].data,h->buf_me[i].size))
			errno_exit("munmap");
		}
	break;
	case IO_METHOD_USERPTR:
		for(i = 0;i < n_buffers; ++i)
		    free(h->buf_me[i].data);
	break;
	}

	free(h->buf_me);
}
static int
start_overlay(struct v4l2_handle *h)
{
    	int e = 1;

	if(-1 == xioctl(h->fd, VIDIOC_OVERLAY, &e))
	{
		errno_exit("VIDIOC_OVERLAY");
	}

	return 0;
}
static void
init_buffers(struct v4l2_handle *h)
{
	switch (io) {
		case IO_METHOD_MMAP:
			init_mmap(h, bcount);
		break;    
		case IO_METHOD_USERPTR:
			init_userp(h, h->fmt.fmt.pix.sizeimage);
		break;
		case IO_METHOD_READ:
			init_read(h, h->fmt.fmt.pix.sizeimage);
		break;    
	}
}
static int vid_capture(void *handle, char *filename)
{
	struct v4l2_handle *h = handle;

       printf("wdp %s %s %d, filename = %s  =========\n", __FILE__, __FUNCTION__, __LINE__, filename);
    	init_buffers(h);
	outfd = fopen(filename, "w+");
	if (outfd == NULL) {
	    fprintf(stderr, "Unable to open %s file\n", filename);
	    return -1;
	}	
	printf("wdp %s %s %d, filename = %s  =========\n", __FILE__, __FUNCTION__, __LINE__, filename);

	start_capturing(h);
	printf("wdp %s %s %d, filename = %s  =========\n", __FILE__, __FUNCTION__, __LINE__, filename);

	mainloop(h);
	printf("wdp %s %s %d, filename = %s  =========\n", __FILE__, __FUNCTION__, __LINE__, filename);

	stop_capturing(h);
	printf("wdp %s %s %d, filename = %s  =========\n", __FILE__, __FUNCTION__, __LINE__, filename);

}

/* main */
/* usage: -d <device> -w <width> -h <height> -n <no.of frames>
 * -o <output dump to file>
 */
static void usage(char *command){

	printf(
	       " Usage: %s [OPTION]... \n"
	       "\n"
       	       "-h, --help	help\n"
	       "-d, --device	video device(default:/dev/v4l/video0)\n"
	       "-w, --width	width (default: 320 pixels)\n"
	       "-H, --height	height (default: 240 pixels)\n"
	       "-n, --frames	no. of frames (default: 10)\n"
	       "-m, --mmap	mmmap(default: read)\n"
	       "-r, --read	read(default: read)\n"
	       "-u, --userp	userp(default: read)\n"
	       "-b, --bcount	buffer count(default: 2)\n"
	       "-p, --preview	preview on(default: off)\n",
	       command
	 );

}
static void signal_handler(int sig)
{
    	fprintf(stderr, "\nAborted by signal %s...\n", strsignal(sig));

	if(vcapture) {
	    if (outfd > 1)
		close(outfd);
		outfd = -1;
	}

	exit(-1);
}
int main(int argc, char *argv[])
{
	struct v4l2_handle *hcapture;
	struct v4l2_handle *hpreview;
	char *short_options="hd:w:H:n:o:v:b:f:mrupC";
	char *prev_device = "/dev/v4l/video1";
	static struct option long_options[] = {
	    {"help", 0, 0, 'h'},
	    {"device",1, 0, 'd'},
	    {"width", 1, 0, 'w'},
	    {"height", 1, 0, 'H'},
	    {"frames", 1, 0, 'n'},
	    {"file", 1, 0, 'o'},
	    {"mmap", 0, 0, 'm'},
	    {"read", 0, 0, 'r'},
	    {"userptr", 0, 0, 'u'},
	    {"bcount", 1, 0, 'b'},
	    {"preview", 0, 0, 'p'},
	    {"fps", 1, 0, 'f'},
	    {"capture", 0, 0, 'C'},
	    {0, 0, 0, 0}
	};
	int option_index;
	int fd, err, c;

	command = argv[0];	    

	if (strstr(argv[0], "vplay")){
		vplayer = 1;
		command = "vplay";
	}else if (strstr(argv[0], "vcapture")){
		vcapture = 1;
		command = "vcapture";
	}else {
		fprintf(stderr, "command should be named either vcapture or vplay\n");
		return 1;
	}

	while ((c = getopt_long(argc, argv, short_options, long_options,
				&option_index)) != -1) {
	    switch(c){
	      case 'h':
		  usage(command);	    
		  return 0;
	      case 'd':
		  device = optarg;
		  break;
	      case 'w':
		  width = atoi(optarg);
		  break;
	      case 'H':
		  height = atoi(optarg);
		  break;
	      case 'n':
		  nframes = atoi(optarg);
		  break;
	      case 'N':
		  open_mode |= O_NONBLOCK;
		  break;
	      case 'm':
		  io = IO_METHOD_MMAP;
		  printf("Info: MMAP method selected\n");
		  break;
	      case 'r':
		  io = IO_METHOD_READ;
		  printf("Info: READ method selected\n");
		  break;
	      case 'u':
		  io = IO_METHOD_USERPTR;
		  printf("Info: USER Pointer method selected\n");
		  break;
	      case 'b':
		  bcount = atoi(optarg);
		  break;
	      case 'p':
		  preview = 1;
		  break;
	      case 'f':
		  fps = atoi(optarg);
		  break;
	      case 'C':
		  capture = 1;
		  break;
	      default:
		  fprintf(stderr, "Try %s --help for more infromation\n",
			  command);
		  return 1;
	    }
	}

	if(vcapture){
		init_mmap = vcapture_init_mmap;
		init_userp = vcapture_init_userp;
		init_read = vcapture_init_read;
		init_device = vcapture_init_device;
		if (!nframes)
		   nframes = DEFAULT_NFRAMES;
	}

/*
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGABRT, signal_handler);
*/

	hcapture = init_device(device);
	//hpreview = init_device(prev_device);

	if ( hcapture == NULL)
	    fprintf(stderr, "Unable to initialize %s video device\n", device);

	/*if ( hpreview == NULL)
	    fprintf(stderr, "Unable to initialize %s video device\n", prev_device);
	*/

	if(fps)
	    vcapture_set_streamparm(hcapture, fps);

	/*start preview*/
	if(preview)
	    start_overlay(hcapture);

	if(capture){
	    	vid_capture(hcapture, argv[optind]);

		/* init preview buffers 
		 * NOTE: same io method will be used
		 * for video out as video capture
		 */
		//init_buffers(hpreview, bcount);
	}

	/*preview only*/
	if(preview && (!capture))/*spin continously, until user aborts*/
	    while(1);

	uninit_device(hcapture);
	//uninit_device(hpreview);

	if (capture) 
	    fclose(outfd);

	v4l2_close(hcapture);
	//v4l2_close(hpreview);

	return 0;
}
