/* In place of winsys/panfrost/drm: the screen of the one GPU, opened through the SweetMali shim. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipe/p_screen.h"
#include "util/xmlconfig.h"
#include "xf86drm.h"
#include "panfrost/pan_public.h"
#include "sweet_screen.h"

struct pipe_screen *
sweet_panfrost_screen(void)
{
   static struct pipe_screen_config config;
   struct pipe_screen *screen;
   int fd;

   if (getenv("SWEET_SOFTWARE") != NULL)
      return NULL;
   fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
   if (fd < 0)
      return NULL;
   memset(&config, 0, sizeof(config));
   screen = panfrost_create_screen(fd, &config, NULL);
   if (screen == NULL)
      close(fd);
   return screen;
}
