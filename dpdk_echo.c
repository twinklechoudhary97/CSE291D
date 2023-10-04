#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define MAX_CORES 64
#define UDP_MAX_PAYLOAD 1472
#define MAX_SAMPLES (10*1000*1000)

#define FULL_MASK 0xFFFFFFFF
#define EMPTY_MASK 0x0

/* offload checksum calculations */
static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
	},
	.txmode = {
		.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
	},
};

enum {
	MODE_UDP_CLIENT = 0,
	MODE_UDP_SERVER,
};

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

static unsigned int dpdk_port = 1;
static uint8_t mode;
struct rte_mempool *rx_mbuf_pool;
struct rte_mempool *tx_mbuf_pool;
static struct rte_ether_addr my_eth;
static uint32_t my_ip;
static uint32_t server_ip;
struct rte_ether_addr zero_mac = {
		.addr_bytes = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
};
struct rte_ether_addr broadcast_mac = {
		.addr_bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};
struct rte_ether_addr static_server_eth;
static uint64_t snd_times[MAX_SAMPLES];
static uint64_t rcv_times[MAX_SAMPLES];

/* parameters */
static int seconds = 5;
static size_t payload_len = 22; /* total packet size of 64 bytes */
static unsigned int client_port = 50000;
static unsigned int server_port = 8001;
static unsigned int num_queues = 1;

/* dpdk_echo.c: simple application to echo packets using DPDK */

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_queues)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = n_queues, tx_rings = n_queues;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;

	printf("initializing with %u queues\n", n_queues);

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL,
                                        mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Enable TX offloading */
	rte_eth_dev_info_get(0, &dev_info);
	txconf = &dev_info.default_txconf;

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	rte_eth_macaddr_get(port, &my_eth);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			my_eth.addr_bytes[0], my_eth.addr_bytes[1],
			my_eth.addr_bytes[2], my_eth.addr_bytes[3],
			my_eth.addr_bytes[4], my_eth.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/*
 * Validate this ethernet header. Return true if this packet is for higher
 * layers, false otherwise.
 */
static bool check_eth_hdr(struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		/* packet not IPv4 */
		return false;

	return true;
}

/*
 * Return true if this IP packet is to us and contains a UDP packet,
 * false otherwise.
 */
static bool check_ip_hdr(struct rte_mbuf *buf)
{
	struct rte_ipv4_hdr *ipv4_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,
			RTE_ETHER_HDR_LEN);
	if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(my_ip)
			|| ipv4_hdr->next_proto_id != IPPROTO_UDP)
		return false;

	return true;
}

/*
 * Run an echo client
 */
