#ifndef _COLOR_DEBUG_H_
#define _COLOR_DEBUG_H_

#define USE_COLORS 1



#if USE_COLORS
#define color_reset "\x1b[0m"
/* repeat: */
#define repeat_color "\x1b[42;30m"
#define key_color   "\x1b[01;32;41m"
/* events.c */
#define proc_color "\x1b[43;30m"
#define event_color "\x1b[46;30m"
#define color_green "\x1b[43;30m"  /* i have */

#else  /* NO USE_COLORS */

#define color_reset ""
/* repeat: */
#define repeat_color ""
#define key_color  ""
/* events: */
#define proc_color ""
#define event_color ""
#define color_green ""
#define color_reset ""

#endif	/* USE_COLORS */

#endif	/* _COLOR_DEBUG_H_ */
