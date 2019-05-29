#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <uci.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <json-c/json.h>
#include "sysrepo/plugins.h"
#include "sysrepo/values.h"
#include "status.h"
#include <libubox/list.h>

#define XPATH_MAX_LEN 100
#define XPATH_MAX_LEN 100
#define UCIPATH_MAX_LEN 100

static const char *config_file = "wireless";
static const char *lease_file_path = "/tmp/dhcp.leases";

struct list_head leases = LIST_HEAD_INIT(leases);
struct list_head ifs = LIST_HEAD_INIT(ifs);
struct list_head devs = LIST_HEAD_INIT(devs);
struct  board *board;

/* Remove quotes from a string: */
/* Example: '"kernel"' -> 'kernel' */
static char *
remove_quotes(const char *str)
{
    char *unquoted;
    unquoted = strdup(str);
    unquoted = unquoted + 1;
    unquoted[strlen(unquoted) - 1] = '\0';

    return strdup(unquoted);
}

/**
 * Take pointer to string and return new string without quotes.
 * ubus call returns quotes strings and unquoted are needed.
 */
static void
fill_board(char **ref, char *name, struct json_object *r)
{
    struct json_object *o;

    json_object_object_get_ex(r, name, &o);
    *ref = remove_quotes(json_object_to_json_string(o));
}

/**
 * @brief Fill board (and release) by reacting on ubus call request.
 */
static void
system_board_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
    char *json_string;
    struct json_object *r, *t;

    fprintf(stderr, "systemboard cb\n");
    if (!msg) {
        return;
    }

    board = calloc(1, sizeof(*board));
    json_string = blobmsg_format_json(msg, true);
    r = json_tokener_parse(json_string);

    fill_board(&board->kernel, "kernel", r);
    fill_board(&board->hostname, "hostname", r);
    fill_board(&board->system, "system", r);

    struct release *release;
    release = calloc(1, sizeof(*release));

    json_object_object_get_ex(r, "release", &t);

    fill_board(&release->distribution, "distribution", t);
    fill_board(&release->version, "version", t);
    fill_board(&release->revision, "revision", t);
    fill_board(&release->codename, "codename", t);
    fill_board(&release->target, "target", t);
    fill_board(&release->description, "description", t);

    board->release = release;

    print_board(board);

    json_object_put(r);
    free(json_string);
}

/* Fill board with ubus information. */
static int
parse_board(struct ubus_context *ctx, struct board *board)
{
    uint32_t id = 0;
    struct blob_buf buf = {0,};
    int rc = SR_ERR_OK;

    blob_buf_init(&buf, 0);

    rc = ubus_lookup_id(ctx, "system", &id);

    if (rc) {
        fprintf(stderr, "ubus [%d]: no object system\n", rc);
        goto out;
    }
    rc = ubus_invoke(ctx, id, "board", buf.head, system_board_cb, NULL, 5000);
    if (rc) {
        fprintf(stderr, "ubus [%d]: no object board\n", rc);
        goto out;
    }

  out:
    blob_buf_free(&buf);

    return rc;
}

static void
parse_lease(char *line, struct dhcp_lease *leases)
{
    char *ptr;
    char *tokens[5];
    int i;

    ptr = strtok(line, " ");
    tokens[0] = ptr;
    for(i = 1; ptr != NULL && i < 5; i++) {
        ptr = strtok(NULL, " ");
        tokens[i] = ptr;
    }

    i = 0;
    leases->lease_expirey = strdup(tokens[i++]);
    leases->mac = strdup(tokens[i++]);
    leases->ip = strdup(tokens[i++]);
    leases->name = strdup(tokens[i++]);
    leases->id = strdup(tokens[i++]);
    leases->id[strcspn(leases->id, "\n")] = 0;
}

/**
 * @brief Parse OpenWRTs dhcpcd.leases file.
 */
