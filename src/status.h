#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysrepo.h"
#include <libubox/list.h>

struct release {
    char *distribution;
    char *version;
    char *revision;
    char *codename;
    char *target;
    char *description;
};

struct board {
    char *kernel;
    char *hostname;
    char *system;
    struct release *release;
};

void
print_release(struct release *r)
{
    printf("release:\n");
    printf("\tdistribution:%s\n\tversion:%s\n\trevision:%s\n\tcodename:%s\n\ttarget:%s\n\tdescription:%s\n",
           r->distribution,
           r->version,
           r->revision,
           r->codename,
           r->target,
           r->description);
}

void
print_board(struct board *b)
{
    printf("board:\n");
    printf("\tkernel:%s\n\thostname:%s\n\tsystem:%s\n",
           b->kernel, b->hostname, b->system);
    print_release(b->release);
}

struct dhcp_lease {
    struct list_head head;
    char *lease_expirey;
    char *mac;
    char *ip;
    char *name;
    char *id;
};

void
print_dhcp_lease(struct dhcp_lease *l)
{
    printf("dhcp leases:\n");
    printf("\t%s\n\t%s\n\t%s\n\t%s\n\t%s\n",
           l->lease_expirey,
           l->mac,
           l->ip,
           l->name,
           l->id);
}

struct wifi_device {
    struct list_head head;
    char *name;
    char *type;
    char *channel;
    char *macaddr;
    char *hwmode;
    char *disabled;
};

void
print_wifi_device(struct wifi_device *dev)
{
    printf("wifi-device\n");
    printf("\tname:%s\n\ttype:%s\n\tchannel:%s\n\tmacaddr:%s\n\thwmo:%s\n\tdisabled:%s\n",
           dev->name,
           dev->type,
           dev->channel,
           dev->macaddr,
           dev->hwmode,
           dev->disabled);
}

struct wifi_iface {
    struct list_head head;
    char *name;
    char *device;
    char *network;
    char *mode;
    char *ssid;
    char *encryption;
    char *maclist;
    char *macfilter;
    char *key;
};

void
print_wifi_iface(struct wifi_iface *iface)
{
    printf("wifi-iface\n");
    printf("\tname:%s\n\tdevice:%s\n\tnetwork:%s\n\tmode:%s\n\tssid:%s\n\tencryption:%s\n\tmaclist:%s\n\tmacfilter:%s\n\tkey:%s\n",
           iface->name,
           iface->device,
           iface->network,
           iface->mode,
           iface->ssid,
           iface->encryption,
           iface->maclist,
           iface->macfilter,
           iface->key);
}

struct model {
    struct list_head *wifi_devs;
    struct list_head *wifi_ifs;
    struct list_head *leases;
    struct board *board;

    struct ubus_context *ubus_ctx;
    struct uci_context *uci_ctx;
    sr_subscription_ctx_t *subscription;
};
