/* (c) 2004-2010 Michal Maruska <mmaruska@gmail.com>
 *
 * Handling Requests of the X11 protocol extension.
 * Requests for configuration & status information (history) */

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


// this is xf86 specific. we should not use it.

#include "../hw/xfree86/common/xf86Module.h"
#include "../hw/xfree86/loader/loaderProcs.h"
/* handling Requests from the X11 protocol extension */

/* Copied from xkb.c fixme!*/

#define	CHK_DEVICE(dev, id, client, access_mode, lf) {          \
        int why;                                                \
        int rc = lf(&(dev), id, client, access_mode, &why);     \
        if (rc != Success) {                                    \
            client->errorValue = _XkbErrCode2(why, id);         \
            return rc;                                          \
        }                                                       \
    }

#define	CHK_KBD_DEVICE(dev, id, client, mode)                   \
    CHK_DEVICE(dev, id, client, mode, _XkbLookupKeyboard)





/* A `store' of known plugin classes:
 * Plugin Class is the implementation
 * Plugin is an instantiation of that implementation (inserted in a pipeline).
 */
typedef struct _plugin_record plugin_record;

struct _plugin_record
{
    DevicePluginRec* plugin_class;
    /* should i have a ref count ? */
    plugin_record* next;
} *plugin_store = NULL;


DevicePluginRec*
xkb_find_plugin_class(const char *name)
{
    plugin_record *pr = plugin_store;
    if (!plugin_store)
    {
        ErrorF("%s: the plugin store is empty.\n", __FUNCTION__);
        return NULL;
    }

    ErrorF("%s: searching for %s.\n", __FUNCTION__, name);
    while (pr && (pr->plugin_class) && strcmp(pr->plugin_class->name, name))
    {
        ErrorF("%s: skipping over (non-matching) %s\n", __FUNCTION__, pr->plugin_class->name);
        pr = pr->next;
    }
    return (pr)?pr->plugin_class:NULL;
}


/* Remove the plugin class from the store (of available classes).
   Next time the class is requested, a module will be loaded. */
static void
remove_plugin_class(const char *name)
{
    plugin_record **a = &plugin_store;
    if (!plugin_store)
    {
        ErrorF("%s: the plugin store is empty.\n", __FUNCTION__);
        return;
    }

    ErrorF("%s: searching for %s.\n", __FUNCTION__, name);


    while ((*a) && ((*a)->plugin_class) && strcmp((*a)->plugin_class->name, name))
    {
        ErrorF("%s: skipping over (non-matching) %s\n", __FUNCTION__,
               (*a)->plugin_class->name);
        a = &((*a)->next);
    }
    if (*a){
        ErrorF("%s: removing!\n", __FUNCTION__);
        (*a) = (*a)->next;
    }
}


/* MODULE_API  ... the plugins use this to `provide'  */
_X_EXPORT void
xkb_add_plugin_class(DevicePluginRec* plugin_class)
{
    plugin_record* new_class = calloc(1, sizeof(plugin_record));
    ErrorF("%s: adding new plugin %s\n", __FUNCTION__, plugin_class->name);
    new_class->plugin_class = plugin_class;
    new_class->next = plugin_store;
    plugin_store = new_class;
}



/*
 * `instances'
 */


static PluginInstance*
xkb_find_plugin_by_name(DeviceIntPtr keybd, char* name)
{
    PluginInstance* plugin = keybd->pipeline;
    while (plugin && strcmp(PLUGIN_NAME(plugin), name)) /* != 0*/
    {
        plugin = plugin->next;
    }
    return plugin;
}

static PluginInstance*
xkb_find_plugin_by_id(DeviceIntPtr keybd, int id)
{
    PluginInstance* plugin = keybd->pipeline;
    while (plugin && (id != plugin->id))
    {
        plugin = plugin->next;
    }
    return plugin;
}

/* return the total `length' of plugin names, and the `number' of them. */
static void
measure_plugin_names(DeviceIntPtr keybd,int *num_plugins, int *plugin_names_lenght)
{
    int n = 0;
    int len = 0;
    PluginInstance* plugin = keybd->pipeline;

    while (plugin)
    {
        n++;
        len += strlen(PLUGIN_NAME(plugin));
        plugin = plugin->next;
    };
    *num_plugins = n;
    *plugin_names_lenght = len;
}