static int
parse_leases_file(FILE *lease_fd, struct list_head *leases)
{
    struct dhcp_lease *lease;
    size_t n_lease = 0;
    char *line = NULL;
    int n_read = 0;
    size_t len = 0;

    while((n_read = getline(&line, &len, lease_fd)) > 0) {
        lease = calloc(1, sizeof(*lease));
        parse_lease(line, lease);
        list_add(&lease->head, leases);
        n_lease++;
    }

    if (n_lease < 1) {
        fprintf(stderr, "Lease file is empty.\n");
        return -1;
    }

    if (line) {
        free(line);
    }

    return 0;
}

static void
parse_wifi_device(struct uci_section *s, struct wifi_device *wifi_dev)
{
    struct uci_element *e;
    struct uci_option *o;
    char *name, *value;

    uci_foreach_element(&s->options, e) {
        o = uci_to_option(e);
        name = e->name;
        value = o->v.string;
        if        (!strcmp("name", name)) {
            wifi_dev->name = strdup(value);
        } else if (!strcmp("type", name)) {
            wifi_dev->type = strdup(value);
        } else if (!strcmp("channel", name)) {
            wifi_dev->channel = strdup(value);
        } else if (!strcmp("macaddr", name)) {
            wifi_dev->macaddr = strdup(value);
        } else if (!strcmp("hwmode", name)) {
            wifi_dev->hwmode = strdup(value);
        } else if (!strcmp("disabled", name)) {
            wifi_dev->disabled = strdup(value);
        }
    }
}

static void
parse_wifi_iface(struct uci_section *s, struct wifi_iface *wifi_if)
{
    struct uci_element *e;
    struct uci_option *o;
    char *name, *value;

    wifi_if->name = strdup(s->e.name);

    uci_foreach_element(&s->options, e) {
        o = uci_to_option(e);
        name = o->e.name;
        value = o->v.string;
        if        (!strcmp("name", name)) {
            wifi_if->name = strdup(value);
        } else if (!strcmp("device", name)) {
            wifi_if->device= strdup(value);
        } else if (!strcmp("network", name)) {
            wifi_if->network = strdup(value);
        } else if (!strcmp("mode", name)) {
            wifi_if->mode = strdup(value);
        } else if (!strcmp("ssid", name)) {
            wifi_if->ssid = strdup(value);
        } else if (!strcmp("encryption", name)) {
            wifi_if->encryption = strdup(value);
        } else if (!strcmp("maclist", name)) {
            wifi_if->maclist = strdup(value);
        } else if (!strcmp("macfilter", name)) {
            wifi_if->macfilter = strdup(value);
        } else if (!strcmp("key", name)) {
            wifi_if->key = strdup(value);
        } else {
            fprintf(stderr, "unexpected option: %s:%s\n", name, value);
        }
    }
}

/**
 * @breif Get information about WIFI devices and interfaces.
 *
 * @param[in] ctx UCI context needed for iterating over configurations.
 * @param[out] ifs List of interfaces.
 * @param[out] devs List of devices.
 */
static int
status_wifi(struct uci_context *ctx, struct list_head *ifs, struct list_head *devs)
{
    struct uci_package *package = NULL;
    struct wifi_iface *wifi_if;
    struct wifi_device *wifi_dev;
    struct uci_element *e;
    struct uci_section *s;
    int rc;

    rc = uci_load(ctx, config_file, &package);
    if (rc != UCI_OK) {
        fprintf(stderr, "No configuration (package): %s\n", config_file);
        goto out;
    }

    uci_foreach_element(&package->sections, e) {
        s = uci_to_section(e);
        wifi_if = calloc(1, sizeof(*wifi_if));
        wifi_dev = calloc(1, sizeof(*wifi_dev));

        if (!strcmp(s->type, "wifi-iface") || !strcmp(s->type, "'wifi-iface'")) {
            parse_wifi_iface(s, wifi_if);
            list_add(&wifi_if->head, ifs);
        } else if (!strcmp("wifi-device", s->type) || !strcmp(s->type, "'wifi-device'")) {
            parse_wifi_device(s, wifi_dev);
            list_add(&wifi_dev->head, devs);

        } else {
            fprintf(stderr, "Unexpected section: %s\n", s->type);
        }
    }

    struct wifi_iface *if_it;
    list_for_each_entry(if_it, ifs, head) {
        print_wifi_iface(wifi_if);
    }

    struct wifi_device *dev_it;
    list_for_each_entry(dev_it, devs, head) {
        print_wifi_device(wifi_dev);
    }

  out:
    if (package) {
        uci_unload(ctx, package);
    }

    return true;
}


