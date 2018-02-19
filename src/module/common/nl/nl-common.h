#ifndef __NL_COMMON_H__
#define __NL_COMMON_H__

#include <net/genetlink.h>
#include "config.h"

int verify_privileges(void);
struct request_hdr *get_jool_hdr(struct genl_info *info);
void *get_jool_payload(struct genl_info *info);
int validate_request_size(struct genl_info *info, size_t min_expected);

#endif
