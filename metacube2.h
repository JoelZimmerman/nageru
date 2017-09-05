#ifndef _METACUBE2_H
#define _METACUBE2_H

/*
 * Definitions for the Metacube2 protocol, used to communicate with Cubemap.
 *
 * Note: This file is meant to compile as both C and C++, for easier inclusion
 * in other projects.
 */

#include <stdint.h>

#define METACUBE2_SYNC "cube!map"  /* 8 bytes long. */
#define METACUBE_FLAGS_HEADER 0x1
#define METACUBE_FLAGS_NOT_SUITABLE_FOR_STREAM_START 0x2

/*
 * Metadata packets; should not be counted as data, but rather
 * parsed (or ignored if you don't understand them).
 *
 * Metadata packets start with a uint64_t (network byte order)
 * that describe the type; the rest is defined by the type.
 */
#define METACUBE_FLAGS_METADATA 0x4

struct metacube2_block_header {
	char sync[8];    /* METACUBE2_SYNC */
	uint32_t size;   /* Network byte order. Does not include header. */
	uint16_t flags;  /* Network byte order. METACUBE_FLAGS_*. */
	uint16_t csum;   /* Network byte order. CRC16 of size and flags.
                            If METACUBE_FLAGS_METADATA is set, inverted
                            so that older clients will ignore it as broken. */
};

uint16_t metacube2_compute_crc(const struct metacube2_block_header *hdr);

/*
 * The only currently defined metadata type. Set by the encoder,
 * and can be measured for latency purposes (e.g., if the network
 * can't keep up, the latency will tend to increase.
 */
#define METACUBE_METADATA_TYPE_ENCODER_TIMESTAMP 0x1

struct metacube2_timestamp_packet {
	uint64_t type;  /* METACUBE_METADATA_TYPE_ENCODER_TIMESTAMP, in network byte order. */

	/*
	 * Time since the UTC epoch. Basically a struct timespec.
	 * Both are in network byte order.
	 */
	uint64_t tv_sec;
	uint64_t tv_nsec;
};

#endif  /* !defined(_METACUBE_H) */