static int
set_value_str(sr_session_ctx_t *sess, char *val_str, char *set_path)
{
    sr_val_t val = { 0, };

    val.type = SR_STRING_T;
    val.data.string_val = val_str;

    int rc = sr_set_item(sess, set_path, &val, SR_EDIT_DEFAULT);

    return rc;
}

/**
 * Update Sysrepo data-store with given run-time values.
 */
static int
set_values(sr_session_ctx_t *sess, struct board *board,
           struct list_head *wifi_dev,
           struct list_head *wifi_if,
           struct list_head *leases)

{
    int rc = SR_ERR_OK;
    char xpath[XPATH_MAX_LEN];

    /* Setting kernel values. */
    if (board->kernel){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/kernel");
        if (SR_ERR_OK != set_value_str(sess, board->kernel, xpath)) {
            goto cleanup;
        }
    }

    if (board->hostname){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/hostname");
        if (SR_ERR_OK != set_value_str(sess, board->hostname, xpath)) {
            goto cleanup;
        }
    }

    if (board->system){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/system");
        if (SR_ERR_OK != set_value_str(sess, board->system, xpath)) {
            goto cleanup;
        }
    }

    if (board->release->distribution){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/release/distribution");
        if (SR_ERR_OK != set_value_str(sess, board->release->distribution, xpath)) {
            goto cleanup;
        }
    }

    if (board->release->version){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/release/version");
        if (SR_ERR_OK != set_value_str(sess, board->release->version, xpath)) {
            goto cleanup;
        }
    }

    if (board->release->revision){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/release/revision");
        if (SR_ERR_OK != set_value_str(sess, board->release->revision, xpath)) {
            goto cleanup;
        }
    }
    if (board->release->codename){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/release/codename");
        if (SR_ERR_OK != set_value_str(sess, board->release->codename, xpath)) {
            goto cleanup;
        }
    }

    if (board->release->target){
        snprintf(xpath, XPATH_MAX_LEN, "/status:board/release/target");
        if (SR_ERR_OK != set_value_str(sess, board->release->target, xpath)) {
            goto cleanup;
        }
    }

    /* Setting leases values. */
    struct dhcp_lease *l;
    list_for_each_entry(l, leases, head) {
        if (!l->id || !strcmp("", l->id) ) break;

        if (l->lease_expirey){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:dhcp/dhcp-leases[id='%s']/lease-expirey", l->id);
            if (SR_ERR_OK != set_value_str(sess, l->lease_expirey, xpath)) {
                goto cleanup;
            }
        }

        if (l->mac){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:dhcp/dhcp-leases[id='%s']/mac", l->id);
            if (SR_ERR_OK != set_value_str(sess, l->mac, xpath)) {
                goto cleanup;
            }
        }

        if (l->ip){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:dhcp/dhcp-leases[id='%s']/ip", l->id);
            if (SR_ERR_OK != set_value_str(sess, l->ip, xpath)) {
                goto cleanup;
            }
        }

        if (l->name){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:dhcp/dhcp-leases[id='%s']/name", l->id);
            if (SR_ERR_OK != set_value_str(sess, l->name, xpath)) {
                goto cleanup;
            }
        }

    }

    /* Setting wifi values. */
    struct wifi_device *d;
    list_for_each_entry(d, wifi_dev , head) {
        if (!d->name|| !strcmp("", d->name) ) break;

        if (d->type){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-device[name='%s']/type", d->name);
            if (SR_ERR_OK != set_value_str(sess, d->type, xpath)) {
                goto cleanup;
            }
        }

        if (d->channel){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-device[name='%s']/type", d->name);
            if (SR_ERR_OK != set_value_str(sess, d->channel, xpath)) {
                goto cleanup;
            }
        }

        if (d->macaddr){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-device[name='%s']/macaddr", d->name);
            if (SR_ERR_OK != set_value_str(sess, d->macaddr, xpath)) {
                goto cleanup;
            }
        }

        if (d->hwmode){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-device[name='%s']/hwmode", d->name);
            if (SR_ERR_OK != set_value_str(sess, d->hwmode, xpath)) {
                goto cleanup;
            }
        }

        if (d->disabled){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-device[name='%s']/disabled", d->name);
            if (SR_ERR_OK != set_value_str(sess, d->disabled, xpath)) {
                goto cleanup;
            }
        }
    }


