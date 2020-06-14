/**
 * 10.3 パケットキャプチャプログラム
 * 
 * RAWソケットはパケットやフレームを自由に操作できるため、
 * 応用分野が広く、例えばping, traceroute arp、ポートスキャン、ホストスキャン、
 * 冒頭で触れたスイッチハブなど、色々なシステムの作成に適用できる。
 * 
 * ここではパケットキャプチャプログラムを作成する。
 * 受信のみなので、ネットワーク上に異常パケットを送信してネットワークを混乱させたりする心配もなく、
 * LAN内で安心して動作確認も行える。
 * 
 * tcpdumpではフラグメントされたパケットを再構成して表示する。
 * ネットワークの仕組みを理解するときは元のままのパケットを見たいことがあrう
 * 
 * またMACアドレスなどイーサネットレベルの情報は表示されない。
 * 
 * ここでは届いたパケットをそのまま表示し、全ての情報を表示する。
 * ここではIP(TCP/UDP/ICMP/IP), ARPを対象にした。
 * 
 * 起動時に監視するネットワークインタフェース名を指定し、さらにオプションで
 * TCP/UDP/ICMP/ARPの表示をそれぞれオフにすることや、TCP/UDPの場合には
 * ポート番号を1つに絞るか、指定したポート番号以外とするようにできる。
 */

/**
 * ヘッダファイルのインクルード
 * はじめに必要なヘッダファイルをインクルードする
 */
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/if.h> // add

#include <arpa/inet.h>
#include <net/ethernet.h> // add
#include <netinet/in.h> // add
#include <netinet/ip.h> // add
#include <netinet/ip6.h> // add
#include <netinet/tcp.h> // add
#include <netinet/udp.h> // add
#include <netinet/ip_icmp.h> // add
#include <netinet/if_ether.h> // add
#include <netinet/in.h>
#include <netpacket/packet.h> // add
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

/**
 * グローバル変数
 */
// ソケットディスクリプタ
static int g_soc = -1;
// パラメータ
struct {
    char *device;
    int arp, icmp, tcp, udp;
    int port;
} g_param = {"", 1, 1, 1, 1, 0};
// 終了フラグ
static int g_gotsig = 0;

// 境界線表示
#define print_separator() { (void) printf("============================" \
    "============================" \
    "============================\n"); }

/**
 * 汎用データ表示
 * 
 * TCPやUDPのデータ部分を表示するための関数
 * 
 * 横方向に16バイト、16進表示し、その隣に印字可能文字はキャラクタ表示も行う。
 * telnetやhttpの通信をパケットキャプチャし、通信内容を見る際、キャラクタ表示が役に立つ。
 */
void print_data(const uint8_t *data, size_t size)
{
    int i, j;
    (void) printf("data----------------------------------\n");
    for (i = 0; i < size; i++) {
        for (j = 0; j < 16; j++) {
            if (j != 0) {
                (void) printf(" ");
            }
            if (i + j < size) {
                (void) printf("%02X", *(data + j));
            } else {
                (void) printf(" ");
            }
        }

        (void) printf("   ");
        for (j = 0; j < 16; j++) {
            if (i < size) {
                if (isascii(*data) && isprint(*data)) {
                    (void) printf("%c", *data);
                } else {
                    (void) printf(".");
                }
                data++;
                i++;
            } else {
                (void) printf(" ");
            }
        }
        (void) printf("\n");
    }
}

/**
 * イーサネットヘッダの表示
 * 
 * 宛先、送信元のMACアドレスと、パケットに含まれるデータのタイプを表示
 * 
 * IPv6パケットはether_typeが0x86DDだが、Linuxのヘッダに定義されていない
 * 本来はプログラムの先頭でdefineで定義するべきだが、ここではあえて直接書き込む。
 */
void print_ether_header(struct ether_header *eh)
{
    int i;
    (void) printf("ether_header-------------------------------\n");
    (void) printf("ether_dhost = ");
    for (i = 0; i < ETHER_ADDR_LEN; i++) {
        if (i != 0) {
            (void) printf(":");
        }
        (void) printf("%02X", eh->ether_dhost[i]);
    }
    (void) printf("\n");
    (void) printf("ether_shost = ");
    for (i = 0; i < ETHER_ADDR_LEN; i++) {
        if (i != 0) {
            (void) printf(":");
        }
        (void) printf("%02X", eh->ether_shost[i]);
    }
    (void) printf("\n");
    (void) printf("ether_type = %02X", ntohs(eh->ether_type));
    switch (ntohs(eh->ether_type)) {
    case ETHERTYPE_PUP:
        (void) printf("(Xerox PUP) \n");
        break;
    case ETHERTYPE_IP:
        (void) printf("(IP)\n");
        break;
    case ETHERTYPE_ARP:
        (void) printf("(Address resolution)\n");
        break;
    case ETHERTYPE_REVARP:
        (void) printf("(Reverse ARP)\n");
        break;
    case 0x86DD: // IPv6
        (void) printf("(IPv6) \n");
        break;
    default:
        (void) printf("(unknown) \n");
        break;
    }
}

