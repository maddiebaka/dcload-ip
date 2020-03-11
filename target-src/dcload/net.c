#include <string.h>
#include "commands.h"
#include "packet.h"
#include "adapter.h"
#include "scif.h"
#include "net.h"
#include "dhcp.h"
#include "memfuncs.h"

static void process_broadcast(unsigned char *pkt);
static void process_icmp(ether_header_t *ether, ip_header_t *ip, icmp_header_t *icmp);
static void process_udp(ether_header_t *ether, ip_header_t *ip, udp_header_t *udp);
static void process_mine(unsigned char *pkt);

const unsigned char broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// Packet transmit buffer
__attribute__((aligned(8))) unsigned char raw_pkt_buf[RAW_TX_PKT_BUF_SIZE]; // Here's a global array. Global packet transmit buffer.
// Need to offset the packet by 2 for the command->data after headers to always be aligned to 8 bytes
// The performance gains are well worth the 2 wasted bytes.
unsigned char * pkt_buf = &(raw_pkt_buf[2]);

static void process_broadcast(unsigned char *pkt)
{
	ether_header_t *ether_header = (ether_header_t *)pkt;
	arp_header_t *arp_header = (arp_header_t *)(pkt + ETHER_H_LEN);
	__attribute__((aligned(2))) unsigned char tmp[10];
	__attribute__((aligned(2))) unsigned int ip = htonl(our_ip);

	if (ether_header->type[1] != 0x06) /* ARP */
		return;

	/* hardware address space = ethernet */
	if (arp_header->hw_addr_space != 0x0100)
		return;

	/* protocol address space = IP */
	if (arp_header->proto_addr_space != 0x0008)
		return;

	if (arp_header->opcode == 0x0100) /* arp request */
	{
		if (our_ip == 0) /* return if we don't know our ip */
			return;

		// NOTE: using the 16-bit memcmp here is faster than the 32-bit one.
		// This is because by the time arp_header->proto_target is manually aligned
		// to 4 bytes, memcmp_16bit_eq would already be done the comparison operation.
		if (!memcmp_16bit_eq(arp_header->proto_target, &ip, 4/2)) /* for us */
		{
			/* put src hw address into dest hw address */
			memcpy_16bit(ether_header->dest, ether_header->src, 6/2);
			/* put our hw address into src hw address */
			memcpy_16bit(ether_header->src, bb->mac, 6/2);
			arp_header->opcode = 0x0200; /* arp reply */
			/* swap sender and target addresses */
			memcpy_16bit(tmp, arp_header->hw_sender, 10/2);
			memcpy_16bit(arp_header->hw_sender, arp_header->hw_target, 10/2);
			memcpy_16bit(arp_header->hw_target, tmp, 10/2);
			/* put our hw address into sender hw address */
			memcpy_16bit(arp_header->hw_sender, bb->mac, 6/2);
			/* transmit */
			bb->tx(pkt, ETHER_H_LEN + ARP_H_LEN);
		}
	}
}

static void process_icmp(ether_header_t *ether, ip_header_t *ip, icmp_header_t *icmp)
{
	unsigned int i;
	__attribute__((aligned(2))) unsigned char tmp[6];

	if (icmp->type == 8) /* echo request */
	{
		// dcload-ip only supports echo (ping), so the verification checksum can be
		// computed here. This way only ping packets are checked while others are
		// discarded without wasting time on them. This step is necessary to ensure
		// malformed packets aren't sent across the network.
		/* check icmp checksum */
		i = icmp->checksum;
		icmp->checksum = 0;
		icmp->checksum = checksum((unsigned short *)icmp, (ntohs(ip->length)+1)/2 - 2*(ip->version_ihl & 0x0f), ntohs(ip->length)%2);
		if (i != icmp->checksum)
			return;

		// Now make and send the reply
		// Just use the receive buffer since ping is an echo
		/* set echo reply */
		icmp->type = 0;
		/* swap src and dest hw addresses */
		memcpy_16bit(tmp, ether->dest, 6/2);
		memcpy_16bit(ether->dest, ether->src, 6/2);
		memcpy_16bit(ether->src, tmp, 6/2);
		/* swap src and dest ip addresses */
		memcpy_16bit(&i, &ip->src, 4/2);
		memcpy_16bit(&ip->src, &ip->dest, 4/2);
		memcpy_16bit(&ip->dest, &i, 4/2);
		/* recompute ip header checksum */
		ip->checksum = 0;
		ip->checksum = checksum((unsigned short *)ip, 2*(ip->version_ihl & 0x0f), 0);
		/* recompute icmp checksum */
		icmp->checksum = 0;
		icmp->checksum = checksum((unsigned short *)icmp, ntohs(ip->length)/2 - 2*(ip->version_ihl & 0x0f), ntohs(ip->length)%2);
		/* transmit */
		bb->tx((unsigned char *)ether, ETHER_H_LEN + ntohs(ip->length));
	}
}