    /* Setting wifi interfaces values. */
    struct wifi_iface *i;
    list_for_each_entry(i, wifi_if, head) {
        if (!i->name|| !strcmp("", i->name) ) break;

        print_wifi_iface(i);
        if (i->device){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/device", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->device, xpath)) {
                goto cleanup;
            }
        }

        if (i->network){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/network", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->network, xpath)) {
                goto cleanup;
            }
        }

        if (i->mode){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/mode", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->mode, xpath)) {
                goto cleanup;
            }
        }

        if (i->ssid){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/ssid", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->ssid, xpath)) {
                goto cleanup;
            }
        }

        if (i->encryption){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/encryption", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->encryption, xpath)) {
                goto cleanup;
            }
        }

        if (i->maclist){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/maclist", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->maclist, xpath)) {
                goto cleanup;
            }
        }

        if (i->macfilter){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/macfilter", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->macfilter, xpath)) {
                goto cleanup;
            }
        }

        if (i->key){
            snprintf(xpath, XPATH_MAX_LEN,
                     "/status:wifi/wifi-iface[name='%s']/key", i->name);
            if (SR_ERR_OK != set_value_str(sess, i->key, xpath)) {
                goto cleanup;
            }
        }
    }

    /* Commit values set. */
    rc = sr_commit(sess);
    if (SR_ERR_OK != rc) {
        fprintf(stderr, "Error by sr_commit: %s\n", sr_strerror(rc));
        goto cleanup;
    }

  cleanup:
    return rc;
}

/**
 * @brief Initialize necessary information describing the model.
 *
 * @param[out] ctx Model to fill.
 */
static void
init_data(struct model *ctx)
{
    FILE *fd_lease = NULL;

    ctx->uci_ctx = uci_alloc_context();
    if (!ctx->uci_ctx) {
        fprintf(stderr, "Cant allocate uci\n");
        goto out;
    }

    ctx->ubus_ctx = ubus_connect(NULL);
    if (ctx->ubus_ctx == NULL) {
        fprintf(stderr, "Cant allocate ubus\n");
        goto out;
    }

    parse_board(ctx->ubus_ctx, board);
    status_wifi(ctx->uci_ctx, ctx->wifi_ifs, ctx->wifi_devs);

    fd_lease = fopen(lease_file_path, "r");
    if (fd_lease == NULL) {
        goto out;
    }
    parse_leases_file(fd_lease, ctx->leases);

    ctx->board = board;
    struct wifi_iface *if_it;
    list_for_each_entry(if_it, ctx->wifi_ifs, head) {
        print_wifi_iface(if_it);
    }

  out:
    if (fd_lease) {
        fclose(fd_lease);
    }
}

/**
 * @brief Client-defined validation check triggered on Sysrepo module change.
 *
 * @param[in] session
 * @param[in] change_path xpath for change events.
 *
 * @return[out] SR_ERR_OK on success otherwise some Sysepo error code.
 */
static int
validate_changes(sr_session_ctx_t *session, char *change_path)
{
    int rc = SR_ERR_OK;
    sr_val_t *old_value = NULL;
    sr_val_t *new_value = NULL;
    sr_change_oper_t oper;
    sr_change_iter_t *it = NULL;

    fprintf(stderr, "=============== validating changes ================" "\n");

    rc = sr_get_changes_iter(session, change_path , &it);
    if (SR_ERR_OK != rc) {
        fprintf(stderr, "Get changes iter failed for xpath %s", change_path);
        goto cleanup;
    }

    while (SR_ERR_OK == sr_get_change_next(session, it, &oper, &old_value, &new_value)) {

        if (strstr(new_value->xpath, "board") || strstr(new_value->xpath, "dhcp_leases")) {
            fprintf(stderr, "Can not change board information.\n");

            rc = SR_ERR_VALIDATION_FAILED;
            break;
        }

        sr_free_val(old_value);
        sr_free_val(new_value);
    }

  cleanup:
    sr_free_change_iter(it);
    sr_free_val(old_value);
    sr_free_val(new_value);

    return rc;
}