/**
 * ARPデータの表示
 * 
 * ARPはアドレス解決のために使われ、主にIPアドレスに対応するMACアドレスを取得するのに使用される。
 * ARPパケットには数種類の役割があるが、データの形式は1種類
 */
void print_ether_arp(struct ether_arp *ether_arp)
{
    static char *hrd[] = {
        "from KA9Q: NET/ROM pseudo.",
        "Ethernet 10/10Mbps.",
        "Experimental Ethernet.",
        "AX.25 Level 2.",
        "PROnet token ring",
        "Chaosnet.",
        "IEEE 802.2 Ethernet/TR/TB.",
        "ARCnet.",
        "APPLEtalk.",
        "undefine",
        "undefine",
        "undefine",
        "undefine",
        "undefine",
        "undefine",
        "Frame Relay DLCI.",
        "undefine",
        "undefine",
        "undefine",
        "ATM.",
        "undefine",
        "undefine",
        "undefine",
        "Metricom STRIP (new IANA id)."
    };
    static char *op[] = {
        "undefined",
        "ARP request",
        "ARP reply",
        "RARP request.",
        "RARP reply.",
        "undefined",
        "undefined",
        "undefined",
        "InARP request.",
        "InARP reply.",
        "(ATM)ARP NAK."
    };
    int i;
    (void) printf("ether_arp------------------------------------------\n");
    (void) printf("arp_hrd = %u", ntohs(ether_arp->arp_hrd));
    if (ntohs(ether_arp->arp_hrd) <= 23) {
        (void) printf("(%s),", hrd[ntohs(ether_arp->arp_hrd)]);
    } else {
        (void) printf("(undefined), ");
    }
    (void) printf("arp_pro = %u", ntohs(ether_arp->arp_pro));
    switch(ntohs(ether_arp->arp_pro)) {
    case ETHERTYPE_PUP:
        (void) printf("(Xerox POP)\n");
        break;
    case ETHERTYPE_IP:
        (void) printf("(IP)\n");
        break;
    case ETHERTYPE_ARP:
        (void) printf("(Address resolution)\n");
        break;
    case ETHERTYPE_REVARP:
        (void) printf("(Reverse ARP)\n");
        break;
    default:
        (void) printf("(unknown)\n");
        break;
    }
    (void) printf("arp_hln = %u, ", ether_arp->arp_hln);
    (void) printf("arp_pln = %u, ", ether_arp->arp_pln);
    (void) printf("arp_op = %u", ntohs(ether_arp->arp_op));
    if (ntohs(ether_arp->arp_op) <= 10) {
        (void) printf("(%s)\n", op[ntohs(ether_arp->arp_op)]);
    } else {
        (void) printf("(undefine)\n");
    }
    (void) printf("arp_sha = ");
    for (i = 0; i < ether_arp->arp_hln; i++) {
        if (i != 0) {
            (void) printf(":");
        }
        (void) printf("%02X", ether_arp->arp_sha[i]);
    }
    (void) printf("\n");
    (void) printf("arp_spa = ");
    for (i = 0; i < ether_arp->arp_pln; i++) {
        if (i != 0) {
            (void) printf(".");
        }
        (void) printf("%u", ether_arp->arp_spa[i]);
    }
    (void) printf("\n");
    (void) printf("arp_tha = ");
    for (i = 0; i < ether_arp->arp_hln; i++) {
        if (i != 0) {
            (void) printf(":");
        }
        (void) printf("%02X", ether_arp->arp_tha[i]);
    }
    (void) printf("\n");
    (void) printf("arp_tpa = ");
    for (i = 0; i < ether_arp->arp_pln; i++) {
        if (i != 0) {
            (void) printf(".");
        }
        (void) printf("%u", ether_arp->arp_tpa[i]);
    }
    (void printf("\n"));
}

