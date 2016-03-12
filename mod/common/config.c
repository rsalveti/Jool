#include "nat64/mod/common/config.h"

#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include "nat64/common/config.h"
#include "nat64/common/constants.h"
#include "nat64/common/types.h"
#include "nat64/mod/common/tags.h"
#include "nat64/mod/common/wkmalloc.h"

static DEFINE_MUTEX(lock);

RCUTAG_USR
int config_init(struct global_configuration **result)
{
	struct global_config *config;
	__u16 plateaus[] = DEFAULT_MTU_PLATEAUS;

	*result = wkmalloc(struct global_configuration, GFP_KERNEL);
	if (!(*result))
		return -ENOMEM;
	kref_init(&(*result)->refcounter);
	config = &(*result)->cfg;

	config->status = 0; /* This is never read, but whatever. */
	config->enabled = DEFAULT_INSTANCE_ENABLED;
	config->reset_traffic_class = DEFAULT_RESET_TRAFFIC_CLASS;
	config->reset_tos = DEFAULT_RESET_TOS;
	config->new_tos = DEFAULT_NEW_TOS;

	config->atomic_frags.df_always_on = DEFAULT_DF_ALWAYS_ON;
	config->atomic_frags.build_ipv6_fh = DEFAULT_BUILD_IPV6_FH;
	config->atomic_frags.build_ipv4_id = DEFAULT_BUILD_IPV4_ID;
	config->atomic_frags.lower_mtu_fail = DEFAULT_LOWER_MTU_FAIL;

	if (xlat_is_siit()) {
		config->siit.compute_udp_csum_zero = DEFAULT_COMPUTE_UDP_CSUM0;
		config->siit.eam_hairpin_mode = DEFAULT_EAM_HAIRPIN_MODE;
		config->siit.randomize_error_addresses = DEFAULT_RANDOMIZE_RFC6791;
	} else {
		config->nat64.src_icmp6errs_better = DEFAULT_SRC_ICMP6ERRS_BETTER;
		config->nat64.drop_by_addr = DEFAULT_ADDR_DEPENDENT_FILTERING;
		config->nat64.drop_external_tcp = DEFAULT_DROP_EXTERNAL_CONNECTIONS;
		config->nat64.drop_icmp6_info = DEFAULT_FILTER_ICMPV6_INFO;
		config->nat64.f_args = DEFAULT_F_ARGS;
	}

	config->mtu_plateau_count = ARRAY_SIZE(plateaus);
	memcpy(config->mtu_plateaus, &plateaus, sizeof(plateaus));

	return 0;
}

void config_get(struct global_configuration *config)
{
	kref_get(&config->refcounter);
}

static void destroy_config(struct kref *refcounter)
{
	struct global_configuration *config;
	config = container_of(refcounter, typeof(*config), refcounter);
	wkfree(struct global_configuration, config);
}

void config_put(struct global_configuration *config)
{
	kref_put(&config->refcounter, destroy_config);
}

RCUTAG_PKT
void config_copy(struct global_config *from, struct global_config *to)
{
	memcpy(to, from, sizeof(*from));
}

RCUTAG_FREE
void prepare_config_for_userspace(struct full_config *config, bool pools_empty)
{
	struct global_config *global;
	struct session_config *session;
	struct fragdb_config *frag;

	global = &config->global;
	global->status = global->enabled && !pools_empty;

	session = &config->session;
	session->ttl.tcp_est = jiffies_to_msecs(session->ttl.tcp_est);
	session->ttl.tcp_trans = jiffies_to_msecs(session->ttl.tcp_trans);
	session->ttl.udp = jiffies_to_msecs(session->ttl.udp);
	session->ttl.icmp = jiffies_to_msecs(session->ttl.icmp);

	frag = &config->frag;
	frag->ttl = jiffies_to_msecs(frag->ttl);
}
