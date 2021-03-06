/*
 * pkt_handler.c
 *
 *  Created on: May 16, 2010
 *      Author: slezicz, slezicz@gmail.com
 */

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#define __FAVOR_BSD
#include <netinet/tcp.h>
#include <err.h>

#ifndef PKT_HANDLER_H
#include "pkt_handler.h"
#endif
#ifndef PKT_H_
#include "pkt.h"
#endif
#ifndef LBIO_H_
#include "lbio.h"
#endif
#ifndef CTL_H_
#include "ctl.h"
#endif
#include "labrea6.h"
#include "utils.h"


static void
echo_handler(const struct pcap_pkthdr* header, const u_char* packet,
    const u_char *icmp_header)
{
  /* Construct a reply */
  u_char *reply_pkt = copy_eth_ip_hdr(packet, header->caplen);

  int len = (packet + header->caplen - icmp_header);

  append_next_header(icmp_header, reply_pkt, (ETH_HLEN + IPV6_HLEN), len,
      IPPROTO_ICMPV6);
  lbio_send_echo_reply(reply_pkt, (ETH_HLEN + IPV6_HLEN + len));
}

static void
nd_handler(const struct pcap_pkthdr *header, const u_char *packet,
    const u_char *icmp_header)
{
  u_char *reply_pkt = copy_eth_ip_hdr(packet, header->caplen);

  int len = (packet + header->caplen - icmp_header);

  append_next_header(icmp_header, reply_pkt, (ETH_HLEN + IPV6_HLEN), len,
      IPPROTO_ICMPV6);
  lbio_send_neighbor_adv(reply_pkt, (ETH_HLEN + IPV6_HLEN + len));
}

static void
tcp_handler(const struct pcap_pkthdr *header, const u_char *packet,
    const u_char *tcp_header)
{
  struct ip6_hdr* ip6;
  struct tcphdr *tcp; /* The TCP header */

  u_char *reply_pkt = copy_eth_ip_hdr(packet, header->caplen);
  int len = (packet + header->caplen - tcp_header);

#ifdef DEBUG
  warnx("caplen=%d, paket p=%x, tcp_header p=%01x, len=%d", header->caplen,
      packet, tcp_header, len);
#endif

  ip6 = (struct ip6_hdr*) (reply_pkt + ETH_HLEN);
  tcp = (struct tcphdr*) append_next_header(tcp_header, reply_pkt, (ETH_HLEN
      + IPV6_HLEN), len, IPPROTO_TCP);
  if (ctl.tcpport_filter && (filter_check_port(ntohs(tcp->th_dport)) == FALSE))
    {
      char dst[INET6_ADDRSTRLEN], src[INET6_ADDRSTRLEN];
      util_print(VERY_VERBOSE, "%s: %s %d -> %s %d","Filter communication atempt",
          inet_ntop(AF_INET6, &ip6->ip6_dst, dst, INET6_ADDRSTRLEN),
          htons(tcp->th_sport),
          inet_ntop(AF_INET6, &ip6->ip6_src, src, INET6_ADDRSTRLEN),
          htons(tcp->th_dport));
      return;
    }
#ifdef DEBUG
  warnx("new tcp: sport=%d, dport=%d, flags=%d", htons(tcp->th_sport), htons(
          tcp->th_dport), tcp->th_flags);
#endif
  switch ((tcp->th_flags) & (TH_SYN | TH_ACK))
    {
  case TH_SYN:
    /* send SYN/ACK */
    tcp->th_win = ctl.throttlesize;

    /*
     * I use this hack, because I don't want to maintain any structure (like hash table) with IPs and states of its connections.
     * I simply encode the seq no with last 32bits of IP sport and dport to be able to recognize ACK packet in 3-way handshake.
     * And also Linux win probes.
     *
     * We store his sequence no. in our sequence no.
     * by encoding his seq and use this encoded one as ours seq.
     *
     */
    uint32_t my_seq = tcp->th_seq;
    my_seq ^= ntohl(ip6->ip6_src.s6_addr32[3]);
    my_seq ^= ((tcp->th_sport << 16) + tcp->th_dport);

    tcp->th_ack = tcp->th_seq + htonl(1);

    tcp->th_seq = my_seq;
    tcp->th_flags = (TH_SYN | TH_ACK);
    /* Answering only to selected ports */

    lbio_send_tcp(reply_pkt, (ETH_HLEN + IPV6_HLEN + len),
        "Initial Connect - tarpitting");
    break;
  case (TH_SYN | TH_ACK):
    tcp->th_flags = (TH_RST);
    tcp->th_ack = tcp->th_seq + htonl(1);
    tcp->th_seq = ctl.sequence;
    lbio_send_tcp(reply_pkt, (ETH_HLEN + IPV6_HLEN + len), "Inbound SYN/ACK");
    break;
  case (TH_ACK):
    tcp->th_win = ctl.throttlesize;

    uint32_t atack_seq = tcp->th_seq - htonl(1); /* take his seq - 1 */

    atack_seq ^= ntohl(ip6->ip6_src.s6_addr32[3]);
    atack_seq ^= ((tcp->th_sport << 16) + tcp->th_dport); /* decode it */

    //u_int length = ETH_HLEN + sizeof(struct ip6_hdr) + sizeof(struct tcphdr);
    uint32_t tmp_ack = tcp->th_ack - htonl(1); /* take my seq -1 */

    if (tmp_ack == atack_seq) /* if they matches it's ACK in 3-way */
      return;

#ifdef DEBUG
    warnx("ip.plen=%x, %i , len= %x, %i ", ntohs(ip6->ip6_plen), ntohs(
            ip6->ip6_plen), len, len);
#endif

    tcp->th_ack = tcp->th_seq + ctl.throttlesize; /* otherwise win probe or other communication so we send tcp choke packet*/

    tcp->th_seq = tmp_ack + htonl(1);
    lbio_send_tcp(reply_pkt, (ETH_HLEN + IPV6_HLEN + len),
        "Inbound ACK - choke paket sent");

    break;
    }

}