/**
 * IPv4ヘッダの表示
 */
void print_ip(struct ip *ip)
{
    static char *proto[] = {
        "undeinfed",
        "ICMP",
        "IGMP",
        "undeinfed",
        "IPIP",
        "undeinfed",
        "TCP",
        "undeinfed",
        "EGP",
        "undeinfed",
        "undeinfed",
        "undeinfed",
        "PUP",
        "undeinfed",
        "undeinfed",
        "undeinfed",
        "undeinfed",
        "UDP"
    };
    (void) printf("ip==============================\n");
    (void) printf("ip_v = %u, ", ip->ip_v);
    (void) printf("ip_hl = %u, ", ip->ip_hl);
    (void) printf("ip_tos = %x, ", ip->ip_tos);
    (void) printf("ip_len = %d\n", ntohs(ip->ip_len));
    (void) printf("ip_id = %u, ", ntohs(ip->ip_id));
    (void) printf("ip_off = %x, %d\n",
                    (ntohs(ip->ip_off))>>13&0x07,
                    ntohs(ip->ip_off)&0x1FFF);
    (void) printf("ip_ttl = %u, ", ip->ip_ttl);
    (void) printf("ip_p = %u", ip->ip_p);
    if (ip->ip_p <= 17) {
        (void) printf("(%s), ", proto[ip->ip_p]);
    } else {
        (void) printf("(undefined), ");
    }
    (void) printf("ip_sum = %u\n", ntohs(ip->ip_sum));
    (void) printf("ip_src = %s\n", inet_ntoa(ip->ip_src));
    (void) printf("ip_dst = %s\n", inet_ntoa(ip->ip_dst));
}

/**
 * IPv6ヘッダの表示
 */
void print_ipv6(struct ip6_hdr *ip6_hdr)
{
    char buf[256];
    static char *proto[] = {
        "undeinfed",
        "ICMP",
        "IGMP",
        "undeinfed",
        "IPIP",
        "undeinfed",
        "TCP",
        "undeinfed",
        "EGP",
        "undeinfed",
        "undeinfed",
        "undeinfed",
        "PUP",
        "undeinfed",
        "undeinfed",
        "undeinfed",
        "undeinfed",
        "UDP"
    };
    (void) printf("ip6==============================\n");
    (void) printf("ip6_v = %u, ", ip6_hdr->ip6_vfc);
    (void) printf("ip6_flow = %u, ", ip6_hdr->ip6_flow);
    (void) printf("ip6_plen = %x, ", ip6_hdr->ip6_plen);
    if (ip6_hdr->ip6_nxt <= 17) {
        (void) printf("(%s), ", proto[ip6_hdr->ip6_nxt]);
    } else {
        (void) printf("(undefined), ");
    }
    (void) printf("ip6_hlim = %u\n", ip6_hdr->ip6_hlim);
    (void) printf("ip6_src = %s\n", inet_ntop(AF_INET6,
                                                &ip6_hdr->ip6_src,
                                                buf,
                                                sizeof(buf)));

    (void) printf("ip6_dst = %s\n", inet_ntop(AF_INET6,
                                                &ip6_hdr->ip6_dst,
                                                buf,
                                                sizeof(buf)));
}

/**
 * TCPヘッダの表示
 */
void print_tcphdr(struct tcphdr *tcphdr)
{
    (void) printf("tcphdr--------------------------------------\n");
    (void) printf("source = %u, ", ntohs(tcphdr->source));
    (void) printf("dest = %u\n", ntohs(tcphdr->dest));
    (void) printf("seq = %u\n", ntohl(tcphdr->seq));
    (void) printf("ack_seq = %u\n", ntohl(tcphdr->ack_seq));
    (void) printf("doff = %u, ", tcphdr->doff);
    (void) printf("urg = %u, ", tcphdr->urg);
    (void) printf("ack = %u, ", tcphdr->ack);
    (void) printf("psh = %u, ", tcphdr->psh);
    (void) printf("rst = %u, ", tcphdr->rst);
    (void) printf("syn = %u, ", tcphdr->syn);
    (void) printf("fin = %u, ", tcphdr->fin);
    (void) printf("th_win = %u\n ", ntohs(tcphdr->window)));
    (void) printf("th_sum = %u, ", ntohs(tcphdr->check)));
    (void) printf("th_urp = %u\n ", ntohs(tcphdr->urg_ptr)));
}

