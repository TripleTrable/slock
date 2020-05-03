/* See LICENSE file for license details. */
#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif
#define MAX_SOURCE_SIZE (0x100000)

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <Imlib2.h>
#include <math.h>
#include<CL/cl.h>
#include <wchar.h>

#include "arg.h"
#include "util.h"

char *argv0;

enum {
	INIT,
	INPUT,
	FAILED,
	NUMCOLS
};

struct lock {
	int screen;
	Window root, win;
	Pixmap pmap;
	Pixmap bgmap;
	unsigned long colors[NUMCOLS];
};

struct xrandr {
	int active;
	int evbase;
	int errbase;
};

#include "config.h"

Imlib_Image image;

static void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

#ifdef __linux__
#include <fcntl.h>
#include <linux/oom.h>

static void
dontkillme(void)
{
	FILE *f;
	const char oomfile[] = "/proc/self/oom_score_adj";

	if (!(f = fopen(oomfile, "w"))) {
		if (errno == ENOENT)
			return;
		die("slock: fopen %s: %s\n", oomfile, strerror(errno));
	}
	fprintf(f, "%d", OOM_SCORE_ADJ_MIN);
	if (fclose(f)) {
		if (errno == EACCES)
			die("slock: unable to disable OOM killer. "
			    "Make sure to suid or sgid slock.\n");
		else
			die("slock: fclose %s: %s\n", oomfile, strerror(errno));
	}
}
#endif

static const char *
gethash(void)
{
	const char *hash;
	struct passwd *pw;

	/* Check if the current user has a password entry */
	errno = 0;
	if (!(pw = getpwuid(getuid()))) {
		if (errno)
			die("slock: getpwuid: %s\n", strerror(errno));
		else
			die("slock: cannot retrieve password entry\n");
	}
	hash = pw->pw_passwd;

#if HAVE_SHADOW_H
	if (!strcmp(hash, "x")) {
		struct spwd *sp;
		if (!(sp = getspnam(pw->pw_name)))
			die("slock: getspnam: cannot retrieve shadow entry. "
			    "Make sure to suid or sgid slock.\n");
		hash = sp->sp_pwdp;
	}
#else
	if (!strcmp(hash, "*")) {
#ifdef __OpenBSD__
		if (!(pw = getpwuid_shadow(getuid())))
			die("slock: getpwnam_shadow: cannot retrieve shadow entry. "
			    "Make sure to suid or sgid slock.\n");
		hash = pw->pw_passwd;
#else
		die("slock: getpwuid: cannot retrieve shadow entry. "
		    "Make sure to suid or sgid slock.\n");
#endif /* __OpenBSD__ */
	}
#endif /* HAVE_SHADOW_H */

	return hash;
}

static void
readpw(Display *dpy, struct xrandr *rr, struct lock **locks, int nscreens,
       const char *hash)
{
	XRRScreenChangeNotifyEvent *rre;
	char buf[32], passwd[256], *inputhash;
	int num, screen, running, failure, oldc;
	unsigned int len, color;
	KeySym ksym;
	XEvent ev;

	len = 0;
	running = 1;
	failure = 0;
	oldc = INIT;

	while (running && !XNextEvent(dpy, &ev)) {
		if (ev.type == KeyPress) {
			explicit_bzero(&buf, sizeof(buf));
			num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
			if (IsKeypadKey(ksym)) {
				if (ksym == XK_KP_Enter)
					ksym = XK_Return;
				else if (ksym >= XK_KP_0 && ksym <= XK_KP_9)
					ksym = (ksym - XK_KP_0) + XK_0;
			}
			if (IsFunctionKey(ksym) ||
			    IsKeypadKey(ksym) ||
			    IsMiscFunctionKey(ksym) ||
			    IsPFKey(ksym) ||
			    IsPrivateKeypadKey(ksym))
				continue;
			switch (ksym) {
			case XK_Return:
				passwd[len] = '\0';
				errno = 0;
				if (!(inputhash = crypt(passwd, hash)))
					fprintf(stderr, "slock: crypt: %s\n", strerror(errno));
				else
					running = !!strcmp(inputhash, hash);
				if (running) {
					XBell(dpy, 100);
					failure = 1;
				}
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				break;
			case XK_Escape:
				explicit_bzero(&passwd, sizeof(passwd));
				len = 0;
				break;
			case XK_BackSpace:
				if (len)
					passwd[--len] = '\0';
				break;
			default:
				if (num && !iscntrl((int)buf[0]) &&
				    (len + num < sizeof(passwd))) {
					memcpy(passwd + len, buf, num);
					len += num;
				}
				break;
			}
			color = len ? INPUT : ((failure || failonclear) ? FAILED : INIT);
			if (running && oldc != color) {
				for (screen = 0; screen < nscreens; screen++) {
                    if(locks[screen]->bgmap)
                        XSetWindowBackgroundPixmap(dpy, locks[screen]->win, locks[screen]->bgmap);
                    else
                        XSetWindowBackground(dpy, locks[screen]->win, locks[screen]->colors[0]);
					XClearWindow(dpy, locks[screen]->win);
				}
				oldc = color;
			}
		} else if (rr->active && ev.type == rr->evbase + RRScreenChangeNotify) {
			rre = (XRRScreenChangeNotifyEvent*)&ev;
			for (screen = 0; screen < nscreens; screen++) {
				if (locks[screen]->win == rre->window) {
					if (rre->rotation == RR_Rotate_90 ||
					    rre->rotation == RR_Rotate_270)
						XResizeWindow(dpy, locks[screen]->win,
						              rre->height, rre->width);
					else
						XResizeWindow(dpy, locks[screen]->win,
						              rre->width, rre->height);
					XClearWindow(dpy, locks[screen]->win);
					break;
				}
			}
		} else {
			for (screen = 0; screen < nscreens; screen++)
				XRaiseWindow(dpy, locks[screen]->win);
		}
	}
}