/*
 * This function is called when packet arrives. Parse the packet and call proper handler.
 */

void
pkt_handler(u_char* client_data, const struct pcap_pkthdr* header,
    const u_char* packet)
{

  /* declare pointers to packet headers */
  const struct ethhdr *eth; /* The ethernet header [1] */
  const struct ip6_hdr *ip6;
  const struct icmp6_hdr *icmp6;
  const u_char *next_header;
  uint8_t next_proto;

  /**************************************************************************
   * ether header is commented because of testing on ipv6 tunnel that cuts eth header.
   * remember to add ETH_HLEN to packet pointer when casting to packet structs!!!
   ***************************************************************************/

  /* define ethernet header */
  eth = (struct ethhdr*) (packet);

  /* define/compute ip header offset */
  ip6 = (struct ip6_hdr*) (packet + ETH_HLEN);

  /* do we handle this ip address ?? */
  if ((ctl.ipaddr_filter || io.man_host_info) && (filter_check_ip(ip6)
      == FALSE))
    {
      /* we do ignore this ip but we report it */
      char dst[INET6_ADDRSTRLEN], src[INET6_ADDRSTRLEN];
      util_print(VERY_VERBOSE, "%s: %s -> %s","Filter communication atempt",
          inet_ntop(AF_INET6, &ip6->ip6_dst, dst, INET6_ADDRSTRLEN),
          inet_ntop(AF_INET6, &ip6->ip6_src, src, INET6_ADDRSTRLEN));
      return;
    }

  /* yes. we do.. */
  char dst[INET6_ADDRSTRLEN], src[INET6_ADDRSTRLEN];

  /* determine the type of next protocol by skipping ipv6ext headers in *packet* */
  next_header = next_proto_position(packet, &next_proto);

  switch (next_proto)
    {
  //TCP

  case IPPROTO_TCP: /* we have found a tcp packet */
    if (next_header != NULL)
      tcp_handler(header, packet, next_header);
    break; /* TCP */
    //ICMPv6
  case IPPROTO_ICMPV6: /* we have found the icmpv6 packet */

    icmp6 = (struct icmp6_hdr *) next_header;
    /*
     * ICMPv6 types:
     * ICMP6_ECHO_REQUEST          128 *
     * ICMP6_ECHO_REPLY            129
     * ND_NEIGHBOR_SOLICIT         135 *
     * ND_NEIGHBOR_ADVERT          136
     */
#ifdef DEBUG
    warnx("ICMP type =%d ", icmp6->icmp6_type);
#endif
    switch (icmp6->icmp6_type)
      {
    case ND_NEIGHBOR_SOLICIT: /* neighbor solicitation */
      if (next_header != NULL)
        nd_handler(header, packet, next_header);
      break;
    case ICMP6_ECHO_REQUEST: /* echo request */
      if (next_header != NULL)
        echo_handler(header, packet, next_header);
      break;
      } /* icmp6->icmp6_type */

    break; /* ICMPv6 */
  default:
    /* We cannot determine what kind of protocol the packet carries so we at least print some info. */
    warnx(
        "Next protocol: UNKNOWN!!!\nipsrc=%s -> ipdst=%s, nxt=%d, nxthx = %04x\n",
        inet_ntop(AF_INET6, &ip6->ip6_src, src, INET6_ADDRSTRLEN), inet_ntop(
            AF_INET6, &ip6->ip6_dst, dst, INET6_ADDRSTRLEN),
        ip6->ip6_nxt,
        ip6->ip6_nxt);

    break;
    } /* switch ip6->ip6_nxt */

  return;

}