/**
 * TCPヘッダのオプションの表示
 * 
 * シンプルにオプションとパディング分を16進表示する
 */
void print_tcp_optpad(unsigned char *data, int size)
{
    int i;
    (void) printf("option, pad = ");
    for (i = 0; i < size; i++) {
        if (i != 0) {
            (void) printf(", ");
        }
        (void) printf("%x", *data);
        data++;
    }
    (void) printf("\n");
}

/**
 * UDPヘッダの表示
 */
void print_udphdr(struct udphder *udphdr)
{
    (void) printf("udphdr--------------------------------------\n");
    (void) printf("source = %u, ", ntohs(udphdr->source));
    (void) printf("dest = %u\n", ntohs(udphdr->dest));
    (void) printf("len = %u, ", ntohs(udphdr->len));
    (void) printf("check = %u\n", ntohs(udphdr->check));
}

/**
 * ICMPデータの表示
 * TODO
 */
void print_icmp(struct icmp *icmp, unsigned char *hptr, int size)
{
    static char *type[] = {
        "Echo Reply",
        "undefined",
        "undefined",
        "destination Unreachable",
        "Source Quench",
        "Redirect",
        "undefined",
        "undefined",
        "Echo Request",
        "Router Adverisement",
        "Router Seletion",
        "Time Exceeded for Datagram",
        "Timestamp Request",
        "Timestamp Reply",
        "Information Request",
        "Information REply",
        "Address Mask Request",
        "Address Mask Reply"
    };
    struct tcphdr tcphdr;
    struct udphdr udphdr;
    (void) printf("icmp----------------------------------------------\n");
    (void) printf("icmp_type = %u", icmp->icmp_type);
    if (icmp->icmp_type <= 18) {
        (void) printf("(%s), ", type[icmp->icmp_type]);
    } else {
        (void) printf("(undefined),");
    }
    (void) printf("icmp_code = %u, ", icmp->icmp_code);
    (void) printf("icmp_cksum = %u\n", ntohs(icmp->icmp_cksum));
    if (icmp->icmp_type == 0 || icmp->icmp_type == 8) {
        (void) printf("icmp_id = %u, ", ntohs(icmp->icmp_id));
        (void) printf("icmp_seq = %u\n", ntohs(icmp->icmp_seq));
        print_data(hptr+8, size-8);
    } else if (icmp->icmp_type == 3) {
        if (icmp->icmp_code ==4) {
            (void) printf("icmp_pmvoid = %u\n", ntohs(icmp->icmp_pmvoid));
            (void) printf("icmp_nextmtu = %u\n", ntohs(icmp->icmp_nextmtu));
        } else {
            (void) printf("icmp_void = %u\n", ntohs(icmp->icmp_void));
        }
    } else if (icmp->icmp_type == 5) {
        (void) printf("icmp_gwaddr = %s\n", inet_ntoa(icmp->icmp_gwaddr));
    } else if (icmp->icmp_type == 11) {
        (void) printf("icmp_void = %u\n", ntohs(icmp->icmp_void));
    }
    if (icmp->icmp_type == 3 || icmp->icmp_type == 5 || icmp->icmp_type == 11) {
        print_ip(&icmp->icmp_ip);
        if (icmp->icmp_ip.ip_p == IPPROTO_TCP) {
            hptr += 8;
            size -= 8;
            hptr += sizeof(struct ip);
            size -= sizeof(struct ip);
            (void) memcpy(&tcphdr, hptr, sizeof(struct tcphdr));
            hptr += sizeof(struct tcphdr);
            size -= sizeof(struct tcphdr);
            print_tcphdr(&tcphdr);
        } else if (icmp->icmp_ip.ip_p == IPPROTO_UDP) {
            hptr += 8;
            size -= 8;
            hptr += sizeof(struct ip);
            size -= sizeof(struct ip);
            (void) memcpy(&udphdr, hptr, sizeof(struct udphdr));
            hptr += sizeof(struct udphdr);
            size -= sizeof(struct udphdr);
            print_udphdr(&udphdr);
        }
    }
}

/**
 * 対象ポート判定
 *
 * TCP/UDPの場合に対象ポート限定または、除外する機能を、起動時の引数で指定できるようにする。
 * そのための関数
 * 
 * 負数でしたいされた場合は、正にした値のポートを除外させる。
 */
