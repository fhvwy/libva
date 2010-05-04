/*
 * Copyright (c) 2007 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE 1
#include "va.h"
#include "va_backend.h"
#include "va_android.h"
#include "x11/va_dricommon.h" /* needs some helper functions from this file */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <errno.h>

#define CHECK_SYMBOL(func) { if (!func) printf("func %s not found\n", #func); return VA_STATUS_ERROR_UNKNOWN; }
#define DEVICE_NAME "/dev/dri/card0"

static VADisplayContextP pDisplayContexts = NULL;

static int open_device (char *dev_name)
{
  struct stat st;
  int fd;

  if (-1 == stat (dev_name, &st))
    {
      printf ("Cannot identify '%s': %d, %s\n",
               dev_name, errno, strerror (errno));
      return -1;
    }

  if (!S_ISCHR (st.st_mode))
    {
      printf ("%s is no device\n", dev_name);
      return -1;
    }

  fd = open (dev_name, O_RDWR /* required */  | O_NONBLOCK, 0);

  if (-1 == fd)
    {
      fprintf (stderr, "Cannot open '%s': %d, %s\n",
               dev_name, errno, strerror (errno));
      return -1;
    }

  return fd;
}

static int va_DisplayContextIsValid (
    VADisplayContextP pDisplayContext
)
{
    VADisplayContextP ctx = pDisplayContexts;

    while (ctx)
    {
	if (ctx == pDisplayContext && pDisplayContext->pDriverContext)
	    return 1;
	ctx = ctx->pNext;
    }
    return 0;
}

static void va_DisplayContextDestroy (
    VADisplayContextP pDisplayContext
)
{
    VADisplayContextP *ctx = &pDisplayContexts;

    /* Throw away pDisplayContext */
    while (*ctx)
    {
	if (*ctx == pDisplayContext)
	{
	    *ctx = pDisplayContext->pNext;
	    pDisplayContext->pNext = NULL;
	    break;
	}
	ctx = &((*ctx)->pNext);
    }
    free(pDisplayContext->pDriverContext->dri_state);
    free(pDisplayContext->pDriverContext);
    free(pDisplayContext);
}


static VAStatus va_DisplayContextGetDriverName (
    VADisplayContextP pDisplayContext,
    char **driver_name
)
{
    VADriverContextP ctx = pDisplayContext->pDriverContext;
    struct dri_state *dri_state = (struct dri_state *)ctx->dri_state;
    char *driver_name_env;
    int vendor_id, device_id;
    
    struct {
        int vendor_id;
        int device_id;
        char driver_name[64];
    } devices[] = {
        { 0x8086, 0x4100, "pvr" },
        { 0x8086, 0x0130, "pvr" },
        { 0x0,    0x0,    "\0" },
    };

    memset(dri_state, 0, sizeof(*dri_state));
    dri_state->fd = open_device(DEVICE_NAME);
    
    if (dri_state->fd < 0) {
        fprintf(stderr,"can't open DRM devices\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }

    if ((driver_name_env = getenv("LIBVA_DRIVER_NAME")) != NULL
        && geteuid() == getuid()) {
        /* don't allow setuid apps to use LIBVA_DRIVER_NAME */
        *driver_name = strdup(driver_name_env);
        return VA_STATUS_SUCCESS;
    } else { /* TBD: other vendor driver names */
	vendor_id = devices[0].vendor_id;
	device_id = devices[0].device_id;
	*driver_name = strdup(devices[0].driver_name);
    }

    dri_state->driConnectedFlag = VA_DUMMY;
    
    return VA_STATUS_SUCCESS;
}


VADisplay vaGetDisplay (
    void *native_dpy /* implementation specific */
)
{
  VADisplay dpy = NULL;
  VADisplayContextP pDisplayContext = pDisplayContexts;

  if (!native_dpy)
      return NULL;

  while (pDisplayContext)
  {
      if (pDisplayContext->pDriverContext &&
	  pDisplayContext->pDriverContext->native_dpy == (void *)native_dpy)
      {
          dpy = (VADisplay)pDisplayContext;
          break;
      }
      pDisplayContext = pDisplayContext->pNext;
  }


  if (!dpy)
  {
      /* create new entry */
      VADriverContextP pDriverContext;
      struct dri_state *dri_state;
      pDisplayContext = (VADisplayContextP)calloc(1, sizeof(*pDisplayContext));
      pDriverContext  = (VADriverContextP)calloc(1, sizeof(*pDriverContext));
      dri_state       = calloc(1, sizeof(*dri_state));
      if (pDisplayContext && pDriverContext && dri_state)
      {
	  pDisplayContext->vadpy_magic = VA_DISPLAY_MAGIC;          

	  pDriverContext->native_dpy       = (void *)native_dpy;
	  pDisplayContext->pNext           = pDisplayContexts;
	  pDisplayContext->pDriverContext  = pDriverContext;
	  pDisplayContext->vaIsValid       = va_DisplayContextIsValid;
	  pDisplayContext->vaDestroy       = va_DisplayContextDestroy;
	  pDisplayContext->vaGetDriverName = va_DisplayContextGetDriverName;
	  pDisplayContexts                 = pDisplayContext;
	  pDriverContext->dri_state 	   = dri_state;
	  dpy                              = (VADisplay)pDisplayContext;
      }
      else
      {
	  if (pDisplayContext)
	      free(pDisplayContext);
	  if (pDriverContext)
	      free(pDriverContext);
          if (dri_state)
              free(dri_state);
      }
  }
  
  return dpy;
}

#define CTX(dpy) (((VADisplayContextP)dpy)->pDriverContext)
#define CHECK_DISPLAY(dpy) if( !vaDisplayIsValid(dpy) ) { return VA_STATUS_ERROR_INVALID_DISPLAY; }

static int vaDisplayIsValid(VADisplay dpy)
{
    VADisplayContextP pDisplayContext = (VADisplayContextP)dpy;
    return pDisplayContext && (pDisplayContext->vadpy_magic == VA_DISPLAY_MAGIC) && pDisplayContext->vaIsValid(pDisplayContext);
}

#ifdef ANDROID
VAStatus vaPutSurface (
    VADisplay dpy,
    VASurfaceID surface,
    Surface *draw, /* Android Surface/Window */
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    VARectangle *cliprects, /* client supplied clip list */
    unsigned int number_cliprects, /* number of clip rects in the clip list */
    unsigned int flags /* de-interlacing flags */
)
{
  VADriverContextP ctx;

  CHECK_DISPLAY(dpy);
  ctx = CTX(dpy);

  return ctx->vtable.vaPutSurface( ctx, surface, (void *)draw, srcx, srcy, srcw, srch, 
                                   destx, desty, destw, desth,
                                   cliprects, number_cliprects, flags );
}

#endif