static struct lock *
lockscreen(Display *dpy, struct xrandr *rr, int screen)
{
	char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
	int i, ptgrab, kbgrab;
	struct lock *lock;
	XColor color, dummy;
	XSetWindowAttributes wa;
	Cursor invisible;

	if (dpy == NULL || screen < 0 || !(lock = malloc(sizeof(struct lock))))
		return NULL;

	lock->screen = screen;
	lock->root = RootWindow(dpy, lock->screen);

    if(image) 
    {
        lock->bgmap = XCreatePixmap(dpy, lock->root, DisplayWidth(dpy, lock->screen), DisplayHeight(dpy, lock->screen), DefaultDepth(dpy, lock->screen));
        imlib_context_set_image(image);
        imlib_context_set_display(dpy);
        imlib_context_set_visual(DefaultVisual(dpy, lock->screen));
        imlib_context_set_colormap(DefaultColormap(dpy, lock->screen));
        imlib_context_set_drawable(lock->bgmap);
        imlib_render_image_on_drawable(0, 0);
        imlib_free_image();
    }
	for (i = 0; i < NUMCOLS; i++) {
		XAllocNamedColor(dpy, DefaultColormap(dpy, lock->screen),
		                 colorname[i], &color, &dummy);
		lock->colors[i] = color.pixel;
	}

	/* init */
	wa.override_redirect = 1;
	wa.background_pixel = lock->colors[INIT];
	lock->win = XCreateWindow(dpy, lock->root, 0, 0,
	                          DisplayWidth(dpy, lock->screen),
	                          DisplayHeight(dpy, lock->screen),
	                          0, DefaultDepth(dpy, lock->screen),
	                          CopyFromParent,
	                          DefaultVisual(dpy, lock->screen),
	                          CWOverrideRedirect | CWBackPixel, &wa);
    if(lock->bgmap)
        XSetWindowBackgroundPixmap(dpy, lock->win, lock->bgmap);
	lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);
	invisible = XCreatePixmapCursor(dpy, lock->pmap, lock->pmap,
	                                &color, &color, 0, 0);
	XDefineCursor(dpy, lock->win, invisible);

	/* Try to grab mouse pointer *and* keyboard for 600ms, else fail the lock */
	for (i = 0, ptgrab = kbgrab = -1; i < 6; i++) {
		if (ptgrab != GrabSuccess) {
			ptgrab = XGrabPointer(dpy, lock->root, False,
			                      ButtonPressMask | ButtonReleaseMask |
			                      PointerMotionMask, GrabModeAsync,
			                      GrabModeAsync, None, invisible, CurrentTime);
		}
		if (kbgrab != GrabSuccess) {
			kbgrab = XGrabKeyboard(dpy, lock->root, True,
			                       GrabModeAsync, GrabModeAsync, CurrentTime);
		}

		/* input is grabbed: we can lock the screen */
		if (ptgrab == GrabSuccess && kbgrab == GrabSuccess) {
			XMapRaised(dpy, lock->win);
			if (rr->active)
				XRRSelectInput(dpy, lock->win, RRScreenChangeNotifyMask);

			XSelectInput(dpy, lock->root, SubstructureNotifyMask);
			return lock;
		}

		/* retry on AlreadyGrabbed but fail on other errors */
		if ((ptgrab != AlreadyGrabbed && ptgrab != GrabSuccess) ||
		    (kbgrab != AlreadyGrabbed && kbgrab != GrabSuccess))
			break;

		usleep(100000);
	}

	/* we couldn't grab all input: fail out */
	if (ptgrab != GrabSuccess)
		fprintf(stderr, "slock: unable to grab mouse pointer for screen %d\n",
		        screen);
	if (kbgrab != GrabSuccess)
		fprintf(stderr, "slock: unable to grab keyboard for screen %d\n",
		        screen);
	return NULL;
}

