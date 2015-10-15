#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC extern
#endif

#define CNET_PROVIDES_APPLICATION_LAYER 1
#define CNET_PROVIDES_KEYBOARD 1
#define CNET_PROVIDES_WANS 1
#define CNET_PROVIDES_WLANS 1
#define CNET_PROVIDES_MOBILITY 1

#ifdef __cplusplus
EXTERNC {
#endif
#include <cnet.h>
#include "randomwalk.h"
#ifdef __cplusplus
}
#endif

EXTERNC EVENT_HANDLER(transmit);
EXTERNC EVENT_HANDLER(receive);
EXTERNC EVENT_HANDLER(collision);
EXTERNC EVENT_HANDLER(reboot_node);