/* concatenate & copy into STR  the names of plugins on the pipeline (of keybd) */
static void
write_plugin_names(DeviceIntPtr keybd,unsigned char *str)
/* fixme: This should take endianess of the client... */
{
    PluginInstance* plugin = keybd->pipeline;
    /* [len] [string ... _not_ 0!] [len] ... ? */
    while (plugin)
    {
        unsigned char len = strlen(PLUGIN_NAME(plugin));
        *(CARD32*)str = plugin->id; /* fixme: might be 2 bytes only.  Endianess!! */
        str = (unsigned char*)(((CARD32*)str) + 1);
        /* fixme: len < 256, as we have just 1 byte, where is that enforced!? */
        *str = len;
        str++;
        memcpy(str, PLUGIN_NAME(plugin), len);
        str += len;
        plugin = plugin->next;
    };
}


/* Remove the `exhausted' plugin from the pipeline.
 * Exhausted as it does not keep any events anymore.
 */

/* The plugin itself requests this? */
/* `PLUGIN_API' */
_X_EXPORT Bool
xkb_remove_plugin(PluginInstance* plugin)
{
    PluginInstance* prev;
    DevicePluginRec* plugin_class;
    assert (plugin);

    /* remove from the double-linked list */
    prev = plugin->prev;
    prev->next = plugin->next;
    plugin->next->prev = prev;

    plugin_class = PluginClass(plugin);
    /* deallocate the instance ! */
    PluginClass(plugin)->terminate(plugin);

    plugin = NULL;

    /* Unload the module? */
    if (plugin_class->module)
    {
        // fixme: how to remove this?
        plugin_class->ref_count --;
        if (plugin_class->ref_count == 0)
        {
#if 0
            ErrorF("%s: we cannot UnloadModule NOW!\n", __FUNCTION__);
            void* module = plugin_class->module;
            UnloadModule(module);
            /* unload the module */
#endif
        }
    }
    return TRUE;
}


static int
load_plugin(const char* filename)    /* DeviceIntPtr dev, */
{
    /* first, open the  module */
#ifdef NO_MODULE_EXTS
    /* vfb */
    return -1;
#else
    const char* dir = DEFAULT_MODULE_PATH; /* "/usr/lib/modules/xkb-plugins/" */
#if 0
    void *module;
    const XF86ModReqInfo modreq =
        {
            0,
            0,
            0,
            ABI_CLASS_INPUT,
            ABI_INPUT_VERSION,
            MOD_CLASS_INPUT
        };
    int errmaj, errmin;

    ErrorF("%s: LoadModule %s\n", __FUNCTION__, filename);
    module = LoadSubModule(filename, dir, /* fixme: not absolute! */
                           NULL, /*const char **subdirlist,*/
                           NULL, NULL, /* const char **patternlist, pointer options, */
                           &modreq, &errmaj, &errmin);
    if (!module)
    {
        /* fixme! */
        return -1;
    }
#else
    void *module;
    static const char* soext = ".so";
    static const char* dirdiv = "/";

    char* path = alloca(strlen(dir) + sizeof(dirdiv) + strlen(filename) + strlen(soext) + 1);
    strcpy(path, dir);
    strcat(path, dirdiv);
    strcat(path, filename);
    strcat(path, soext);
    module = dlopen(path, RTLD_NOW);
    if (!module)
    {
        ErrorF("%s dlopen failed: %s\n", __FUNCTION__, dlerror());
        return -1;
    }
#endif
    return Success;
#endif  /* NO_MODULE_EXTS */
};

_X_EXPORT int
insert_plugin_after(DeviceIntPtr dev, char* name, char* around, Bool before)
{
    ErrorF("%s: @ %s: %s around %s\n", __FUNCTION__, dev->name, name, around);
#if 0                           /* incompatible! */
    if (dev->pipeline)
    {
        int ret = unload_xkb_rewriting_plugin(dev);
        if (ret != Success)
            return ret;
    } else {
        /* fixme:  check the result   BadAlloc! */
        dev->pipeline = plugin = malloc(sizeof(DeviceIntRec_plugin));
        ErrorF("setting: %d\n", dev->pipeline);
        bzero (plugin, sizeof(DeviceIntRec_plugin));
    };
#endif

    DevicePluginRec* plugin_class = xkb_find_plugin_class(name);
    if (! plugin_class)
    {
        int ret;
        ErrorF("%s: the plugin is not yet known. Trying to load"
               " the plugin from module %s\n", __FUNCTION__, name);
        ret = load_plugin(name); /* dev */
        if (ret != Success)
            return ret;

        plugin_class = xkb_find_plugin_class(name);
        if (! plugin_class)
        {
            ErrorF("%s: still not found. Module loaded ok, but did not"
                   " register the expected plugin. see!\n",
                   __FUNCTION__);
            return BadAccess;
        }
    }

    if (plugin_class)
    {
        ErrorF("%s: instantiate @ %s: %s %lu\n",
                __FUNCTION__, dev->name, plugin_class->name, dev);
        PluginInstance* plugin = plugin_class->instantiate(dev, plugin_class);
        plugin->id = ++dev->pipeline_counter;
        if (plugin)
            insert_plugin_around (dev, plugin, around, before);
        else
            return BadAlloc;
    }

    return Success;
}