static void process_udp(ether_header_t *ether, ip_header_t *ip, udp_header_t *udp)
{
	ip_udp_pseudo_header_t *pseudo;
	unsigned short i;
	command_t *command;

	pseudo = (ip_udp_pseudo_header_t *)pkt_buf; // Just use the transmit buffer
	pseudo->src_ip = ip->src;
	pseudo->dest_ip = ip->dest;
	pseudo->zero = 0;
	pseudo->protocol = ip->protocol;
	pseudo->udp_length = udp->length;
	pseudo->src_port = udp->src;
	pseudo->dest_port = udp->dest;
	pseudo->length = udp->length;
	pseudo->checksum = 0;

	/* checksum == 0 means no checksum */
	if (udp->checksum != 0)
		i = checksum_udp((unsigned short *)pseudo, (unsigned short *)udp->data, (ntohs(udp->length) - 8)/2, ntohs(udp->length)%2); // integer divide; need to round up to next even number
	else
		i = 0;
	/* checksum == 0xffff means checksum was really 0 */
	if (udp->checksum == 0xffff)
		udp->checksum = 0;

	if (__builtin_expect(i != udp->checksum, 0))
	{
		/*    scif_puts("UDP CHECKSUM BAD\n"); */
		return;
	}

	// Handle receipt of DHCP packets that are directed to this system
	dhcp_pkt_t *udp_pkt_data = (dhcp_pkt_t*)&udp->data;
	if(__builtin_expect(udp_pkt_data->op == DHCP_OP_BOOTREPLY, 0)) // DHCP ACK or DHCP OFFER
	{
		if(!handle_dhcp_reply(ether->src, (dhcp_pkt_t*)&udp->data, ntohs(udp->length) - 8))
		{ // -1 is true in C
			// If we got a DHCP packet that belongs to some other machine, e.g. some machine requires a broadcasted address instead of a unicasted one,
			// don't escape the loop since we didn't get the packet we needed.
			escape_loop = 1;
		}
	}
	else
	{
		// Fun fact: simply reordering this function improved network performance by 15kB/s

		make_ether(ether->src, ether->dest, (ether_header_t *)pkt_buf);

		command = (command_t *)udp->data;

		// Only one of these will ever match at a time. What we can do is set this variable to 0 after compare succeeds.
		// There is no id of 0, as they're all 4-character, non-null-terminated strings.
		// Unfortunately packet headers are 42 bytes, which is NOT a multiple of 4. It is a multiple of 2, though, so we can do this without crashing:
//		__attribute__((aligned(4))) unsigned int pkt_match_id = ((unsigned int) *(unsigned short*)&command->id[2] << 16) | (unsigned int) *(unsigned short*)command->id;

		// We can do this now that the receive packet buffer has been aligned.
		// All command structs are now aligned on a 4-byte boundary.
		unsigned int pkt_match_id = *(unsigned int*)command->id;

		// This one is the most likely to be called the most often, so put it first and tell GCC it's likely to be called
		if (__builtin_expect((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_PARTBIN, 4/4)), 1))
		{
			cmd_partbin(command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_DONEBIN, 4/4)))
		{
			cmd_donebin(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_RETVAL, 4/4)))
		{
			cmd_retval(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_LOADBIN, 4/4)))
		{
			cmd_loadbin(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_MAPLE, 4/4)))
		{
	    cmd_maple(ip, udp, command);
			pkt_match_id = 0;
	  }

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_PMCR, 4/4)))
		{
			cmd_pmcr(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_SENDBINQ, 4/4)))
		{
			cmd_sendbinq(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_SENDBIN, 4/4)))
		{
			cmd_sendbin(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_EXECUTE, 4/4)))
		{
			cmd_execute(ether, ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_VERSION, 4/4)))
		{
			cmd_version(ip, udp, command);
			pkt_match_id = 0;
		}

		if ((pkt_match_id) && (!memcmp_32bit_eq(&pkt_match_id, CMD_REBOOT, 4/4)))
		{
			// This function does not return
			cmd_reboot();
		}
	}
}

static void process_mine(unsigned char *pkt)
{
	ether_header_t *ether_header = (ether_header_t *)pkt;
	ip_header_t *ip_header = (ip_header_t *)(pkt + ETHER_H_LEN);
	icmp_header_t *icmp_header;
	udp_header_t *udp_header;
	int i;

	if(__builtin_expect(ether_header->type[1] != 0x00, 0))
		return;

	/* ignore fragmented packets */

	if(__builtin_expect(ntohs(ip_header->flags_frag_offset) & 0x3fff, 0))
		return;

	/* check ip header checksum */
	i = ip_header->checksum;
	ip_header->checksum = 0;
	ip_header->checksum = checksum((unsigned short *)ip_header, 2*(ip_header->version_ihl & 0x0f), 0);
	if (i != ip_header->checksum)
		return;

	if(__builtin_expect(ip_header->protocol == IP_UDP_PROTOCOL, 1))
	{
		/* udp */
		udp_header = (udp_header_t *)(pkt + ETHER_H_LEN + 4*(ip_header->version_ihl & 0x0f));
		process_udp(ether_header, ip_header, udp_header);
	}
	else if(__builtin_expect(ip_header->protocol == IP_ICMP_PROTOCOL, 0))
	{
		/* icmp */
		icmp_header = (icmp_header_t *)(pkt + ETHER_H_LEN + 4*(ip_header->version_ihl & 0x0f));
		process_icmp(ether_header, ip_header, icmp_header);
	}
}

void process_pkt(unsigned char *pkt)
{
	ether_header_t *ether_header = (ether_header_t *)pkt;

	if (ether_header->type[0] != 0x08)
		return;

	// Destination ethernet header is the first thing in the packet, so it's always aligned to 2 bytes
	if (!memcmp_16bit_eq(ether_header->dest, bb->mac, 6/2)) {
		process_mine(pkt);
		return;
	}

	if (!memcmp_16bit_eq(ether_header->dest, broadcast, 6/2)) {
		process_broadcast(pkt);
		return;
	}
}