int is_target_port(uint16_t port1, uint16_t port2)
{
    int flag;
    if (g_param.port == 0) {
        flag = 1;
    } else if (g_param.port > 0) {
        if (g_param.port == port1 || g_param.port == port2) {
            flag = 1;
        } else {
            flag = 0;
        }
    } else {
        if (-g_param.port == port1 || -g_param.port == port2) {
            flag = 0;
        } else {
            flag = 1;
        }
    }
    return (flag);
}

/**
 * パケット解析
 * 
 * 受信したパケットを解析し、それぞれのプロトコルに応じた構造体に格納し、表示関数を呼び出す
 */
void analyze_packet(uint8_t *ptr, size_t len)
{
    struct ether_header eh;
    struct ether_arp ether_arp;
    struct ip ip;
    struct ip6_hdr ip6_hdr;
    struct tcphdr tcphdr;
    struct udphdr udphdr;
    struct icmp icmp;
    uint8_t *hptr;
    int size;
    int lest;

    // イーサネットヘッダ取得
    (void) memcpy(&eh, ptr, sizeof(struct ether_header));
    ptr += sizeof(struct ether_header);
    len -= sizeof(struct ether_header);
    if (ntohs(eh.ether_type) == ETHERTYPE_ARP || ntohs(eh.ether_type) == ETHERTYPE_REVARP) {
        // ARP関連パケット
        if (g_param.arp) {
            // ARPデータ取得
            (void) memcpy(&ether_arp, ptr, sizeof(struct ether_arp));
            print_separator();
            printf("[ARP]\n");
            print_ether_header(&eh);
            print_ether_arp(&ether_arp);
            print_separator();
            (void) printf("\n");
        }
    } else if (ntohs(eh.ether_type) == ETHERTYPE_IP) {
        // IPパケット
        // IPヘッダ取得
        (void) memcpy(&ip, ptr, sizeof(struct ip));
        ptr += sizeof(struct ip);
        len = ntohs(ip.ip_len) - sizeof(struct ip);
        if (ip.ip_p == IPPROTO_TCP) {
            // TCPパケット
            if (g_param.tcp) {
                // TCPヘッダ取得
                (void) memcpy(&tcphdr, ptr, sizeof(struct tcphdr));
                ptr += sizeof(struct tcphdr);
                len -= sizeof(struct tcphdr);
                if (is_target_port(ntohs(tcphdr.source), ntohs(tcphdr.dest))) {
                    print_separator();
                    printf("[TCP]\n");
                    print_ether_header(&eh);
                    print_ip(&ip);
                    print_tcphdr(&tcphdr);
                    lest = tcphdr.doff * 4 - sizeof(struct tcphdr);
                    if (lest > 0) {
                        // オプションあり
                        print_tcp_optpad(ptr, lest);
                    }
                    ptr += lest;
                    len -= lest;
                    if (len > 0) {
                        // データあり
                        print_data(ptr, len);
                    }
                    print_separator();
                    (void) printf("\n");
                }
            }
        } else if (ip.ip_p == IPPROTO_UDP) {
            // UDPパケット
            if (g_param.udp) {
                // UDPヘッダ取得
                (void) memcpy(&udphdr, ptr, sizeof(struct udphdr));
                ptr += sizeof(struct udphdr);
                len -= sizeof(struct udphdr);
                if (is_target_port(ntohs(udphdr.source), ntohs(udphdr.dest))) {
                    print_separator();
                    printf("[UDP]\n");
                    print_ether_header(&eh);
                    print_ip(&ip);
                    print_udphdr(&udphdr);
                    if (len > 0) {
                        // データあり
                        print_data(ptr, len);
                    }
                    print_separator();
                    (void) printf("\n");
                }
            }
        } else if (ip.ip_p == IPPROTO_ICMP) {
            // ICMPパケット
            if (g_param.icmp) {
                hptr = ptr;
                size = len;
                // ICMPデータ取得
                (void) memcpy(&icmp, ptr, sizeof(struct icmp));
                ptr += sizeof(struct icmp);
                len -= sizeof(struct icmp);
                print_separator();
                (void) printf("[ICMP]\n");
                print_ether_header(&eh);
                print_ip(&ip);
                print_icmp(&icmp, hptr, size);
                print_separator();
                (void) printf("\n");
            } 
        }
    } else if (ntohs(eh.ether_type) == 0x86DD) {
        // IPv6パケット
        // IPv6ヘッダ取得
        (void) memcpy(&ip6_hdr, ptr, sizeof(struct ip6_hdr));
        ptr += sizeof(struct ip6_hdr);
        len -= sizeof(struct ip6_hdr);
        if (ip6_hdr.ip6_nxt == IPPROTO_TCP) {
            // TCPパケット
            if (g_param.tcp) {
                // TCPヘッダ取得
                (void) memcpy(&tcphdr, ptr, sizeof(struct tcphdr));
                ptr += sizeof(struct tcphdr);
                len -= sizeof(struct tcphdr);
                if (is_target_port(ntohs(tcphdr.source), ntohs(tcphdr.dest))) {
                    print_separator();
                    printf("[TCP6]\n");
                    print_ether_header(&eh);
                    print_ipv6(&ip6_hdr);
                    print_tcphdr(&tcphdr);
                    lest = tcphdr.doff * 4 - sizeof(struct tcphdr);
                    if (lest > 0) {
                        // オプションあり
                        print_tcp_optpad(ptr, lest);
                    }
                    ptr += lest;
                    len -= lest;
                    if (len > 0) {
                        // データあり
                        print_data(ptr, len);
                    }
                    print_separator();
                    (void) printf("\n");
                }
            }
        }
    } else if (ip6_hdr.ip6_nxt == IPPROTO_UDP) {
        // UDPパケット
        if (g_param.dup) {
            // UDPヘッダ取得
            (void) memcpy(&udphdr, ptr, sizeof(struct udphdr));
            ptr += sizeof(struct udphdr);
            len -= sizeof(struct udphdr);
            if (is_target_port(ntohs(udphdr.source), ntohs(udphdr.dest))) {
                print_separator();
                printf("[UDP6]\n");
                print_ether_header(&eh);
                print_ipv6(&ip6_hdr);
                print_udphdr(&udphdr);
                if (len > 0) {
                    // データあり
                    print_data(ptr, len);
                }
                print_separator();
                (void) printf("\n");
            }
        }
    }
}