/**
 * @brief Submit UCI option.
 *
 * @param[in] ctx Context used for looking up and setting UCI objects.
 * @param[in] str_opt Options key.
 * @param[in] str_val Options value.
 * @param[fmt] fmt Format for path identifier used in UCI.
 *
 * @return UCI error code, UCI_OK on success. UCI_ERR_INVAL on empty str_val parameter.
 */
static int
submit_to_uci(struct uci_context *ctx, char *str_opt, char *str_val, char *fmt)
{
    int rc = UCI_OK;
    struct uci_ptr up;
    char ucipath[UCIPATH_MAX_LEN];

    if (!str_val) {
        return UCI_ERR_INVAL;
    }

    sprintf(ucipath, fmt, str_opt, str_val);

    if ((rc = uci_lookup_ptr(ctx, &up, ucipath, true)) != UCI_OK) {
        fprintf(stderr, "Nothing found on UCI path.\n");
        goto exit;
    }

    if ((rc = uci_set(ctx, &up)) != UCI_OK) {
        fprintf(stderr, "Could not set UCI value [%s] for path [%s].\n", str_val, ucipath);
        goto exit;
    }

  exit:
    return rc;
}

static int
wifi_devs_to_uci(struct uci_context *ctx, struct list_head *devs)
{
    int rc = UCI_OK;
    const char *fmt = "wireless.@wifi-device[%d].%s=%s";
    char fmt_named[UCIPATH_MAX_LEN];
    int n_ifs = 0;

    struct wifi_device *d;
    list_for_each_entry(d, devs, head) {

        sprintf(fmt_named, fmt, n_ifs, "%s", "%s");

        rc = submit_to_uci(ctx, "name", d->name, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "type", d->type, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "channel", d->channel, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "macaddr", d->macaddr, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "hwmode", d->hwmode, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "disabled", d->disabled, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }
    }

  exit:
    return rc;
}

static int
wifi_ifs_to_uci(struct uci_context *ctx, struct list_head *ifs)
{
    int rc = UCI_OK;
    const char *fmt = "wireless.@wifi-iface[%d].%s=%s";
    char fmt_named[UCIPATH_MAX_LEN];
    int n_ifs = 0;

    struct wifi_iface *i;
    list_for_each_entry(i, ifs, head) {

        sprintf(fmt_named, fmt, n_ifs, "%s", "%s");

        rc = submit_to_uci(ctx, "name", i->name, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "device", i->device, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "network", i->network, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "mode", i->mode, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "ssid", i->ssid, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "encryption", i->encryption, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "maclist", i->maclist, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "macfilter", i->macfilter, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }

        rc = submit_to_uci(ctx, "key", i->key, fmt_named);
        if (UCI_OK != rc) {
            goto exit;
        }
    }

  exit:
    return rc;
}

/**
 * @brief Commit run-time data to UCI configuration files.
 *
 * @param[in] model Model with current run-time data
 * @return UCI error code. UCI_OK on success.
 */
static int
commit_to_uci(struct model *model)
{
    int rc = UCI_OK;

    struct uci_package *up = NULL;
    struct uci_context *ctx = uci_alloc_context();

    rc = uci_load(ctx, "asterisk", &up);
    if (rc != UCI_OK) {
        fprintf(stderr, "No configuration (package): %s\n", "asterisk");
        return rc;
    }

    rc = wifi_ifs_to_uci(ctx, model->wifi_ifs);
    if (UCI_OK != rc) {
        fprintf(stderr, "wifi_ifs_to_uci error %d\n", rc);
    }

    rc = wifi_devs_to_uci(ctx, model->wifi_devs);
    if (UCI_OK != rc) {
        fprintf(stderr, "wifi_devs_to_uci error %d\n", rc);
    }

    rc = uci_commit(ctx, &up, false);
    if (UCI_OK != rc) {
        fprintf(stderr, "trunk_to_uci error %d\n", rc);
    }

    /* Restart network service. */
    rc = system("/etc/init.d/network restart");
    if (0 > rc) {
      fprintf(stderr, "Can't restart 'network service' %d\n", rc);
    }

    return rc;
}

