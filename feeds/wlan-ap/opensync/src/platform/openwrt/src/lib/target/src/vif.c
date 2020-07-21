/*
Copyright (c) 2017, Plume Design Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
   3. Neither the name of the Plume Design Inc. nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Plume Design Inc. BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>

#include <uci.h>
#include <uci_blob.h>

#include "log.h"
#include "const.h"
#include "target.h"
#include "evsched.h"
#include "radio.h"
#include "vif.h"
#include "vlan.h"
#include "nl80211.h"
#include "utils.h"

#define MODULE_ID LOG_MODULE_ID_VIF
#define UCI_BUFFER_SIZE 80

extern struct blob_buf b;

enum {
	WIF_ATTR_DEVICE,
	WIF_ATTR_IFNAME,
	WIF_ATTR_INDEX,
	WIF_ATTR_MODE,
	WIF_ATTR_SSID,
	WIF_ATTR_BSSID,
	WIF_ATTR_ENCRYPTION,
	WIF_ATTR_KEY,
	WIF_ATTR_DISABLED,
	WIF_ATTR_HIDDEN,
	WIF_ATTR_ISOLATE,
	WIF_ATTR_NETWORK,
	WIF_ATTR_SERVER,
	WIF_ATTR_PORT,
	WIF_ATTR_AUTH_SECRET,
	WIF_ATTR_IEEE80211R,
	WIF_ATTR_IEEE80211W,
	WIF_ATTR_MOBILITY_DOMAIN,
	WIF_ATTR_FT_PSK_LOCAL,
	WIF_ATTR_UAPSD,
	WIF_ATTR_VLAN_ID,
	WIF_ATTR_VID,
	WIF_ATTR_MACLIST,
	WIF_ATTR_MACFILTER,
	WIF_ATTR_RATELIMIT,
	WIF_ATTR_URATE,
	WIF_ATTR_DRATE,
	WIF_ATTR_CURATE,
	WIF_ATTR_CDRATE,
	__WIF_ATTR_MAX,
};

static const struct blobmsg_policy wifi_iface_policy[__WIF_ATTR_MAX] = {
	[WIF_ATTR_DEVICE] = { .name = "device", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_MODE] = { .name = "mode", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_IFNAME] = { .name = "ifname", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_INDEX] = { .name = "index", .type = BLOBMSG_TYPE_INT32 },
	[WIF_ATTR_SSID] = { .name = "ssid", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_BSSID] = { .name = "bssid", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_ENCRYPTION] = { .name = "encryption", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_KEY] = { .name = "key", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_DISABLED] = { .name = "disabled", .type = BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_HIDDEN] = { .name = "hidden", .type = BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_ISOLATE] = { .name = "isolate", .type = BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_NETWORK] = { .name = "network", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_SERVER] = { .name = "server", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_PORT] = { .name = "port", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_AUTH_SECRET] = { .name = "auth_secret", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_IEEE80211R] = { .name = "ieee80211r", BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_IEEE80211W] = { .name = "ieee80211w", BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_MOBILITY_DOMAIN] = { .name = "mobility_domain", BLOBMSG_TYPE_STRING },
	[WIF_ATTR_FT_PSK_LOCAL] = { .name = "ft_psk_generate_local" ,BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_UAPSD] = { .name = "uapsd", BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_VLAN_ID] = { .name = "vlan_id", BLOBMSG_TYPE_INT32 },
	[WIF_ATTR_VID] = { .name = "vid", BLOBMSG_TYPE_INT32 },
	[WIF_ATTR_MACFILTER]  = { .name = "macfilter", .type = BLOBMSG_TYPE_STRING },
	[WIF_ATTR_MACLIST]  = { .name = "maclist", .type = BLOBMSG_TYPE_ARRAY },
	[WIF_ATTR_RATELIMIT] = { .name = "rlimit", BLOBMSG_TYPE_BOOL },
	[WIF_ATTR_URATE] = { .name = "urate", BLOBMSG_TYPE_STRING },
	[WIF_ATTR_DRATE] = { .name = "drate", BLOBMSG_TYPE_STRING },
	[WIF_ATTR_CURATE] = { .name = "curate", BLOBMSG_TYPE_STRING },
	[WIF_ATTR_CDRATE] = { .name = "cdrate", BLOBMSG_TYPE_STRING },
};

const struct uci_blob_param_list wifi_iface_param = {
	.n_params = __WIF_ATTR_MAX,
	.params = wifi_iface_policy,
};

static struct vif_crypto {
	char *uci;
	char *encryption;
	char *mode;
	int enterprise;
} vif_crypto[] = {
	{ "psk", OVSDB_SECURITY_ENCRYPTION_WPA_PSK, OVSDB_SECURITY_MODE_WPA1, 0 },
	{ "psk2", OVSDB_SECURITY_ENCRYPTION_WPA_PSK, OVSDB_SECURITY_MODE_WPA2, 0 },
	{ "psk-mixed", OVSDB_SECURITY_ENCRYPTION_WPA_PSK, OVSDB_SECURITY_MODE_MIXED, 0 },
	{ "wpa", OVSDB_SECURITY_ENCRYPTION_WPA_EAP, OVSDB_SECURITY_MODE_WPA1, 1 },
	{ "wpa2", OVSDB_SECURITY_ENCRYPTION_WPA_EAP, OVSDB_SECURITY_MODE_WPA2, 1 },
	{ "wpa-mixed", OVSDB_SECURITY_ENCRYPTION_WPA_EAP, OVSDB_SECURITY_MODE_MIXED, 1 },
};

static void vif_config_security_set(struct blob_buf *b,
				    const struct schema_Wifi_VIF_Config *vconf)
{
	const char *encryption = SCHEMA_KEY_VAL(vconf->security, SCHEMA_CONSTS_SECURITY_ENCRYPT);
	const char *mode = SCHEMA_KEY_VAL(vconf->security, SCHEMA_CONSTS_SECURITY_MODE);
	unsigned int i;

	if (!strcmp(encryption, OVSDB_SECURITY_ENCRYPTION_OPEN) || !mode)
		goto open;
	for (i = 0; i < ARRAY_SIZE(vif_crypto); i++) {
		if (strcmp(vif_crypto[i].encryption, encryption))
			continue;
		if (strcmp(vif_crypto[i].mode, mode))
			continue;
		blobmsg_add_string(b, "encryption", vif_crypto[i].uci);
		blobmsg_add_bool(b, "ieee80211w", 1);
		if (vif_crypto[i].enterprise) {
			blobmsg_add_string(b, "server",
					   SCHEMA_KEY_VAL(vconf->security, SCHEMA_CONSTS_SECURITY_RADIUS_IP));
			blobmsg_add_string(b, "port",
					   SCHEMA_KEY_VAL(vconf->security, SCHEMA_CONSTS_SECURITY_RADIUS_PORT));
			blobmsg_add_string(b, "auth_secret",
					   SCHEMA_KEY_VAL(vconf->security, SCHEMA_CONSTS_SECURITY_RADIUS_SECRET));
		} else {
			blobmsg_add_string(b, "key",
					   SCHEMA_KEY_VAL(vconf->security, SCHEMA_CONSTS_SECURITY_KEY));
		}
	}
open:
	blobmsg_add_string(b, "encryption", "none");
	blobmsg_add_string(b, "key", "");
	blobmsg_add_bool(b, "ieee80211w", 0);
}

static void vif_state_security_append(struct schema_Wifi_VIF_State *vstate,
				      int *index, const char *key, const char *value)
{
	STRSCPY(vstate->security_keys[*index], key);
	STRSCPY(vstate->security[*index], value);

	*index = *index + 1;
	vstate->security_len = *index;
}

static void vif_state_security_get(struct schema_Wifi_VIF_State *vstate,
				   struct blob_attr **tb)
{
	struct vif_crypto *vc = NULL;
	char *encryption = NULL;
	unsigned int i;
	int index = 0;

	if (tb[WIF_ATTR_ENCRYPTION]) {
		encryption = blobmsg_get_string(tb[WIF_ATTR_ENCRYPTION]);
		for (i = 0; i < ARRAY_SIZE(vif_crypto); i++)
			if (!strcmp(vif_crypto[i].uci, encryption))
				vc = &vif_crypto[i];
	}

	if (!encryption || !vc)
		goto out_none;

	if (vc->enterprise) {
		if (!tb[WIF_ATTR_SERVER] || !tb[WIF_ATTR_PORT] || !tb[WIF_ATTR_AUTH_SECRET])
			goto out_none;
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_ENCRYPTION,
					  OVSDB_SECURITY_ENCRYPTION_WPA_EAP);
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_MODE,
					  vc->mode);
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_RADIUS_SERVER_IP,
					  blobmsg_get_string(tb[WIF_ATTR_SERVER]));
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_RADIUS_SERVER_PORT,
					  blobmsg_get_string(tb[WIF_ATTR_PORT]));
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_KEY,
					  blobmsg_get_string(tb[WIF_ATTR_AUTH_SECRET]));
	} else {
		if (!tb[WIF_ATTR_KEY])
			goto out_none;
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_ENCRYPTION,
					  OVSDB_SECURITY_ENCRYPTION_WPA_PSK);
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_MODE,
					  vc->mode);
		vif_state_security_append(vstate, &index, OVSDB_SECURITY_KEY,
					  blobmsg_get_string(tb[WIF_ATTR_KEY]));
	}
	return;

out_none:
	vif_state_security_append(vstate, &index, OVSDB_SECURITY_ENCRYPTION,
				  OVSDB_SECURITY_ENCRYPTION_OPEN);
}

/* Custom options table */
#define SCHEMA_CUSTOM_OPT_SZ            20
#define SCHEMA_CUSTOM_OPTS_MAX          5