static void
usage(void)
{
	die("usage: slock [-v] [cmd [arg ...]]\n");
}

int
main(int argc, char **argv) {
	struct xrandr rr;
	struct lock **locks;
	struct passwd *pwd;
	struct group *grp;
	uid_t duid;
	gid_t dgid;
	const char *hash;
	Display *dpy;
	int s, nlocks, nscreens;

	ARGBEGIN {
	case 'v':
		fprintf(stderr, "slock-"VERSION"\n");
		return 0;
	default:
		usage();
	} ARGEND

	/* validate drop-user and -group */
	errno = 0;
	if (!(pwd = getpwnam(user)))
		die("slock: getpwnam %s: %s\n", user,
		    errno ? strerror(errno) : "user entry not found");
	duid = pwd->pw_uid;
	errno = 0;
	if (!(grp = getgrnam(group)))
		die("slock: getgrnam %s: %s\n", group,
		    errno ? strerror(errno) : "group entry not found");
	dgid = grp->gr_gid;

#ifdef __linux__
	dontkillme();
#endif

	hash = gethash();
	errno = 0;
	if (!crypt("", hash))
		die("slock: crypt: %s\n", strerror(errno));

	if (!(dpy = XOpenDisplay(NULL)))
		die("slock: cannot open display\n");

	/* drop privileges */
	if (setgroups(0, NULL) < 0)
		die("slock: setgroups: %s\n", strerror(errno));
	if (setgid(dgid) < 0)
		die("slock: setgid: %s\n", strerror(errno));
	if (setuid(duid) < 0)
		die("slock: setuid: %s\n", strerror(errno));

	/*Create screenshot Image*/
	Screen *scr = ScreenOfDisplay(dpy, DefaultScreen(dpy));
	image = imlib_create_image(scr->width,scr->height);
	imlib_context_set_image(image);
	imlib_context_set_display(dpy);
	imlib_context_set_visual(DefaultVisual(dpy,0));
	imlib_context_set_drawable(RootWindow(dpy,XScreenNumberOfScreen(scr)));	
	imlib_copy_drawable_to_image(0,0,0,scr->width,scr->height,0,0,1);

	int width = scr->width;
	int height = scr->height;
	int inputSize = width*height*4;
	Imlib_Color pixel;
	Imlib_Color* pp = &pixel;
	char *buffer = malloc(inputSize * sizeof(int));

	char *Out = malloc(inputSize * sizeof(int));
	/*convert Imlib_Image to rgba array*/
	for(int y = 0;y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			imlib_image_query_pixel(x,y,pp);
			buffer[(x+y*width)*4 + 0] = pixel.red;
			buffer[(x+y*width)*4 + 1] = pixel.green;
			buffer[(x+y*width)*4 + 2] = pixel.blue;
			buffer[(x+y*width)*4 + 3] = pixel.alpha;
		}
	}