/**
 * キャプチャ・ループ
 * 
 * イーサネットレベルのパケットを扱うRAWソケットでは、受信はrecv()を使う。
 * select()で多重化しているが、現状ではread()でいきなり処理をブロックしても処理上問題ない
 */
void capture_loop(void)
{
    uint8_t buf[2048];
    struct timeval timeout;
    fd_set mask;
    int width;
    ssize_t len;
    while (g_gotsig == 0) {
        // select()用マスクの作成
        FD_ZERO(&mask);
        FD_SET(g_soc, &mask);
        width = g_soc + 1;
        // select()用タイムアウト値のセット
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        switch (select(width, (fd_set *) &mask, NULL, NULL, &timeout)) {
        case -1:
            // error
            perror("select");
            break;
        case 0:
            // timeout
            break;
        default:
            // ready 有り
            if (FD_ISSET(g_soc, &mask)) {
                if ((len = recv(g_soc, buf, sizeof(buf), 0)) == -1) {
                    perror("read");
                } else {
                    // パケット解析
                    analyze_packet(buf, len);
                }
            }
            break;
        }
    }
}

/**
 * 指定した名前のインタフェース情報の表示
 * 
 * プログラム起動時に指定されたインタフェースの情報を表示する
 */
int show_ifreq(int soc, const char *name)
{
    char addr_str[256];
    struct ifreq ifreq;
    int i;
    struct ifaddrs *ifaddrs;
    struct ifaddrs *ifa;
    uint8_t *p;

    // ifreq構造体に名前をセット
    (void) snprintf(ifreq.ifr_name, sizeof(ifreq.ifr_name), "%s", name);
    // フラグの取得・表示
    if (ioctl(soc, SIOCGIFFLAGS, &ifreq) == -1) {
        perror("ioctl:SIOCGIFFLAGS");
        return (-1);
    }
    if (ifreq.ifr_flags & IFF_UP) (void) fprintf(stdout, "UP ");
    if (ifreq.ifr_flags & IFF_BROADCAST) (void) fprintf(stdout, "BROADCAST ");
    if (ifreq.ifr_flags & IFF_PROMISC) (void) fprintf(stdout, "PROMISC ");
    if (ifreq.ifr_flags & IFF_MULTICAST) (void) fprintf(stdout, "MULTICAST ");
    if (ifreq.ifr_flags & IFF_LOOPBACK) (void) fprintf(stdout, "LOOPBACK ");
    if (ifreq.ifr_flags & IFF_POINTOPOINT) (void) fprintf(stdout, "P2P ");

    (void) fprintf(stdout, "\n");
    // MTUの取得・表示
    if (ioctl(soc, SIOCGIFMTU, &ifreq) == -1) {
        perror("ioctl:SIOCGIFMTU");
    } else {
        (void) fprintf(stdout, "mtu=%d\n", ifreq.ifr_mtu);
    }
    // getifaddrs()でインタフェースのアドレス一覧をifaddrsに取得
    if (getifaddrs(&ifaddrs) == -1) {
        perror("getifaddrs");
        return (-1);
    }
    // ifaddrsに取得したアドレス一覧のlinked listをたどる
    for (i = 0, ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next) {
        /**
         * getifaddrs()ではアドレスを持つインタフェース全ての情報を取得するため
         * 指定されたインタフェース名以外は除外
         */
        if (strcmp(ifa->ifa_name, name) != 0) {
            continue;
        }
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // IPv4
            // ユニキャストアドレスの取得・表示
            (void) fprintf(stdout, "addr[%d]=%s\n", i, inet_ntop(AF_INET,
                                                        &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                                                        addr_str,
                                                        sizeof(addr_str)));
            if (ifa->ifa_ifu.ifu_dstaddr) {
                // ポイント・ポイントアドレスの取得・表示
                (void) fprintf(stdout, "dstaddr[%d]=%s\n", i, inet_ntop(AF_INET,
                                                            &((struct sockaddr_in *)ifa->ifa_ifu.ifu_dstaddr)->sin_addr,
                                                            addr_str,
                                                            sizeof(addr_str)));
            }
            if (ifa->ifa_ifu.ifu_broadaddr) {
                // ブロードキャストアドレスの取得・表示
                (void) fprintf(stdout, "broadaddr[%d]=%s\n", i, inet_ntop(AF_INET,
                                                            &((struct sockaddr_in *)ifa->ifa_ifu.ifu_broadaddr)->sin_addr,
                                                            addr_str,
                                                            sizeof(addr_str)));
            }
            // ネットマスクの取得・表示
            (void) fprintf(stdout, "netmask[%d]=%s\n", i, inet_ntop(AF_INET,
                                                        &((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr,
                                                        addr_str,
                                                        sizeof(addr_str)));
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            // IPv6
            (void) fprintf(stdout, "add6[%d]=%s\n", i, inet_ntop(AF_INET6,
                                                        &((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr,
                                                        addr_str,
                                                        sizeof(addr_str)));
            if (ifa->ifa_ifu.ifu_dstaddr) {
                // ポイント・ポイントアドレスの取得・表示
                (void) fprintf(stdout, "dstaddr6[%d]=%s\n", i, inet_ntop(AF_INET6,
                                                            &((struct sockaddr_in6 *)ifa->ifa_ifu.ifu_dstaddr)->sin6_addr,
                                                            addr_str,
                                                            sizeof(addr_str)));
            }
            // ネットマスクの取得・表示
            (void) fprintf(stdout, "netmask6[%d]=%s\n", i, inet_ntop(AF_INET6,
                                                        &((struct sockaddr_in6 *)ifa->ifa_netmask)->sin6_addr,
                                                        addr_str,
                                                        sizeof(addr_str)));
        } else {
            continue;
        }
        i++;
    }
    // getifaddrs()が確保したメモリを解放
    freeifaddrs(ifaddrs);
    // MACアドレスの取得・表示
    if (ioctl(soc, SIOCGIFHWADDR, &ifreq) == -1) {
        perror("ioctl:hwaddr");
    } else {
        p = (uint8_t *) &ifreq.ifr_hwaddr.sa_data;
        (void) fprintf(stdout,
                        "hwaddr=%02X:%02X:%02X:%02X:%02X:%02X\n",
                        *p,
                        *(p+1),
                        *(p+2),
                        *(p+3),
                        *(p+4),
                        *(p+5));
        return (0);
    }
}

/**
 * キャプチャ用ソケット準備
 * 
 * ソケットをAF_PACKET, SOCK_RAW, ETH_P_ALLで生成し、
 * イーサネットレベルのパケット(フレーム)を扱うソケットを準備する。
 * 
 * 指定されたインタフェース情報をioctl()で取得し、sockaddr_ll型構造体にセットしてbind()
 * 
 * bind()はIPアドレスやポートをソケットに結びつけることに使うが
 * イーサネットレベルのパケットを扱うソケットでは、インタフェースを指定する時に使う。
 */
int raw_socket(const char *device)
{
    struct ifreq if_req;
    struct sockaddr_ll sa;
    int soc;
    // ソケットの生成
    if ((soc = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) == -1) {
        perror("socket");
        return (-1);
    }
    // インタフェース情報の取得
    (void) snprintf(if_req.ifr_name, sizeof(if_req.ifr_name), "%s", device);
    if (ioctl(soc, SIOCGIFINDEX, &ir_req) == -1) {
        perror("ioctl");
        (void) close(soc);
        return (-1);
    }
    // インタフェースをbind()
    sa.sll_family = PF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    sa.sll_ifindex = if_req.ifr_ifindex;
    if (bind(soc, (struct sockaddr *) &sa, sizeof(sa)) == -1) {
        perror("bind");
        (void) close(soc);
        return (-1);
    }
    // インタフェースのフラグ取得
    if (ioctl(soc, SIOCGIFFLAGS, &if_req) == -1) {
        perror("ioctl");
        (void) close(soc);
        return (-1);
    }
    // インタフェースのフラグにプロミスキャスモード・UPをセット
    if_req.ifr_flags = if_req.ifr_flags | IFF_PROMISC | IFF_UP;
    if (ioctl(soc, SIOCSIFLAGS, &if_req) == -1) {
        perror("ioctl");
        (void) close(soc);
        return (-1);
    }
    return (soc);
}

/**
 * 終了処理
 * 
 * このプログラムはエラーが発生しない限り、永遠にパケットキャプチャを続ける
 * 終了するにはCtrl + CでSIGINTを送る
 * 
 * 今回はインタフェースをプロミスキャスモードにしているためSIGINTを補足し終了処理を行うようにした
 * ここではあくまでフラグをセットするだけ
 */
int sig_int_handler(int sig)
{
    g_gotsig = sig;
} 

/**
 * main 関数
 * 
 * 起動時オプションの解釈を行い、キャプチャ用ソケットを準備、インタフェース情報を表示する
 */
int main(int argc, char *argv[])
{
    struct ifreq if_req;
    int i;
    if (argc <= 1) {
        (void) fprintf(stderr, "pdump device [-tcp] [-udp] [-arp]"
                                "[-icmp] [port-no] [-port-no\n");
        return (EX_USAGE);
    }
    // 起動引数の解釈
    g_param.device = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-tcp") == 0) {
            g_param.tcp = 0;
        } else if (strcmp(argv[i], "-udp") == 0) {
            g_param.udp = 0;
        } else if (strcmp(argv[i], "-arp") == 0) {
            g_param.arp = 0;
        } else if (strcmp(argv[i], "-icmp") == 0) {
            g_param.icmp = 0;
        } else {
            g_param.port = atoi(argv[i]);
        }
    }
    (void) fprintf(stderr,
                    "tcp = %d, udp = %d, arp = %d, icmp = %d, port = %d\n",
                    g_param.tcp,
                    g_param.udp,
                    g_param.arp,
                    g_param.icmp,
                    g_param.port);
    // キャプチャ用ソケットの準備
    if ((g_soc = raw_socket(g_param.device)) == -1) {
        return (EX_OSERR);
    }
    // インタフェース情報の表示
    (void) printf("+++++++++++++++++++++++++++++++++++++\n");
    (void) printf("device = %s\n", g_param.device);
    (void) show_ifreq(g_soc, g_param.device);
    (void) printf("+++++++++++++++++++++++++++++++++++++\n\n");

    // シグナルのセット
    (void) signal(SIGINT, sig_int_handler);
    // キャプチャループ
    capture_loop();
    // 終了処理
    (void) snprintf(if_req.ifr_name, sizeof(if_req.ifr_name), "%s", g_param.device);
    if (ioctl(g_soc, SIOCGIFINDEX, &if_req) == -1) {
        perror("ioctl");
        (void) close(g_soc);
        return (EX_OSERR);
    }
    // インタフェースのフラグ用
    if (ioctl(g_soc, SIOCGIFFLAGS, &if_req) == -1) {
        perror("ioctl");
        (void) close(g_soc);
        return (EX_OSERR);
    }
    // インタフェースのフラグにプロミスキャスモード・UPをセット
    if_req.ifr_flags = if_req.ifr_flags &~IFF_PROMISC;
    if (ioctl(g_soc, SIOCSIFFLAGS, &if_req) == -1) {
        perror("ioctl");
        (void) close(g_soc);
        return (EX_OSERR);
    }
    (void) close(g_soc);
    return (EX_OK);
}


