/* (c) 2010 Michal Maruska <mmaruska@gmail.com>
 *
 * Common defines for the pipeline & plugins.
 * (SDK for plugins)
 */

#ifdef HAVE_DIX_CONFIG_H
#include <xorg-server.h>
#endif

#define XKB_IN_SERVER 1


/* I don't know exactly what header files for the `loader': */
#include <dlfcn.h>


/* the top end: invoking pipeline  and bottom end: thawing.*/
#define DEBUG_PIPELINE 1


#include "inputstr.h"
DevicePluginRec* xkb_find_plugin_class(const char *name);

Bool insert_plugin_around (DeviceIntPtr keybd, PluginInstance* plugin,
                           const char* name_of_precedent, Bool before);

int insert_plugin_after(DeviceIntPtr dev, char* name, char* around, Bool before);