/* Insert a new plugin between 2 other plugins. Possibly load a module. */
int
ProcXkbSetPlugin(ClientPtr client)
{
    DeviceIntPtr        dev;
    char*  name;
    char*  around;

    REQUEST(xkbSetPluginReq);      /* this declares stuff variable to the right type ? */
    /* register type *stuff = (type *)client->requestBuffer */
    REQUEST_AT_LEAST_SIZE(xkbSetPluginReq);
    /* This is just a check. the request should be read according to its size
     *  in the `envelope' */

    /*   if ((sizeof(req) >> 2) != client->req_len) return(BadLength) */

    if (!(client->xkbClientFlags&_XkbClientInitialized))
        return BadAccess;

    CHK_KBD_DEVICE(dev, stuff->deviceSpec, client, DixGetAttrAccess);


    name = (char*) &(stuff->names);  /* we rely on the \0 ? fixme: if the filename field is not there!
                                      * but the request block is 6bytes+ 2 padding (zeroed) */
    around = ((char *) &(stuff->names)) + strlen(name) + 1;

    ErrorF("%s: @ %s: %s around %s\n", __FUNCTION__, dev->name, name, around);

    if (*name == 0)
    {
        PluginInstance* plugin;
        ErrorF("%s: we should remove %s\n", __FUNCTION__, around);
        plugin = xkb_find_plugin_by_name(dev, around);
        if (plugin) {

            if (!PluginClass(plugin)){
                ErrorF("%s: this plugin class cannot be stopped!\n", __FUNCTION__);
            } else {
                DevicePluginRec* plugin_class = PluginClass(plugin);
                PluginClass(plugin)->stop(plugin);

                if (plugin_class->module)
                {
                    ErrorF("%s: ref count now: %d\n", __FUNCTION__,
                            plugin_class->ref_count);
                    if (plugin_class->ref_count == 0)
                    {
                        /* fixme: This might be doable only later! */
                        remove_plugin_class(plugin_class->name);
                        /* unload the module */
                    }
                }
            }
        } else
            ErrorF("%s: there is no such plugin (%s) on the pipeline!\n",
                    __FUNCTION__, around);

    } else {
        int res = insert_plugin_after(dev, name, around, stuff->before);
        if (res != Success)
            return res;
    }
    return client->noClientException;
}

