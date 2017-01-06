/* (c) 2004-2010 Michal Maruska <mmaruska@gmail.com>
 *
 * Pipeline invocation
 */

/* Note: there should not be any freezing plugin at the beginning of the pipeline */

#include "pipeline.h"

/* `PLUGIN_API' ... means the function needs to be exported, so that plugins can use it! */
/* `MODULE_API' ....This is for modules (with a plugin). Add it to exported! */



/* mmc: is it still needed now that I use the X loader? */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include "inputstr.h"
#include <events.h>
#include <eventstr.h>

#define DEBUG_timeout 0         /* block handler */
#define DEBUG 0

#define XKBSRV_NEED_FILE_FUNCS
#define XKB_IN_SERVER 1
#include <xkbsrv.h>
#include "xkb.h"



/***  Advance declarations:  */
void xkb_pipeline_init(DeviceIntPtr pXDev);

static void start_plugin_pipeline(InternalEvent *event, DeviceIntPtr keybd);
static void thaw_pipeline_end(DeviceIntPtr keybd);
static void set_timeout(void *blockData, void *timeout, Time now);
static void push_time_on_keyboard(DeviceIntPtr keybd, Time upper_bound);


/* fixme: in pipeline.h?  */
extern DevicePluginRec queue_plugin_class;
extern DevicePluginRec core_plugin_class;
extern DevicePluginRec ar_plugin_class;

/* Entry from xkbInit.c */
void
xkb_init_pipeline(DeviceIntPtr device)
{
    PluginInstance *queue_plugin;
    PluginInstance *ar_plugin;
    DevicePluginRec* plugin_class;

    assert(!device->pipeline);
    /* let's build this pipeline:
     *
     *       queue -> AR -> core
     */
    device->pipeline = core_plugin_class.instantiate(device, &core_plugin_class);
    device->pipeline->id = device->pipeline_counter = 0;

    /* todo: Every time we instantiate, we have to increase the global counter! */

    queue_plugin = queue_plugin_class.instantiate(device, &queue_plugin_class);
    queue_plugin->id = ++(device->pipeline_counter);
    // this is append(device, plugin)
    insert_plugin_around(device,
                         queue_plugin,
                         "core", INSERT_BEFORE);

    // if fork is known, add it.
    plugin_class = xkb_find_plugin_class("fork");
    if (plugin_class)
    {
        PluginInstance* plugin = plugin_class->instantiate(device, plugin_class);
        plugin->id = ++device->pipeline_counter;
        if (plugin)
            insert_plugin_around(device, plugin,
                                 "core", INSERT_BEFORE);
        else
            return; // BadAlloc;
        // and based on options populate the forking table?
    }

    ar_plugin = ar_plugin_class.instantiate(device, &ar_plugin_class);
    ar_plugin->id = ++(device->pipeline_counter);
    insert_plugin_around(device,
                         ar_plugin,
                         "core", INSERT_BEFORE);

    /* now we have to re-route the processing: this overwrites values set in ../dix/devices.c
       in _AddInputDevice */
    device->public.processInputProc = device->public.enqueueInputProc =
        device->public.realInputProc = start_plugin_pipeline;
    device->public.thawProc = thaw_pipeline_end;
    device->public.pushTimeProc = push_time_on_keyboard;


    /* we need to interrupt the Select system call at the time requested by the first processor.
     * WaitForSomething consults only
     * timers, and blockHandlers to set such timeout. So i register one! */
    RegisterTimeBlockAndWakeupHandlers(set_timeout, // (TimeBlockHandlerProcPtr)
                                       (TimeWakeupHandlerProcPtr) NoopDDA, device);
}



static void
push_time_on_keyboard(DeviceIntPtr keybd, Time upper_bound)
{
    /* assert (keybd->pipeline);*/
    PluginInstance* plugin = keybd->pipeline;

    if (keybd->time < upper_bound) { /* no event  */
        if (plugin->wakeup_time <= upper_bound) /* recent */
            PluginClass(plugin)->ProcessTime (plugin, upper_bound);
        else {
#if DEBUG_timeout
            ErrorF("%s: time is insufficient. wait %u, b/c the plugin wants %u, "
                   "we have (only) %u as upper bound of inactivity\n",
                   __FUNCTION__,
                   plugin->wakeup_time - upper_bound,
                   plugin->wakeup_time, upper_bound);
#endif
        }
    }
    /* ErrorF("%s: no need to push. Some event was processed this time\n",
     * __FUNCTION__); */
}