#ifdef BLUR

	/*create kernel*/
	float sigma = 2;
	int kernelDimension = (int)ceilf(5 * sigma);
 	if (kernelDimension % 2 == 0)
 	kernelDimension++;
	int cKernelSize = pow(kernelDimension, 2);
	float ckernel[kernelDimension*kernelDimension];
	float acc = 0;
	
	for (int j = 0; j < kernelDimension; j++)
	{
   		int y = j - (kernelDimension / 2);
        for (int i = 0; i < kernelDimension; i++)
      	{
          	int x = i - (kernelDimension / 2);
 
          	ckernel[j*kernelDimension+i] = (1 / (2 * 3.14159*pow(sigma, 2)))*exp(-((pow(x, 2) + pow(y, 2)) / (2 * pow(sigma, 2))));
 
          	acc += ckernel[j*i+i];
		}
	}
	
 	for (int j = 0; j < kernelDimension; j++)
	{
		for (int i = 0; i < kernelDimension; i++)
 		{
      		ckernel[j*kernelDimension + i] = ckernel[j*kernelDimension + i] / acc;
 		}
	}

	/*Get cl file*/
	FILE* inputFile = fopen("gaussgpu.cl","r");
	fseek(inputFile,0L,SEEK_END);
	int fileSize = ftell(inputFile);
	char* source = (char*)malloc(fileSize+1);
	rewind(inputFile);
	fileSize = fread(source,1,fileSize,inputFile);
	source[fileSize] = '\0';

	/*opencl stuff*/
	cl_int stat;
	cl_uint numPlatforms = 0;
	cl_platform_id* platforms = NULL;
	stat = clGetPlatformIDs(0,NULL,&numPlatforms);
	platforms = (cl_platform_id*) malloc(numPlatforms * sizeof(cl_platform_id));
	stat = clGetPlatformIDs(numPlatforms,platforms,NULL);

	cl_uint numDevices = 0;
	cl_device_id* devices = NULL;
	
	stat = clGetDeviceIDs(platforms[0],CL_DEVICE_TYPE_ALL,0,NULL, &numDevices);
	devices = (cl_device_id*)malloc(numDevices * sizeof(cl_device_id));
	stat = clGetDeviceIDs(platforms[0],CL_DEVICE_TYPE_ALL,numDevices,devices,NULL);

	cl_context context = NULL;
	context = clCreateContext(NULL,numDevices, devices,NULL,NULL, & stat);

	cl_command_queue cmdQueue;
	cmdQueue = clCreateCommandQueue(context,devices[0],0,&stat);

	cl_mem bufferPixels;
	cl_mem bufferOut;
	cl_mem bufferCKernel;
	cl_mem bufferRows;
	cl_mem bufferColumns;
	cl_mem bufferCKernelSize;
	cl_mem bufferDebug;
	bufferPixels = clCreateBuffer(context,CL_MEM_READ_ONLY,inputSize,NULL,&stat);
	bufferOut = clCreateBuffer(context,CL_MEM_WRITE_ONLY,inputSize,NULL,&stat);
	bufferCKernel = clCreateBuffer(context,CL_MEM_READ_ONLY,cKernelSize * sizeof(float),NULL,&stat);
	bufferRows = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(int),NULL, &stat);
	bufferColumns = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(int),NULL,&stat);
	bufferCKernelSize = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(int),NULL,&stat);

	stat = clEnqueueWriteBuffer(cmdQueue,bufferPixels,CL_TRUE,0,inputSize,buffer,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue,bufferCKernel,CL_TRUE,0,cKernelSize * sizeof(float),ckernel,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue, bufferRows, CL_TRUE,0,sizeof(int),&height,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue,bufferColumns,CL_TRUE,0,sizeof(int),&width,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue,bufferCKernelSize,CL_TRUE,0,sizeof(int),&kernelDimension,0,NULL,NULL);

	/*error Handling*/
	cl_program program = clCreateProgramWithSource(context,1,(const char **)&source,NULL,&stat);
	stat = clBuildProgram(program,numDevices,devices,NULL,NULL,NULL);
	if(stat != CL_SUCCESS)
	{
		clGetProgramBuildInfo(program,devices[0],CL_PROGRAM_BUILD_STATUS,sizeof(cl_build_status),&stat,NULL);
		size_t logSize;
		wchar_t errorbuffer[2048];
		clGetProgramBuildInfo(program,devices[0], CL_PROGRAM_BUILD_LOG,0,NULL,&logSize);
		printf("%i", logSize);
		char* programLog = (char*)calloc(logSize+1,sizeof(char));
		clGetProgramBuildInfo(program,devices[0],CL_PROGRAM_BUILD_LOG,logSize+1,programLog,NULL);

		wchar_t* wProgramLog = malloc(strlen(programLog)+1);
		mbstowcs(wProgramLog,programLog,strlen(programLog)+1);
		swprintf(errorbuffer,2048,L"error compiling openCL kernel\n Build filed;error=%d,statu=%d, programLog:nn%s",stat,stat,wProgramLog);
		printf("%i\n",stat);
		printf(programLog);
		free(programLog);
		free(wProgramLog);
		printf("ERROR\n");
    // Determine the size of the log
    size_t log_size;
    clGetProgramBuildInfo(program, devices[0], CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);

    // Allocate memory for the log
    char *log = (char *) malloc(log_size);

    // Get the log
    clGetProgramBuildInfo(program, devices[0], CL_PROGRAM_BUILD_LOG, log_size, log, NULL);

    // Print the log
    printf("%s\n", log);

	}
		/*invoke blur*/
		cl_kernel kernel = NULL;
		kernel = clCreateKernel(program,"blur",&stat);
		clSetKernelArg(kernel,0,sizeof(cl_mem),&bufferPixels);
		clSetKernelArg(kernel,1,sizeof(cl_mem),&bufferOut);
		clSetKernelArg(kernel,2,sizeof(cl_mem),&bufferCKernel);
		clSetKernelArg(kernel,3,sizeof(cl_mem),&bufferRows);
		clSetKernelArg(kernel,4,sizeof(cl_mem),&bufferColumns);
		clSetKernelArg(kernel,5,sizeof(cl_mem),&bufferCKernelSize);

	size_t globalWorkSize[1];
	globalWorkSize[0] = inputSize;
	clEnqueueNDRangeKernel(cmdQueue,kernel,1,NULL,globalWorkSize,NULL,0,NULL,NULL);
	clEnqueueReadBuffer(cmdQueue,bufferOut,CL_TRUE,0,inputSize,Out,0,NULL,NULL);
	/*convert array to image*/
	for(int i = 0; i< inputSize;i+=4)
	{
		imlib_context_set_color(Out[i],Out[i+1],Out[i+2],Out[i+3]);
		imlib_image_draw_rectangle((i/4)%width,(i/4)/width,1,1);
	}

	free(devices);
	free(source);
	clReleaseKernel(kernel);
	clReleaseProgram(program);
	clReleaseCommandQueue(cmdQueue);
	clReleaseMemObject(bufferPixels);
	clReleaseMemObject(bufferOut);
	clReleaseMemObject(bufferCKernel);
	clReleaseMemObject(bufferRows);
	clReleaseMemObject(bufferColumns);
	clReleaseMemObject(bufferCKernelSize);