#define old 0
int
ProcXkbConfigPlugin(ClientPtr client, Bool send)
{
    DeviceIntPtr        dev;
#if old
    char*  plugin_name;
#endif

    PluginInstance* plugin;
#if DEBUG
    ErrorF("%s: %s\n", __FUNCTION__, send?"GET":"SET");
#endif
    REQUEST(xkbPluginConfigReq);      /* this declares stuff variable to the right type ? */
    /* register type *stuff = (type *)client->requestBuffer */
    /* fixme:   */
    REQUEST_AT_LEAST_SIZE(xkbPluginConfigReq);
    /*  this is just a check. the request should be read according to its size
     *  in the `envelope' */


    /*   if ((sizeof(req) >> 2) != client->req_len) return(BadLength) */

    if (!(client->xkbClientFlags&_XkbClientInitialized))
        return BadAccess;

    CHK_KBD_DEVICE(dev, stuff->deviceSpec, client, DixGetAttrAccess);

#if !old
    plugin = xkb_find_plugin_by_id(dev, stuff->plugin_id);
    if (!plugin)
    {
        ErrorF("%s: there is no plugin %d\n", __FUNCTION__, (int)stuff->plugin_id);
        return BadAccess;
    }
#else
    plugin_name = &(stuff->namedata);  /* we rely on the \0 ? */
    {
        char* rest;
        rest = strrchr(&stuff->namedata, 0) +1;
    }
    plugin = xkb_find_plugin_by_name(dev, plugin_name);
    if (!plugin)
    {
        ErrorF("%s: there is no plugin %s\n", __FUNCTION__, plugin_name);
        return BadAccess;
    }
#endif  /* old */


    /* todo:  xkb_plugin_send_reply */
    if (send) {
        if (PluginClass(plugin)->getconfig)
        {
            XKBpluginReply rep;
            int *return_config;

            bzero(&rep,sizeof(XKBpluginReply));
            rep.type= X_Reply;  /* ??? */
            rep.sequenceNumber= client->sequence;
            rep.length = 0;
            return_config = (int*) &rep.data00;
#if 0
            int config[5];
            config[0] = stuff->data0;
            config[1] = stuff->data1;
            config[2] = stuff->data2;
            config[3] = stuff->data3;
            config[4] = stuff->data4;
#endif
            PluginClass(plugin)->getconfig(plugin,
                                           /* fixme: wrong! *4 and SIZEOF! */
                                           (int*) &(stuff->data0), /* int[4] */
                                           return_config);
#if 0
            ErrorF("pushing ints: %d %d %d\n", return_config[0],return_config[1],return_config[2]);
            /* todo: endianess! */
            if (client->swapped) {
                register int n;
                swaps(&(rep.sequenceNumber),n);
                /* swapl(&(rep.time),n); */
                swapl(&(rep.data00),n);
            }
#else
#if 0
            memcpy(&rep.data00, return_config, sizeof(return_config));
#endif
#endif


            WriteToClient(client, SIZEOF(XKBpluginReply), (char *)&rep);
#if DEBUG
            ErrorF("%s: sent to client the value %d\n", __FUNCTION__, rep.data00);
#endif
            return client->noClientException;
        } else
        {
#if DEBUG
            ErrorF("%s: plugin %s doesn't have ->getconfig method.\n", __FUNCTION__, PLUGIN_NAME(plugin));
#endif
            return BadAccess;
        }

    } else {

        /* how to skip over the len? */
        if (PluginClass(plugin)->config)
        {
#if 0
            int config[4];
            config[0] = stuff->data0;
            config[1] = stuff->data1;
            config[2] = stuff->data2;
            config[3] = stuff->data3;
            config[4] = stuff->data4;
#endif
            PluginClass(plugin)->config(plugin,
                                        /* fixme: wrong! *4 and SIZEOF! */
                                        (int*) &(stuff->data0));
            return client->noClientException;
        }
        else
            return BadAccess;
    }
}


/* xkb.c */
/* The plugin is handed the client, and can send him XReply. */
int
ProcXkbCommandPlugin(ClientPtr client)
{
    DeviceIntPtr        dev;
    PluginInstance* plugin;
#if DEBUG
    ErrorF("%s:\n", __FUNCTION__);
#endif
    REQUEST(xkbPluginCommandReq);
    /* this declares stuff variable to the right type ? */
    /* register type *stuff = (type *)client->requestBuffer */
    /* fixme:   */
    REQUEST_AT_LEAST_SIZE(xkbPluginCommandReq);
    /*  this is just a check. the request should be read according to its size
     *  in the `envelope' */


    /*   if ((sizeof(req) >> 2) != client->req_len) return(BadLength) */

    if (!(client->xkbClientFlags&_XkbClientInitialized))
        /* fixme! */
        return BadAccess;

    CHK_KBD_DEVICE(dev, stuff->deviceSpec, client, DixGetAttrAccess);


    plugin = xkb_find_plugin_by_id(dev, stuff->plugin_id);
    if (!plugin)
    {
        ErrorF("%s: there is no plugin %ud\n", __FUNCTION__, (int) stuff->plugin_id);
        return BadAccess;
    }


    /* rest = (char*) stuff + SIZEOF(xkbPluginCommandReq); */

    /* how to skip over the len? */
    if (PluginClass(plugin)->client_command)
    {
        /* plugin _must_ send a reply!  */
        PluginClass(plugin)->client_command(client, plugin,
                                            stuff->data0,
                                            stuff->data1,
                                            stuff->data2,
                                            stuff->data3,
                                            stuff->data4);
        /* (stuff->length * 4) - SIZEOF(xkbPluginCommandReq) - (strlen(plugin_name) + 1), rest*/
        return client->noClientException;
    }
    else
        return BadAccess;
}

/* fixme:! This should have another argument: Device! */
/* Return 0 on success, */
/* `PLUGIN_API'  */
_X_EXPORT int
xkb_plugin_send_reply(ClientPtr client, PluginInstance* plugin, char* message,
                      unsigned int length /*, int subtype*/)