/* this `blockHandler' looks at the pipeline's first processor to see what other
   time point in future is significant, so
 * the caller, WaitForSomething, sets such timeout in the Select system call.
 * the time is returned in the WTP. */

/* wtp can be NULL. Hence I keep `static' waittime! */
static void
set_timeout(void *blockData, void *timeoutData, Time now)
// so timeout is CARD32* !!!
{
    const DeviceIntPtr keybd = blockData;
    // struct timeval* wt = *wtp;
    // Time is ... /usr/include/X11/X.h:77:typedef CARD32
    // which is 64 bit!
    Time *timeout = (Time*) timeoutData;

    const DeviceIntPtr master = (keybd) ? GetMaster(keybd, MASTER_ATTACHED) : NULL;

    if ((timeout) && master && (master->pipeline->wakeup_time != 0)) { /* 0 - no timeout! */
        Time deviceTimeout = (master->pipeline->wakeup_time <= now)? 0 :
            master->pipeline->wakeup_time - now;

        if (deviceTimeout < *timeout) {
            // calculate our timeout, and if lesser, then propagate:
            *timeout = deviceTimeout;
        }
    }
}

/*
 * given plugins/processors A and B, such that:
 *
 * and the pipeline:  .c <--> A <--> d......
 *
 * a<->b<->c or  d---b<--->a---c
 *
 * fixme: plugins should be told about a modification of the neighbour?
 */
Bool
insert_plugin_around (DeviceIntPtr keybd, PluginInstance* plugin,
                      const char* name_of_precedent, Bool before)
{
    PluginInstance* prev;
    // bug!    assert (plugin->dev == keybd);
    /* find it... */
    assert (keybd->pipeline);

    prev = keybd->pipeline;
    while (prev && strcmp(PLUGIN_NAME(prev), name_of_precedent)) /* != 0*/
        prev = prev->next;

    if (prev) {
        switch (before) {
            case INSERT_BEFORE:
            {
                PluginInstance* prev_prev;
                prev_prev = prev->prev;

                /*name_of_precedent */
                /* need a lock? */
                plugin->next = prev;
                prev->prev = plugin;

                if (prev_prev) {
                    prev_prev->next = plugin;
                    plugin->prev = prev_prev;
                } else {
                    keybd->pipeline = plugin;
                    plugin->prev = NULL;
                }
                break;
            }
            case INSERT_AFTER:
            {
                /*name_of_precedent */
                /* need a lock? */
                plugin->next = prev->next;
                prev->next = plugin;

                plugin->prev = prev;

                if (plugin->next)
                    plugin->next->prev = plugin;

                break;
            }
        }; /* switch */
    } else {
        ErrorF ("%s: %s not found!\n", __FUNCTION__, name_of_precedent);
    }
    return (prev != NULL);
}

/* this is the interface to the beginning of old key-event processing:
 * just when we take an event from xf86EventQueue */
static void
start_plugin_pipeline(InternalEvent *event, DeviceIntPtr keybd)
{
    PluginInstance* plugin = keybd->pipeline;

    if (plugin && PluginClass(plugin)->ProcessEvent && !plugin_frozen(plugin))
        /* the event is still owned by us. */
        PluginClass(plugin)->ProcessEvent(plugin, event, CALLER_OWNS);
    /* neither we are the owner. it's in a static array! */
    else {
        assert (!plugin);
        ErrorF("%s: no keyboard pipeline processor available for events. Dropping them!\n",
               __FUNCTION__);
      }
}


/* interfaces the end of pipeline to the core key-event processing: AllowSome */
static void
thaw_pipeline_end(DeviceIntPtr keybd) /* CARD32 time */
{
  PluginInstance* plugin = keybd->pipeline;
   assert (keybd->pipeline);

   /* go the the end of chain, and thaw it? */
   while (plugin->next)
      plugin = plugin->next;

   assert (PluginClass(plugin)->NotifyThaw);
   PluginClass(plugin)->NotifyThaw(plugin, 0); /* fixme: time! */
}


void
pipeline_init_plugins(void)
{
    // plugin_store = NULL;
}