#endif // BLUR

#ifdef PIXELATE
	FILE* inputFile = fopen("pixelate.cl","r");	
	fseek(inputFile,0L,SEEK_END);
	int fileSize = ftell(inputFile);
	char* source = (char*)malloc(fileSize+1);
	rewind(inputFile);
	fileSize = fread(source,1,fileSize,inputFile);
	source[fileSize] = '\0';
	cl_int stat;
	cl_uint numPlatforms = 0;
	cl_platform_id* platforms = NULL;
	stat = clGetPlatformIDs(0,NULL,&numPlatforms);
	platforms = (cl_platform_id*) malloc(numPlatforms * sizeof(cl_platform_id));
	stat = clGetPlatformIDs(numPlatforms,platforms,NULL);

	cl_uint numDevices = 0;
	cl_device_id* devices = NULL;
	
	stat = clGetDeviceIDs(platforms[0],CL_DEVICE_TYPE_ALL,0,NULL, &numDevices);
	devices = (cl_device_id*)malloc(numDevices * sizeof(cl_device_id));
	stat = clGetDeviceIDs(platforms[0],CL_DEVICE_TYPE_ALL,numDevices,devices,NULL);

	cl_context context = NULL;
	context = clCreateContext(NULL,numDevices, devices,NULL,NULL, & stat);

	cl_command_queue cmdQueue;
	cmdQueue = clCreateCommandQueue(context,devices[0],0,&stat);

	cl_mem bufferPixel;
	cl_mem bufferOut;
	cl_mem bufferPixelSize;
	cl_mem bufferRows;
	cl_mem bufferColumns;

	bufferPixel = clCreateBuffer(context,CL_MEM_READ_ONLY,inputSize,NULL,&stat);
	bufferOut = clCreateBuffer(context,CL_MEM_WRITE_ONLY,inputSize,NULL,&stat);
	bufferPixelSize = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(float),NULL,&stat);
	bufferRows = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(int),NULL,&stat);
	bufferColumns = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(int),NULL,&stat);
	
	stat = clEnqueueWriteBuffer(cmdQueue,bufferPixel,CL_TRUE,0,inputSize,buffer,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue,bufferPixelSize,CL_TRUE,0,sizeof(int),&pixelSize,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue,bufferRows,CL_TRUE,0,sizeof(int),&height,0,NULL,NULL);
	stat = clEnqueueWriteBuffer(cmdQueue,bufferColumns,CL_TRUE,0,sizeof(int),&width,0,NULL,NULL);

	/*Compile Program*/
	cl_program program = clCreateProgramWithSource(context,1,(const **)&source,NULL,&stat);
	stat = clBuildProgram(program,numDevices,devices,NULL,NULL,NULL);
	/*ErrorHandling*/
	if (stat != CL_SUCCESS)
	{
		size_t log_size;
		clGetProgramBuildInfo(program,devices[0],CL_PROGRAM_BUILD_LOG,0,NULL,&log_size);
		char *log = (char*) malloc(log_size);
		clGetProgramBuildInfo(program,devices[0],CL_PROGRAM_BUILD_LOG,log_size,log,NULL);
		printf("%s\n",log);
	}


	/*invoke pixelation*/
	cl_kernel kernel = NULL;
	kernel = clCreateKernel(program,"pixelate",&stat);
	clSetKernelArg(kernel,0,sizeof(cl_mem),&bufferPixel);
	clSetKernelArg(kernel,1,sizeof(cl_mem),&bufferOut);
	clSetKernelArg(kernel,2,sizeof(cl_mem),&bufferPixelSize),
	clSetKernelArg(kernel,3,sizeof(cl_mem),&bufferRows);
	clSetKernelArg(kernel,4,sizeof(cl_mem),&bufferColumns);

	size_t globalWorkSize[1];
	globalWorkSize[0] = inputSize/pixelSize;
	clEnqueueNDRangeKernel(cmdQueue, kernel, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL);
	clEnqueueReadBuffer(cmdQueue, bufferOut, CL_TRUE, 0, inputSize, Out, 0, NULL, NULL);	

	for(int i = 0; i< inputSize;i+=pixelSize*4)
	{
		imlib_context_set_color(Out[i],Out[i+1],Out[i+2],255);
		//printf("%i/%i/%i/%i\n",Out[i],Out[i+1],Out[i+2],255);
		imlib_image_draw_rectangle((i/4)%width,(i/4)/width,pixelSize,1);
	}


	free(devices);
	free(source);
	clReleaseKernel(kernel);
	clReleaseProgram(program);
	clReleaseCommandQueue(cmdQueue);
	clReleaseMemObject(bufferPixel);
	clReleaseMemObject(bufferOut);
	clReleaseMemObject(bufferRows);
	clReleaseMemObject(bufferColumns);

