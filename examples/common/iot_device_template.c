/*
 * Minimal device-integration template. Product hardware and policy stay in
 * the application; platform services communicate through callbacks.
 */

#include "hq_app_state.h"
#include "hq_net.h"

#include <stdio.h>

static void set_product_safe_state(hq_app_state_t state,
				   hq_app_state_event_t event,
				   hq_app_transition_owner_t owner,
				   uint32_t session)
{
	(void)state;
	(void)event;
	(void)owner;
	(void)session;
	/* Replace with the product's safe action: relay-off, heater-off, etc. */
	puts("product safe-state callback");
}

static void on_network_connected(void *user_data)
{
	(void)user_data;
	puts("network connected");
}

static void on_network_disconnected(void *user_data)
{
	(void)user_data;
	puts("network disconnected; application owns the safe-state decision");
}

int main(void)
{
	const hq_app_state_config_t state_config = {
		.on_safe_state = set_product_safe_state,
	};
	const hq_net_config_t net_config = {
		.backend = HQ_NET_BACKEND_WIFI,
		.startup_mode = HQ_NET_STARTUP_MODE_PROVISIONING,
	};
	const hq_net_callbacks_t net_callbacks = {
		.on_connected = on_network_connected,
		.on_disconnected = on_network_disconnected,
	};

	if (hq_app_state_init(&state_config) != HQ_APP_STATE_OK ||
	    hq_net_start(&net_config, &net_callbacks) != HQ_NET_OK) {
		set_product_safe_state(HQ_APP_STATE_SAFE_OFF,
				      HQ_APP_EVENT_NETWORK_FAILED,
				      HQ_APP_OWNER_NETWORK, 0u);
		return 1;
	}

	puts("device template initialized; add product state, telemetry and RPC handlers");
	hq_net_stop();
	hq_app_state_deinit();
	return 0;
}