static void run_client(uint8_t port)
{
	uint64_t start_time, end_time;
	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	struct rte_ether_hdr *ptr_mac_hdr;
	char *buf_ptr;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *rte_udp_hdr;
	uint32_t nb_tx, nb_rx, i;
	uint64_t reqs = 0;
	struct rte_ether_addr server_eth;
	char mac_buf[64];
	uint64_t time_received;

	/* Verify that we have enough space for all the datapoints, assuming
	   an RTT of at least 4 us */
	uint32_t samples = seconds / ((float) 4.0 / (1000*1000));
	if (samples > MAX_SAMPLES)
		rte_exit(EXIT_FAILURE, "Too many samples: %d\n", samples);

	printf("\nCore %u running in client mode. [Ctrl+C to quit]\n",
			rte_lcore_id());

	rte_ether_format_addr(&mac_buf[0], 64, &static_server_eth);
	printf("Using static server MAC addr: %s\n", &mac_buf[0]);

	/* run for specified amount of time */
	start_time = rte_get_timer_cycles();
	while (rte_get_timer_cycles() <
			start_time + seconds * rte_get_timer_hz()) {
		buf = rte_pktmbuf_alloc(tx_mbuf_pool);
		if (buf == NULL)
			printf("error allocating tx mbuf\n");

		/* ethernet header */
		buf_ptr = rte_pktmbuf_append(buf, RTE_ETHER_HDR_LEN);
		eth_hdr = (struct rte_ether_hdr *) buf_ptr;

		rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
		rte_ether_addr_copy(&static_server_eth, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		/* IPv4 header */
		buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_ipv4_hdr));
		ipv4_hdr = (struct rte_ipv4_hdr *) buf_ptr;
		ipv4_hdr->version_ihl = 0x45;
		ipv4_hdr->type_of_service = 0;
		ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_udp_hdr) + payload_len);
		ipv4_hdr->packet_id = 0;
		ipv4_hdr->fragment_offset = 0;
		ipv4_hdr->time_to_live = 64;
		ipv4_hdr->next_proto_id = IPPROTO_UDP;
		ipv4_hdr->hdr_checksum = 0;
		ipv4_hdr->src_addr = rte_cpu_to_be_32(my_ip);
		ipv4_hdr->dst_addr = rte_cpu_to_be_32(server_ip);

		/* UDP header + fake data */
		buf_ptr = rte_pktmbuf_append(buf,
				sizeof(struct rte_udp_hdr) + payload_len);
		rte_udp_hdr = (struct rte_udp_hdr *) buf_ptr;
		rte_udp_hdr->src_port = rte_cpu_to_be_16(client_port);
		rte_udp_hdr->dst_port = rte_cpu_to_be_16(server_port);
		rte_udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr)
				+ payload_len);
		rte_udp_hdr->dgram_cksum = 0;
		memset(buf_ptr + sizeof(struct rte_udp_hdr), 0xAB, payload_len);

		buf->l2_len = RTE_ETHER_HDR_LEN;
		buf->l3_len = sizeof(struct rte_ipv4_hdr);
		buf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
		
		/* send packet */
		snd_times[reqs] = rte_get_timer_cycles();
		nb_tx = rte_eth_tx_burst(port, 0, &buf, 1);

		if (unlikely(nb_tx != 1)) {
			printf("error: could not send packet\n");
		}

		nb_rx = 0;
		while (rte_get_timer_cycles() <
		       start_time + seconds * rte_get_timer_hz()) {
			nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
			time_received = rte_get_timer_cycles();
			if (nb_rx == 0)
				continue;

			for (i = 0; i < nb_rx; i++) {
				buf = bufs[i];

				if (!check_eth_hdr(buf))
					goto no_match;

				/* this packet is IPv4, check IP header */
				if (!check_ip_hdr(buf))
					goto no_match;

				/* check UDP header */
				rte_udp_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_udp_hdr *,
						RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
				if (rte_udp_hdr->src_port != rte_cpu_to_be_16(server_port) ||
				    rte_udp_hdr->dst_port != rte_cpu_to_be_16(client_port))
					goto no_match;

				/* packet matches */
				rte_pktmbuf_free(buf);
				goto found_match;

			no_match:
				/* packet isn't what we're looking for, free it and rx again */
				rte_pktmbuf_free(buf);
			}
		}
		/* never received a reply and experiment ended */
		break;
		
	found_match:
		rcv_times[reqs++] = time_received;
	}
	end_time = rte_get_timer_cycles();

	/* add up total cycles across all RTTs, skip first and last 10% */
	uint64_t total_cycles = 0;
	uint64_t included_samples = 0;
	for (i = reqs * 0.1; i < reqs * 0.9; i++) {
		total_cycles += rcv_times[i] - snd_times[i];
		included_samples++;
	}

	printf("ran for %f seconds, completed %"PRIu64" echos\n",
			(float) (end_time - start_time) / rte_get_timer_hz(), reqs);
	printf("client reqs/s: %f\n",
			(float) (reqs * rte_get_timer_hz()) / (end_time - start_time));
	if (included_samples > 0)
	  printf("mean latency (us): %f\n", (float) total_cycles *
		 1000 * 1000 / (included_samples * rte_get_timer_hz()));
}

/*
 * Run an echo server
 */
