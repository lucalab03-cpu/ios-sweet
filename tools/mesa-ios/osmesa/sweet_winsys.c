/* In place of winsys/panfrost/drm: the screen of the one GPU, opened through the SweetMali shim. */
#include <stdbool.h>
#include <stdint.h>
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

/* Mesa is built without its XML configuration (no expat): panfrost's options all keep their defaults. */
void
driParseConfigFiles(driOptionCache *cache, const driOptionCache *info, int screenNum, const char *driverName,
    const char *kernelDriverName, const char *deviceName, const char *applicationName,
    uint32_t applicationVersion, const char *engineName, uint32_t engineVersion)
{
   (void)cache; (void)info; (void)screenNum; (void)driverName; (void)kernelDriverName; (void)deviceName;
   (void)applicationName; (void)applicationVersion; (void)engineName; (void)engineVersion;
}

unsigned char
driQueryOptionb(const driOptionCache *cache, const char *name)
{
   (void)cache; (void)name;
   return false;
}

int
driQueryOptioni(const driOptionCache *cache, const char *name)
{
   (void)cache; (void)name;
   return 0;
}