#endif // PIXELATE
	free(buffer);
	free(Out);
	/*Pixelation*/
	/*
	for(int y = 0; y < height; y += pixelSize)
	{
		for(int x = 0; x < width; x += pixelSize)
		{
			int red = 0;
			int green = 0;
			int blue = 0;

			Imlib_Color pixel; 
			Imlib_Color* pp;
			pp = &pixel;
			for(int j = 0; j < pixelSize && j < height; j++)
			{
				for(int i = 0; i < pixelSize && i < width; i++)
				{
					imlib_image_query_pixel(x+i,y+j,pp);
					red += pixel.red;
					green += pixel.green;
					blue += pixel.blue;
				}
			}
			red /= (pixelSize*pixelSize);
			green /= (pixelSize*pixelSize);
			blue /= (pixelSize*pixelSize);
			imlib_context_set_color(red,green,blue,pixel.alpha);
			imlib_image_fill_rectangle(x,y,pixelSize,pixelSize);
			red = 0;
			green = 0;
			blue = 0;
		}
	}
	
	*/
	
	/* check for Xrandr support */
	rr.active = XRRQueryExtension(dpy, &rr.evbase, &rr.errbase);

	/* get number of screens in display "dpy" and blank them */
	nscreens = ScreenCount(dpy);
	if (!(locks = calloc(nscreens, sizeof(struct lock *))))
		die("slock: out of memory\n");
	for (nlocks = 0, s = 0; s < nscreens; s++) {
		if ((locks[s] = lockscreen(dpy, &rr, s)) != NULL)
			nlocks++;
		else
			break;
	}
	XSync(dpy, 0);

	/* did we manage to lock everything? */
	if (nlocks != nscreens)
		return 1;

	/* run post-lock command */
	if (argc > 0) {
		switch (fork()) {
		case -1:
			die("slock: fork failed: %s\n", strerror(errno));
		case 0:
			if (close(ConnectionNumber(dpy)) < 0)
				die("slock: close: %s\n", strerror(errno));
			execvp(argv[0], argv);
			fprintf(stderr, "slock: execvp %s: %s\n", argv[0], strerror(errno));
			_exit(1);
		}
	}

	/* everything is now blank. Wait for the correct password */
	readpw(dpy, &rr, locks, nscreens, hash);

	return 0;
}
