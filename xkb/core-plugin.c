/* (c) 2005-2010 Michal Maruska
 * This is the end of the pipeline.
 * This `plugin' interfaces the pipeline with core processing - grabs, xkb.
 * in ../dix/events.c */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif


#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include "inputstr.h"
#include "eventstr.h"

#include <xkbsrv.h>
#include "xkb.h"

/*
 * Note: (hypothetical) auto-repeat of shift_L could trigger AccessX actions
 * (Either mark the AR events as soft repeat, or avoid shift repeating at all.)
 * todo!
 */
static Bool
core_process_key_event(PluginInstance* plugin,
                       InternalEvent   *event,
                       Bool owner)
{
    DeviceIntPtr keybd = plugin->device;
    // CHECKEVENT(event);

    ProcessKeyboardEvent(event, keybd);
    if (owner)
        free(event);

    plugin->frozen = (keybd->deviceGrab.sync.frozen)?TRUE:FALSE;
    return plugin->frozen?PLUGIN_FROZEN:PLUGIN_NON_FROZEN;
}


static Bool
core_accept_time(PluginInstance* plugin, Time time)
{
    assert(plugin->frozen == PLUGIN_FROZEN);
    // ErrorF("bug!\n");
    return plugin->frozen?PLUGIN_FROZEN:PLUGIN_NON_FROZEN;
}

static void
core_thaw(PluginInstance* plugin, Time time)
{
    assert(plugin->prev);
    assert(PluginClass(plugin->prev)->NotifyThaw);

    plugin->frozen = FALSE;
    PluginClass(plugin->prev)->NotifyThaw(plugin->prev, time);
}



static PluginInstance*
core_plugin_init(DeviceIntPtr dev, DevicePluginRec* plugin_class)
{
    PluginInstance* plugin = calloc(1, sizeof(PluginInstance));

    plugin->device = dev;
    plugin->pclass = plugin_class;

    plugin->frozen = FALSE;
    plugin->wakeup_time = 0;	/* this is forever, unchanged */

    return plugin;
}


DevicePluginRec core_plugin_class =
{
   "core",
   core_plugin_init,
   core_process_key_event,
   core_accept_time,
   core_thaw,
   NULL, NULL,                 /* config */
   NULL,
   NULL                        /* mouse */
};
