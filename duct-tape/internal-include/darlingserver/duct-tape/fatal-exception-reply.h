#ifndef _DARLINGSERVER_DUCT_TAPE_FATAL_EXCEPTION_REPLY_H_
#define _DARLINGSERVER_DUCT_TAPE_FATAL_EXCEPTION_REPLY_H_

#include <stdbool.h>

#ifndef MACH_MSG_OPTION_NONE
#define MACH_MSG_OPTION_NONE 0x00000000
#endif

#ifndef MACH_RCV_TIMEOUT
#define MACH_RCV_TIMEOUT 0x00000100
#endif

#ifndef MACH_MSG_TIMEOUT_NONE
#define MACH_MSG_TIMEOUT_NONE 0
#endif

#ifndef DTAPE_FATAL_EXC_REPLY_TIMEOUT_MS
#define DTAPE_FATAL_EXC_REPLY_TIMEOUT_MS 3000
#endif

static inline bool
dtape_exception_reply_wait_is_bounded(bool fatal_exception_delivery)
{
	return fatal_exception_delivery;
}

static inline unsigned int
dtape_exception_reply_wait_option(bool fatal_exception_delivery)
{
	return dtape_exception_reply_wait_is_bounded(fatal_exception_delivery) ?
	    MACH_RCV_TIMEOUT : MACH_MSG_OPTION_NONE;
}

static inline unsigned int
dtape_exception_reply_wait_timeout(bool fatal_exception_delivery)
{
	return dtape_exception_reply_wait_is_bounded(fatal_exception_delivery) ?
	    DTAPE_FATAL_EXC_REPLY_TIMEOUT_MS : MACH_MSG_TIMEOUT_NONE;
}

#endif