const char custom_options_table[SCHEMA_CUSTOM_OPTS_MAX][SCHEMA_CUSTOM_OPT_SZ] =
{
	SCHEMA_CONSTS_RATE_LIMIT,
	SCHEMA_CONSTS_RATE_DL,
	SCHEMA_CONSTS_RATE_UL,
	SCHEMA_CONSTS_CLIENT_RATE_DL,
	SCHEMA_CONSTS_CLIENT_RATE_UL
};

static void vif_config_custom_opt_set(struct blob_buf *b,
                                      const struct schema_Wifi_VIF_Config *vconf)
{
	int i;
	char value[20];
	const char *opt;
	const char *val;

	for (i = 0; i < SCHEMA_CUSTOM_OPTS_MAX; i++) {
		opt = custom_options_table[i];
		val = SCHEMA_KEY_VAL(vconf->custom_options, opt);

		if (!val)
			strncpy(value, "0", 20);
		else
			strncpy(value, val, 20);

		if (strcmp(opt, "rate_limit_en") == 0) {
			if (strcmp(value, "1") == 0)
				blobmsg_add_bool(b, "rlimit", 1);
			else if (strcmp(value, "0") == 0)
				blobmsg_add_bool(b, "rlimit", 0);
		}
		else if (strcmp(opt, "ssid_ul_limit") == 0)
			blobmsg_add_string(b, "urate", value);
		else if (strcmp(opt, "ssid_dl_limit") == 0)
			blobmsg_add_string(b, "drate", value);
		else if (strcmp(opt, "client_dl_limit") == 0)
			blobmsg_add_string(b, "cdrate", value);
		else if (strcmp(opt, "client_ul_limit") == 0)
			blobmsg_add_string(b, "curate", value);
	}
}