/*
 * Function is called by engine two times, first time to validate the changes, and second time
 * to apply validated.
 */
static int
module_change_cb(sr_session_ctx_t *session, const char *module_name,
                 sr_notif_event_t event, void *private_ctx)
{
    char change_path[XPATH_MAX_LEN] = {0,};

    fprintf(stderr, "=============== module has changed ================" "%d:%s\n", event, module_name);
    snprintf(change_path, XPATH_MAX_LEN, "/%s:*", module_name);

    switch (event) {
    case SR_EV_VERIFY:
        return validate_changes(session, change_path);
    case SR_EV_APPLY:
        return commit_to_uci((struct model *) private_ctx);
    default:
        printf("Changes aborted with event %d\n", event);
        return SR_ERR_OK;
    }
}

/*
 * Initialize plugin with necessary information and store it in the private context usable by
 * engines callbacks.
 * Subscribe module_change callback.
 */
int
sr_plugin_init_cb(sr_session_ctx_t *session, void **private_ctx)
{
    sr_subscription_ctx_t *subscription = NULL;
    int rc = SR_ERR_OK;

    struct model *model = calloc(1, sizeof(*model));
    model->leases = &leases;
    model->wifi_ifs = &ifs;
    model->wifi_devs = &devs;
    model->ubus_ctx = NULL;
    model->uci_ctx = NULL;
    fprintf(stderr, "SR PLUGIN INIT CB\n");

    init_data(model);
    set_values(session, model->board, model->wifi_devs, model->wifi_ifs, model->leases);

    *private_ctx = model;

    rc = sr_module_change_subscribe(session, "status", module_change_cb, *private_ctx,
                                    0, SR_SUBSCR_DEFAULT, &subscription);
    if (SR_ERR_OK != rc) {
        fprintf(stderr, "Module change error.\n");
        goto error;
    }

    model->subscription = subscription;

    return SR_ERR_OK;

  error:
    if (subscription) {
        sr_unsubscribe(session, subscription);
    }
    if (model) {
        free(model);
    }

    return rc;
}

void
sr_plugin_cleanup_cb(sr_session_ctx_t *session, void *private_ctx)
{
    /* subscription was set as our private context */
    struct model *model = (struct model *) private_ctx;
    if (!model) {
        return;
    }
    if (model->subscription) {
        sr_unsubscribe(session, model->subscription);
    }
    if (model->ubus_ctx) {
        ubus_free(model->ubus_ctx);
    }
    if (model->uci_ctx) {
        uci_free_context(model->uci_ctx);
    }
    free(model);
}

#ifdef DEBUG
volatile int exit_application = 0;

static void
sigint_handler(int signum)
{
    fprintf(stderr, "Sigint called, exiting...\n");
    exit_application = 1;
}

int
main(int argc, char *argv[])
{
    sr_conn_ctx_t *connection = NULL;
    sr_session_ctx_t *session = NULL;
    int rc = SR_ERR_OK;

    /* connect to sysrepo */
    rc = sr_connect("sip", SR_CONN_DEFAULT, &connection);
    if (SR_ERR_OK != rc) {
        fprintf(stderr, "Error by sr_connect: %s\n", sr_strerror(rc));
        goto cleanup;
    }

    /* start session */
    rc = sr_session_start(connection, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    if (SR_ERR_OK != rc) {
        fprintf(stderr, "Error by sr_session_start: %s\n", sr_strerror(rc));
        goto cleanup;
    }

    void *ptr = NULL;
    sr_plugin_init_cb(session, &ptr);

    /* loop until ctrl-c is pressed / SIGINT is received */
    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
    while (!exit_application) {
        sleep(1000);  /* or do some more useful work... */
    }

  cleanup:
    sr_plugin_cleanup_cb(session, ptr);

}
#endif
