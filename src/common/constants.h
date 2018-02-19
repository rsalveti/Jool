#ifndef _JOOL_COMMON_CONSTANTS_H
#define _JOOL_COMMON_CONSTANTS_H

/**
 * @file
 * General purpose #defines, intended to minimize use of numerical constants
 * elsewhere in the code.
 */

/** Maximum storable value on a __u8. */
#define MAX_U8 0xFFU
/** Maximum storable value on a __u16. */
#define MAX_U16 0xFFFFU
/** Maximum storable value on a __u32. */
/* TODO shouldn't this belong to usr-string-utils? */
#define MAX_U32 0xFFFFFFFFU

/* -- Timeouts, defined by RFC 6146, section 4. */

/**
 * Minimum allowable session lifetime for UDP bindings, in seconds.
 */
#define UDP_MIN (2 * 60)
/**
 * Defined in the RFC as the minimum allowable default value for the session
 * lifetime of UDP bindings, in seconds. We use it as the actual default value.
 */
#define UDP_DEFAULT (5 * 60)
/**
 * Established connection idle timeout (in seconds).
 * In other words, the tolerance time for established and healthy TCP sessions.
 * If a connection remains idle for longer than this, then we expect it to
 * terminate soon.
 */
#define TCP_EST (2 * 60 * 60)
/**
 * Transitory connection idle timeout (in seconds).
 * In other words, the timeout of TCP sessions which are expected to terminate
 * soon.
 */
#define TCP_TRANS (4 * 60)
/**
 * Timeout of TCP sessions started from v4 which we're skeptical as to whether
 * they are going to make it to the established state.
 * Also the time a user has to manage a hole punch through Jool.
 * Measured in in seconds.
 * This value cannot be configured from the userspace app (this is on purpose).
 */
#define TCP_INCOMING_SYN (6)
/** Default session lifetime for ICMP bindings, in seconds. */
#define ICMP_DEFAULT (1 * 60)

/** Default time interval fragments are allowed to arrive in. In seconds. */
#define FRAGMENT_MIN (2)

/*
 * The timers will never sleep less than this amount of jiffies. This is because
 * I don't think we need to interrupt the kernel too much.
 *
 * 255 stands for TVR_SIZE - 1 (The kernel doesn't export TVR_SIZE).
 * Why that value? It's the maximum we can afford without cascading the timer
 * wheel when CONFIG_BASE_SMALL is false (https://lkml.org/lkml/2005/10/19/46).
 *
 * jiffies can be configured (http://man7.org/linux/man-pages/man7/time.7.html)
 * to be
 * - 0.01 seconds, which will make this minimum ~2.5 seconds.
 * - 0.004 seconds, which will make this minimum ~1 second.
 * - 0.001 seconds, which will make this minimum ~0.25 seconds.
 *
 * If you think this is dumb, you can always assign some other value, such as
 * zero.
 */
#define MIN_TIMER_SLEEP (255)

/** -- TCP state machine states; RFC 6146 section 3.5.2. -- */
typedef enum tcp_state {
	/**
	 * The handshake is complete and the sides are exchanging upper-layer
	 * data.
	 *
	 * This is the zero one so UDP and ICMP can unset the state field if
	 * they want without fear of this looking weird.
	 * (UDP/ICMP sessions are always logically established.)
	 */
	ESTABLISHED = 0,
	/**
	 * A SYN packet arrived from the IPv6 side; some IPv4 node is trying to
	 * start a connection.
	 */
	V6_INIT,
	/**
	 * A SYN packet arrived from the IPv4 side; some IPv4 node is trying to
	 * start a connection.
	 */
	V4_INIT,
	/**
	 * The IPv4 node wants to terminate the connection. Data can still flow.
	 * Awaiting a IPv6 FIN...
	 */
	V4_FIN_RCV,
	/**
	 * The IPv6 node wants to terminate the connection. Data can still flow.
	 * Awaiting a IPv4 FIN...
	 */
	V6_FIN_RCV,
	/** Both sides issued a FIN. Packets can still flow for a short time. */
	V4_FIN_V6_FIN_RCV,
	/** The session might die in a short while. */
	TRANS,
} tcp_state;

/* -- Config defaults -- */
#define DEFAULT_ADDR_DEPENDENT_FILTERING false
#define DEFAULT_FILTER_ICMPV6_INFO false
#define DEFAULT_DROP_EXTERNAL_CONNECTIONS false
#define DEFAULT_MAX_STORED_PKTS 10
#define DEFAULT_SRC_ICMP6ERRS_BETTER false
#define DEFAULT_F_ARGS 0b1011
#define DEFAULT_HANDLE_FIN_RCV_RST false
#define DEFAULT_BIB_LOGGING false
#define DEFAULT_SESSION_LOGGING false

#define DEFAULT_RESET_TRAFFIC_CLASS false
#define DEFAULT_RESET_TOS false
#define DEFAULT_NEW_TOS 0
#define DEFAULT_COMPUTE_UDP_CSUM0 false
#define DEFAULT_EAM_HAIRPIN_MODE EHM_INTRINSIC
#define DEFAULT_RANDOMIZE_RFC6791 true
#define DEFAULT_RFC6791V6_PREFIX NULL
/* Note: total size must be <= PLATEAUS_MAX. */
#define DEFAULT_MTU_PLATEAUS { 65535, 32000, 17914, 8166, 4352, 2002, 1492, \
		1006, 508, 296, 68 }
#define DEFAULT_JOOLD_ENABLED false
#define DEFAULT_JOOLD_FLUSH_ASAP true
#define DEFAULT_JOOLD_DEADLINE msecs_to_jiffies(2000)
#define DEFAULT_JOOLD_CAPACITY 512
/**
 * typical MTU minus max(20, 40) minus the UDP header. (1500 - 40 - 8)
 * There's a 16-bytes joold header and each session spans 64 bytes currently.
 * This means we can fit 22 sessions per packet. (Regardless of IPv4/IPv6)
 */
#define DEFAULT_JOOLD_MAX_PAYLOAD 1452

/* -- IPv6 Pool -- */

/**
 * RFC 6052's allowed prefix lengths.
 */
#define POOL6_PREFIX_LENGTHS { 32, 40, 48, 56, 64, 96 }


/* -- IPv4 pool -- */
#define DEFAULT_POOL4_MIN_PORT 61001
#define DEFAULT_POOL4_MAX_PORT 65535


/* -- ICMP constants missing from icmp.h and icmpv6.h. -- */

/** Code 0 for ICMP messages of type ICMP_PARAMETERPROB. */
#define ICMP_PTR_INDICATES_ERROR 0
/** Code 2 for ICMP messages of type ICMP_PARAMETERPROB. */
#define ICMP_BAD_LENGTH 2


#endif /* _JOOL_COMMON_CONSTANTS_H */