static void set_custom_option_state(struct schema_Wifi_VIF_State *vstate,
                                    int *index, const char *key,
                                    const char *value)
{
	STRSCPY(vstate->custom_options_keys[*index], key);
	STRSCPY(vstate->custom_options[*index], value);
	*index += 1;
	vstate->custom_options_len = *index;
}

static void vif_state_custom_options_get(struct schema_Wifi_VIF_State *vstate,
                                         struct blob_attr **tb)
{
	int i;
	int index = 0;
	const char *opt;
	char *buf = NULL;

	for (i = 0; i < SCHEMA_CUSTOM_OPTS_MAX; i++) {
		opt = custom_options_table[i];

		if (strcmp(opt, "rate_limit_en") == 0) {
			if (tb[WIF_ATTR_RATELIMIT] &&
				blobmsg_get_bool(tb[WIF_ATTR_RATELIMIT]))
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							"1");
			else
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							"0");
		} else if (strcmp(opt, "ssid_ul_limit") == 0) {
			if (tb[WIF_ATTR_URATE]) {
				buf = blobmsg_get_string(tb[WIF_ATTR_URATE]);
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							buf);
			} else {
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							"0");
			}
		} else if (strcmp(opt, "ssid_dl_limit") == 0) {
			if (tb[WIF_ATTR_DRATE]) {
				buf = blobmsg_get_string(tb[WIF_ATTR_DRATE]);
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							buf);
			} else {
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							"0");
			}
		} else if (strcmp(opt, "client_dl_limit") == 0) {
			if (tb[WIF_ATTR_CDRATE]) {
				buf = blobmsg_get_string(tb[WIF_ATTR_CDRATE]);
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							buf);
			} else {
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							"0");
			}
		} else if (strcmp(opt, "client_ul_limit") == 0) {
			if (tb[WIF_ATTR_CURATE]) {
				buf = blobmsg_get_string(tb[WIF_ATTR_CURATE]);
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							buf);
			} else {
				set_custom_option_state(vstate, &index,
							custom_options_table[i],
							"0");
			}
		}
	}
}