static int run_server()
{
	uint8_t port = dpdk_port;
	struct rte_mbuf *rx_bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	struct rte_mbuf *return_buf;
	char *buf_ptr;
	uint16_t nb_rx, nb_tx;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_ipv4_hdr *rx_ipv4_hdr;
	struct rte_udp_hdr *rte_udp_hdr;


	printf("\nCore %u running in server mode. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		/* receive packets */
		nb_rx = rte_eth_rx_burst(port, 0, rx_bufs, BURST_SIZE);

		if (nb_rx == 0)
			continue;

		printf("received a packet!\n");
		
		

		/* TODO: YOUR CODE HERE */
		for(int i=0; i<nb_rx; i++){
		 printf("Buffer value received\n"); 
		 buf = rte_pktmbuf_alloc(tx_mbuf_pool);

		 if (buf == NULL)
			printf("error allocating buffer\n");

		 buf = rx_bufs[i];
		 //rte_pktmbuf_dump(stdout, buf, 64);

		 printf("Ethernet parsing\n");
		 return_buf = rte_pktmbuf_alloc(tx_mbuf_pool);

		 if (return_buf == NULL)
			printf("error allocating return buffer\n");

		/* ethernet header */
		buf_ptr = rte_pktmbuf_append(return_buf, RTE_ETHER_HDR_LEN);
		eth_hdr = (struct rte_ether_hdr *) buf_ptr;

		struct rte_ether_hdr *ptr_mac_hdr;

		ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);

		rte_ether_addr_copy(&ptr_mac_hdr->dst_addr, &eth_hdr->src_addr);
		rte_ether_addr_copy(&ptr_mac_hdr->src_addr, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		printf("Ethernet headers prepared\n");

		/* IPv4 header */
		buf_ptr = rte_pktmbuf_append(return_buf, sizeof(struct rte_ipv4_hdr));
		ipv4_hdr = (struct rte_ipv4_hdr *) buf_ptr;
		ipv4_hdr->version_ihl = 0x45;
		ipv4_hdr->type_of_service = 0;
		ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_udp_hdr) + payload_len);
		ipv4_hdr->packet_id = 0;
		ipv4_hdr->fragment_offset = 0;
		ipv4_hdr->time_to_live = 64;
		ipv4_hdr->next_proto_id = IPPROTO_UDP;
		ipv4_hdr->hdr_checksum = 0;

		rx_ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,RTE_ETHER_HDR_LEN);

		ipv4_hdr->src_addr = rx_ipv4_hdr->dst_addr;
		ipv4_hdr->dst_addr = rx_ipv4_hdr->src_addr;

		struct in_addr ip_addr;
    	ip_addr.s_addr = ipv4_hdr->dst_addr;
        const char* dst_ip=inet_ntoa(ip_addr);
		printf("Result of Dest IP\n");
		printf("%s\n", dst_ip);

		ip_addr.s_addr = ipv4_hdr->src_addr;
        const char* src_ip=inet_ntoa(ip_addr);
		printf("Result of Src IP\n");
		printf("%s\n", src_ip);

		printf("IPV4 headers prepared\n");

		/* UDP header + fake data */
		buf_ptr = rte_pktmbuf_append(return_buf,
				sizeof(struct rte_udp_hdr) + payload_len);
		rte_udp_hdr = (struct rte_udp_hdr *) buf_ptr;
		rte_udp_hdr->src_port = rte_cpu_to_be_16(server_port);
		rte_udp_hdr->dst_port = rte_cpu_to_be_16(client_port);
		rte_udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr)
				+ payload_len);
		rte_udp_hdr->dgram_cksum = 0;
		memset(buf_ptr + sizeof(struct rte_udp_hdr), 0xAB, payload_len);
		printf("UDP Headers complete\n");

		// rte_udp_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_udp_hdr *,
		// 				RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
		// rte_udp_hdr->src_port = rte_cpu_to_be_16(server_port);
		// rte_udp_hdr->dst_port = rte_cpu_to_be_16(client_port);


		return_buf->l2_len = RTE_ETHER_HDR_LEN;
		return_buf->l3_len = sizeof(struct rte_ipv4_hdr);
		return_buf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;

		printf("Flags set complete\n");

		printf("Received packet\n");
		rte_pktmbuf_dump(stdout, buf, 64);

		printf("Sent packet\n");
		rte_pktmbuf_dump(stdout, return_buf, 64);
		
		/* send packet */
		nb_tx = rte_eth_tx_burst(port, 0, &return_buf, 1);

		if (unlikely(nb_tx != 1)) {
			printf("error: could not send packet\n");
		}
		printf("Packet sent\n");

		rte_pktmbuf_free(buf);
		rte_pktmbuf_free(return_buf);

		}
		
		
	}

	return 0;
}

/*
 * Initialize dpdk.
 */
static int dpdk_init(int argc, char *argv[])
{
	int args_parsed;

	/* Initialize the Environment Abstraction Layer (EAL). */
	args_parsed = rte_eal_init(argc, argv);
	if (args_parsed < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	/* Check that there is a port to send/receive on. */
	if (!rte_eth_dev_is_valid_port(dpdk_port))
		rte_exit(EXIT_FAILURE, "Error: port is not available\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	rx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_RX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (rx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx mbuf pool\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	tx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_TX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (tx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create tx mbuf pool\n");

	return args_parsed;
}

static int parse_echo_args(int argc, char *argv[])
{
	long tmp;
	int next_arg;

	/* argv[0] is still the program name */
	if (argc < 3) {
		printf("not enough arguments left: %d\n", argc);
		return -EINVAL;
	}

	str_to_ip(argv[2], &my_ip);

	if (!strcmp(argv[1], "UDP_CLIENT")) {
		mode = MODE_UDP_CLIENT;
		argc -= 3;
		if (argc < 2) {
			printf("not enough arguments left: %d\n", argc);
			return -EINVAL;
		}

		next_arg = 3;
		str_to_ip(argv[next_arg++], &server_ip);
		/* parse static server MAC addr from XX:XX:XX:XX:XX:XX */
		rte_ether_unformat_addr(argv[next_arg++],
					&static_server_eth);
	} else if (!strcmp(argv[1], "UDP_SERVER")) {
		mode = MODE_UDP_SERVER;
		argc -= 3;
		if (argc > 0) {
			printf("warning: extra arguments\n");
			return -EINVAL;
		}
	} else {
		printf("invalid mode '%s'\n", argv[1]);
		return -EINVAL;
	}

	return 0;
}

/*
 * The main function, which does initialization and starts the client or server.
 */
int
main(int argc, char *argv[])
{
	int args_parsed, res;

	/* Initialize dpdk. */
	args_parsed = dpdk_init(argc, argv);

	/* initialize our arguments */
	argc -= args_parsed;
	argv += args_parsed;
	res = parse_echo_args(argc, argv);
	if (res < 0)
		return 0;

	/* initialize port */
	if (mode == MODE_UDP_CLIENT && rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");
	if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", dpdk_port);

	if (mode == MODE_UDP_CLIENT)
		run_client(dpdk_port);
	else
		run_server();

	return 0;
}