/* fixme: This might accept 3 CARD32 args! */
{
    /* include/extensions/XKBproto.h */
    XKBpluginReply	rep = {0};
    int pad_length = 0;

    if (length > 16383 /*((1 << 15) -1)*/)     /* todo: `max_len'  ??*/
        return -1;

    rep.type= X_Reply;
    rep.sequenceNumber= client->sequence;
    /* rep.deviceID = dev->id; */



    rep.real_len = length;
    if (length < 12)             /* 12 bytes available in the regular XReply, w/o extending bytes. */
    {
        rep.subtype = XKB_PLUGIN_SHORT;
        memcpy(&rep.data00, message, length); /* at the initial segment! */

        /* trick: */
        length = 0;
    }
    else
    {
        /*  header | message | pad|  */

        /* todo: send not only the subtype, but also the name of the plugin. */
        rep.subtype = XKB_PLUGIN_LONG;

        /* rep.lenght contains the number of 4-bytes units. So XReply should have 8. */
    }

#if 0
    /* compose the  `appendix': */
    /* the length does not count the first 32 bytes?? */
    rep.length = (SIZEOF(XKBpluginReply)-SIZEOF(xGenericReply))>>2;
    /* maybe not divisible by 4 ? */
#endif


    /* todo: use standard function to get the padded lenght! */

    rep.length = SIZEOF(XKBpluginReply) - SIZEOF(xGenericReply) + length;

    /* i think i _have_ to arrive at least at 32 bytes:  8  */
    /* 32 = SIZEOF(xGenericReply) */
    /* fixme: remove this? */
    if (rep.length < 32) {
        pad_length = 32 - rep.length;
        rep.length = 32;
    } else {
        pad_length = rep.length % 4;
        pad_length = (pad_length == 0)?0: (4-pad_length);
    }

    rep.length = (rep.length +3) >> 2; /* +3 trick! */


    ErrorF("%s: sending %d extra words\n", __FUNCTION__, (int) rep.length);
    /* 1. Header: */
    WriteToClient(client, sizeof(XKBpluginReply), (char *)&rep);
    /* 2. message: */
    WriteToClient(client, length, message);

    /* 3. pad */
    if (pad_length)
    {
        char zero_pad_buffer[3];
        /* bzero(zero_pad_buffer); */
        WriteToClient(client, pad_length, zero_pad_buffer);
    };

    return client->noClientException;
}


#define ALLOCATE_LOCAL(x) alloca(x)
#define DEALLOCATE_LOCAL(x) {}


/* copied from xkb.c */
int
ProcXkbListPipeline(ClientPtr client)
{
    DeviceIntPtr                dev;
    xkbListPipelineReply        rep;
    unsigned char *		str;
    int num_plugins;
    int plugin_names_lenght;
    int str_size;

    REQUEST(xkbListPipelineReq);
    REQUEST_AT_LEAST_SIZE(xkbListPipelineReq);

    if (!(client->xkbClientFlags&_XkbClientInitialized))
        return BadAccess;

    CHK_KBD_DEVICE(dev, stuff->deviceSpec, client, DixGetAttrAccess);

    bzero(&rep,sizeof(xkbListPipelineReply));
    rep.type= X_Reply;
    rep.deviceID = dev->id;
    rep.sequenceNumber = client->sequence;

    /* walk twice the pipeline: first count # of plugins and the sum of lenghts
     * of the names: */
    measure_plugin_names(dev, &num_plugins, &plugin_names_lenght);


    /* NUMBER LEN STRING
     *    4    1   LEN
     */
    str_size = XkbPaddedSize(plugin_names_lenght + num_plugins *
                             (4 + (sizeof (char)))); /* lenght! */
    str = (unsigned char *)ALLOCATE_LOCAL(str_size);
    /*if (!str)  out-of-memory!*/

    /* walk the second time, and write ? */
    write_plugin_names(dev, str);

    rep.nPlugins = num_plugins;
    rep.length = XkbPaddedSize(plugin_names_lenght + num_plugins * 5)   >> 2;
    /* -1 ? last \0 is useless, maybe.*/

#if DEBUG
    ErrorF("%s: %d in %d -> %d (extra %d)\n", __FUNCTION__,
           plugin_names_lenght, num_plugins, rep.length, str_size);
#endif

    if (client->swapped) {
        ErrorF("%s: swapping!\n", __FUNCTION__);
        swaps(&rep.sequenceNumber);
        swapl(&rep.length);
        swaps(&rep.nPlugins);
    }

    WriteToClient(client,SIZEOF(xkbListPipelineReply),(char *)&rep);
    WriteToClient(client, str_size, (char*)str);

    DEALLOCATE_LOCAL((char *)str);

    return client->noClientException;
}