bool vif_state_update(struct uci_section *s, struct schema_Wifi_VIF_Config *vconf)
{
	struct blob_attr *tb[__WIF_ATTR_MAX] = { };
	struct schema_Wifi_VIF_State vstate;
	char mac[ETH_ALEN * 3];
	char *ifname, radio[IF_NAMESIZE];

	memset(&vstate, 0, sizeof(vstate));
	schema_Wifi_VIF_State_mark_all_present(&vstate);

	blob_buf_init(&b, 0);
	uci_to_blob(&b, s, &wifi_iface_param);
	blobmsg_parse(wifi_iface_policy, __WIF_ATTR_MAX, tb, blob_data(b.head), blob_len(b.head));

	if (!tb[WIF_ATTR_DEVICE] || !tb[WIF_ATTR_IFNAME]) {
		LOGE("%s: invalid radio/ifname", s->e.name);
		return false;
	}
	ifname = target_unmap_ifname(blobmsg_get_string(tb[WIF_ATTR_IFNAME]));
	strncpy(radio, blobmsg_get_string(tb[WIF_ATTR_DEVICE]), IF_NAMESIZE);

	vstate._partial_update = true;
	vstate.associated_clients_present = false;
	vstate.vif_config_present = false;

	SCHEMA_SET_INT(vstate.rrm, 1);
	SCHEMA_SET_INT(vstate.btm, 1);
	SCHEMA_SET_INT(vstate.ft_psk, 0);
	SCHEMA_SET_INT(vstate.group_rekey, 0);

	strscpy(vstate.mac_list_type, "none", sizeof(vstate.mac_list_type));
	vstate.mac_list_len = 0;

	SCHEMA_SET_STR(vstate.if_name, ifname);

	if (tb[WIF_ATTR_HIDDEN] && blobmsg_get_bool(tb[WIF_ATTR_HIDDEN]))
		SCHEMA_SET_STR(vstate.ssid_broadcast, "disabled");
	else
		SCHEMA_SET_STR(vstate.ssid_broadcast, "enabled");

	if (tb[WIF_ATTR_MODE])
		SCHEMA_SET_STR(vstate.mode, blobmsg_get_string(tb[WIF_ATTR_MODE]));
	else
		SCHEMA_SET_STR(vstate.mode, "ap");

	if (tb[WIF_ATTR_DISABLED] && blobmsg_get_bool(tb[WIF_ATTR_DISABLED]))
		SCHEMA_SET_INT(vstate.enabled, 0);
	else
		SCHEMA_SET_INT(vstate.enabled, 1);

	if (tb[WIF_ATTR_ISOLATE] && blobmsg_get_bool(tb[WIF_ATTR_ISOLATE]))
		SCHEMA_SET_INT(vstate.ap_bridge, 1);
	else
		SCHEMA_SET_INT(vstate.ap_bridge, 0);

//	if (tb[WIF_ATTR_UAPSD] && blobmsg_get_bool(tb[WIF_ATTR_UAPSD]))
		SCHEMA_SET_INT(vstate.uapsd_enable, true);
//	else
//		SCHEMA_SET_INT(vstate.uapsd_enable, false);

	if (tb[WIF_ATTR_NETWORK])
		SCHEMA_SET_STR(vstate.bridge, blobmsg_get_string(tb[WIF_ATTR_NETWORK]));
	else
		LOGW("%s: unknown bridge/network", s->e.name);

	if (tb[WIF_ATTR_VLAN_ID])
		SCHEMA_SET_INT(vstate.vlan_id, blobmsg_get_u32(tb[WIF_ATTR_VLAN_ID]));
	else
		SCHEMA_SET_INT(vstate.vlan_id, 0);

	if (tb[WIF_ATTR_SSID])
		SCHEMA_SET_STR(vstate.ssid, blobmsg_get_string(tb[WIF_ATTR_SSID]));
	else
		LOGW("%s: failed to get SSID", s->e.name);

	if (strstr(s->e.name, "50"))
		SCHEMA_SET_STR(vstate.min_hw_mode, "11ac");
	else
		SCHEMA_SET_STR(vstate.min_hw_mode, "11n");

	if (tb[WIF_ATTR_BSSID])
		SCHEMA_SET_STR(vstate.mac, blobmsg_get_string(tb[WIF_ATTR_BSSID]));
	else if (tb[WIF_ATTR_IFNAME] && !vif_get_mac(blobmsg_get_string(tb[WIF_ATTR_IFNAME]), mac))
		SCHEMA_SET_STR(vstate.mac, mac);
	else
		LOGN("%s: Failed to get base BSSID (mac)", vstate.if_name);

	if (tb[WIF_ATTR_MACFILTER]) {
		if (!strcmp(blobmsg_get_string(tb[WIF_ATTR_MACFILTER]), "disable")) {
			vstate.mac_list_type_exists = true;
			SCHEMA_SET_STR(vstate.mac_list_type, "none");
		} else if(!strcmp(blobmsg_get_string(tb[WIF_ATTR_MACFILTER]), "allow")) {
			vstate.mac_list_type_exists = true;
			SCHEMA_SET_STR(vstate.mac_list_type, "whitelist");
		} else if(!strcmp(blobmsg_get_string(tb[WIF_ATTR_MACFILTER]), "deny")) {
			vstate.mac_list_type_exists = true;
			SCHEMA_SET_STR(vstate.mac_list_type, "blacklist");
		} else
			vstate.mac_list_type_exists = false;
	}

	if (tb[WIF_ATTR_MACLIST]) {
		struct blob_attr *cur = NULL;
		int rem = 0;

		vstate.mac_list_len = 0;
		blobmsg_for_each_attr(cur, tb[WIF_ATTR_MACLIST], rem) {
			if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
				continue;
			strcpy(vstate.mac_list[vstate.mac_list_len], blobmsg_get_string(cur));
			vstate.mac_list_len++;
		}
	}

	vif_state_security_get(&vstate, tb);
	vif_state_custom_options_get(&vstate, tb);

	if (vconf) {
		LOGN("%s: updating vif config", radio);
		vif_state_to_conf(&vstate, vconf);
		radio_ops->op_vconf(vconf, radio);
	}
	LOGN("%s: updating vif state", radio);
	radio_ops->op_vstate(&vstate, radio);

	return true;
}

bool target_vif_config_set2(const struct schema_Wifi_VIF_Config *vconf,
			    const struct schema_Wifi_Radio_Config *rconf,
			    const struct schema_Wifi_Credential_Config *cconfs,
			    const struct schema_Wifi_VIF_Config_flags *changed,
			    int num_cconfs)
{
	char *ifname = target_map_ifname((char *)vconf->if_name);
	int vid = 0;

	blob_buf_init(&b, 0);

	if (changed->enabled && vconf->enabled)
		blobmsg_add_bool(&b, "disabled", 0);
	else
		blobmsg_add_bool(&b, "disabled", 1);

	if (changed->ssid)
		blobmsg_add_string(&b, "ssid", vconf->ssid);

	if (changed->ssid_broadcast && !strcmp(vconf->ssid_broadcast, "disabled"))
		blobmsg_add_bool(&b, "hidden", 1);
	else
		blobmsg_add_bool(&b, "hidden", 0);

	if (changed->ap_bridge && vconf->ap_bridge)
		blobmsg_add_bool(&b, "isolate", 1);
	else
		blobmsg_add_bool(&b, "isolate", 0);

	if (changed->uapsd_enable && vconf->uapsd_enable)
		blobmsg_add_bool(&b, "uapsd", 1);
	else
		blobmsg_add_bool(&b, "uapsd", 0);

	if ((changed->ft_psk && vconf->ft_psk) ||
	    (changed->ft_mobility_domain && vconf->ft_mobility_domain)) {
		blobmsg_add_bool(&b, "ieee80211r", 1);
		blobmsg_add_hex16(&b, "mobility_domain", vconf->ft_mobility_domain);
		blobmsg_add_bool(&b, "ft_psk_generate_local", vconf->ft_psk);
		blobmsg_add_bool(&b, "ft_over_ds", 0);
		blobmsg_add_bool(&b, "reassociation_deadline", 1);
	} else {
		blobmsg_add_bool(&b, "ieee80211r", 0);
	}

	if (changed->bridge)
		blobmsg_add_string(&b, "network", vconf->bridge);

	if (changed->vlan_id) {
		blobmsg_add_u32(&b, "vlan_id", vconf->vlan_id);
		if (vconf->vlan_id > 2)
			vid = vconf->vlan_id;
		blobmsg_add_u32(&b, "vid", vid);
	}

	if (changed->mac_list_type) {
		struct blob_attr *a;
		int i;

		if (!strcmp(vconf->mac_list_type, "whitelist"))
			blobmsg_add_string(&b, "macfilter", "allow");
		else if (!strcmp(vconf->mac_list_type,"blacklist"))
			blobmsg_add_string(&b, "macfilter", "deny");
		else
			blobmsg_add_string(&b, "macfilter", "disable");

		a = blobmsg_open_array(&b, "maclist");
		for (i = 0; i < vconf->mac_list_len; i++)
			blobmsg_add_string(&b, NULL, (char*)vconf->mac_list[i]);
		blobmsg_close_array(&b, a);
	}

	vif_config_security_set(&b, vconf);
	if (changed->custom_options)
		vif_config_custom_opt_set(&b, vconf);

	blob_to_uci_section(uci, "wireless", ifname, "wifi-iface",
			    b.head, &wifi_iface_param);

	if (vid)
		vlan_add(ifname, vconf->vlan_id, vid);
	else
		vlan_del(ifname);

	uci_commit_all(uci);
	reload_config = 1;

	return true;
}
