/*
 * ethernet.c - RA8P1 Ethernet driver (FreeRTOS+TCP wrapper)
 *
 * Initialization sequence:
 *   ethernet_init()
 *     -> FreeRTOS_IPInit()
 *       -> pxFSP_Eth_FillInterfaceDescriptor()  [backward-compatible path]
 *         -> xFSP_Eth_NetworkInterfaceInitialise()
 *           -> R_RMAC_Open()
 *             -> R_LAYER3_SWITCH_Open()
 *               -> PHY init + auto-negotiation
 *           -> creates RXHandlerTask + CheckLinkStatusTask
 *   CheckLinkStatusTask polls R_RMAC_LinkProcess() every 1s
 *   On link-up: calls vIPNetworkUpCalls() -> IP stack becomes operational
 */

#include "ethernet.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "common_data.h"
#include "r_rmac.h"
#include "rpmsg_log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- IP address storage (4-byte arrays for FreeRTOS_IPInit) ---- */
static uint8_t ucMACAddress[6]       = ETH_MAC_ADDR;

#ifdef ETH_USE_DHCP
static uint8_t ucIPAddress[4]        = {0, 0, 0, 0};
static uint8_t ucNetMask[4]          = {255, 255, 255, 0};
static uint8_t ucGatewayAddress[4]   = {0, 0, 0, 0};
static uint8_t ucDNSServerAddress[4] = {0, 0, 0, 0};
static volatile uint32_t dhcp_in_use = 0;
#else
static uint8_t ucIPAddress[4]        = {192, 168, 1, 100};
static uint8_t ucNetMask[4]          = {255, 255, 255, 0};
static uint8_t ucGatewayAddress[4]   = {192, 168, 1, 1};
static uint8_t ucDNSServerAddress[4] = {8, 8, 8, 8};
#endif

static volatile bool s_ethernet_initialized = false;

/* ---- Helper: parse dot-decimal string to 4-byte array ---- */
static void parse_ip(const char *str, uint8_t *out)
{
    uint32_t addr = FreeRTOS_inet_addr(str);
    out[0] = (uint8_t)(addr & 0xFF);
    out[1] = (uint8_t)((addr >> 8) & 0xFF);
    out[2] = (uint8_t)((addr >> 16) & 0xFF);
    out[3] = (uint8_t)((addr >> 24) & 0xFF);
}

/* ---- Helper: format 4-byte array to dot-decimal string ---- */
static void format_ip(const uint8_t *ip, char *buf, int size)
{
    snprintf(buf, size, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

/* ---- Random number generator (required by FreeRTOS+TCP) ---- */
uint32_t ulRand(void)
{
    uint32_t result = ((((uint32_t) rand()) & 0x7fffuL)) |
                      ((((uint32_t) rand()) & 0x7fffuL) << 15) |
                      ((((uint32_t) rand()) & 0x0003uL) << 30);
    return result;
}

/* ---- TCP sequence number hook (required by FreeRTOS+TCP) ---- */
uint32_t ulApplicationGetNextSequenceNumber(uint32_t ulSourceAddress,
                                            uint16_t usSourcePort,
                                            uint32_t ulDestinationAddress,
                                            uint16_t usDestinationPort)
{
    return ((ulSourceAddress + ulDestinationAddress + usSourcePort + usDestinationPort) && ulRand());
}

/* ---- Ping reply hook ---- */
static volatile uint32_t s_ping_received = 0;
static volatile uint32_t s_ping_lost     = 0;

void vApplicationPingReplyHook(ePingReplyStatus_t eStatus, uint16_t usIdentifier)
{
    (void) usIdentifier;
    if (eStatus == eSuccess) {
        s_ping_received++;
    } else {
        s_ping_lost++;
    }
}

#ifdef ETH_USE_DHCP
/* ---- DHCP callback hook (when DHCP mode is enabled) ---- */
eDHCPCallbackAnswer_t xApplicationDHCPHook(eDHCPCallbackPhase_t eDHCPPhase,
                                           uint32_t ulIPAddress)
{
    (void) ulIPAddress;
    switch (eDHCPPhase) {
    case eDHCPPhasePreDiscover:
        break;
    case eDHCPPhasePreRequest:
        dhcp_in_use = 1;
        break;
    default:
        break;
    }
    return eDHCPContinue;
}
#else
/* ---- DHCP callback hook (when static IP mode, stop DHCP immediately) ---- */
eDHCPCallbackAnswer_t xApplicationDHCPHook(eDHCPCallbackPhase_t eDHCPPhase,
                                           uint32_t ulIPAddress)
{
    (void) ulIPAddress;
    (void) eDHCPPhase;
    return eDHCPUseDefaults;
}
#endif

#if (ipconfigDHCP_REGISTER_HOSTNAME == 1)
const char *pcApplicationHostnameHook(void)
{
    return "RA8P1-CPU1";
}
#endif

/* ---- Public API ---- */

int ethernet_init(void)
{
    BaseType_t ret;

    if (s_ethernet_initialized) {
        rpmsg_log_cpu1_printf("[ETH] Already initialized\r\n");
        return 0;
    }

#ifdef ETH_USE_DHCP
    parse_ip(ETH_STATIC_IP, ucIPAddress);         /* fallback address */
    parse_ip(ETH_STATIC_GATEWAY, ucGatewayAddress);
    parse_ip(ETH_STATIC_DNS, ucDNSServerAddress);
    rpmsg_log_cpu1_printf("[ETH] Mode: DHCP (fallback static: %s)\r\n", ETH_STATIC_IP);
#else
    rpmsg_log_cpu1_printf("[ETH] Mode: Static IP\r\n");
#endif

    rpmsg_log_cpu1_printf("[ETH] MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                          ucMACAddress[0], ucMACAddress[1], ucMACAddress[2],
                          ucMACAddress[3], ucMACAddress[4], ucMACAddress[5]);

    char ip_str[16], nm_str[16], gw_str[16], dns_str[16];
    format_ip(ucIPAddress, ip_str, sizeof(ip_str));
    format_ip(ucNetMask, nm_str, sizeof(nm_str));
    format_ip(ucGatewayAddress, gw_str, sizeof(gw_str));
    format_ip(ucDNSServerAddress, dns_str, sizeof(dns_str));
    rpmsg_log_cpu1_printf("[ETH] IP: %s  Mask: %s\r\n", ip_str, nm_str);
    rpmsg_log_cpu1_printf("[ETH] GW: %s  DNS: %s\r\n", gw_str, dns_str);

    ret = FreeRTOS_IPInit(ucIPAddress, ucNetMask, ucGatewayAddress,
                          ucDNSServerAddress, ucMACAddress);
    if (ret != pdTRUE) {
        rpmsg_log_cpu1_printf("[ETH] FreeRTOS_IPInit FAILED\r\n");
        return -1;
    }

    s_ethernet_initialized = true;
    rpmsg_log_cpu1_printf("[ETH] FreeRTOS_IPInit OK, waiting for link...\r\n");
    return 0;
}

eth_status_t ethernet_get_status(void)
{
    BaseType_t ip_up;
    fsp_err_t eth_link;

    if (!s_ethernet_initialized) {
        return ETH_STATUS_LINK_DOWN;
    }

    ip_up = FreeRTOS_IsNetworkUp();
    eth_link = R_RMAC_LinkProcess(g_ether0.p_ctrl);

    if ((eth_link == FSP_SUCCESS) && (ip_up == pdTRUE)) {
        return ETH_STATUS_IP_READY;
    } else if (eth_link == FSP_SUCCESS) {
        return ETH_STATUS_LINK_UP;
    }
    return ETH_STATUS_LINK_DOWN;
}

int ethernet_wait_ready(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;

    while (ethernet_get_status() != ETH_STATUS_IP_READY) {
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;
        if ((timeout_ms > 0) && (elapsed >= timeout_ms)) {
            rpmsg_log_cpu1_printf("[ETH] Wait timeout after %lu ms\r\n", (unsigned long)elapsed);
            return -1;
        }
    }

#ifdef ETH_USE_DHCP
    if (dhcp_in_use) {
        ucNetMask[0] = (uint8_t)(FreeRTOS_GetNetmask() & 0xFF);
        ucNetMask[1] = (uint8_t)((FreeRTOS_GetNetmask() >> 8) & 0xFF);
        ucNetMask[2] = (uint8_t)((FreeRTOS_GetNetmask() >> 16) & 0xFF);
        ucNetMask[3] = (uint8_t)((FreeRTOS_GetNetmask() >> 24) & 0xFF);
        ucGatewayAddress[0] = (uint8_t)(FreeRTOS_GetGatewayAddress() & 0xFF);
        ucGatewayAddress[1] = (uint8_t)((FreeRTOS_GetGatewayAddress() >> 8) & 0xFF);
        ucGatewayAddress[2] = (uint8_t)((FreeRTOS_GetGatewayAddress() >> 16) & 0xFF);
        ucGatewayAddress[3] = (uint8_t)((FreeRTOS_GetGatewayAddress() >> 24) & 0xFF);
        ucDNSServerAddress[0] = (uint8_t)(FreeRTOS_GetDNSServerAddress() & 0xFF);
        ucDNSServerAddress[1] = (uint8_t)((FreeRTOS_GetDNSServerAddress() >> 8) & 0xFF);
        ucDNSServerAddress[2] = (uint8_t)((FreeRTOS_GetDNSServerAddress() >> 16) & 0xFF);
        ucDNSServerAddress[3] = (uint8_t)((FreeRTOS_GetDNSServerAddress() >> 24) & 0xFF);
    }
#endif

    char ip_str[16];
    ethernet_get_ip(ip_str, sizeof(ip_str));
    rpmsg_log_cpu1_printf("[ETH] Network UP: %s\r\n", ip_str);
    return 0;
}

int ethernet_get_ip(char *buf, int size)
{
    format_ip(ucIPAddress, buf, size);
    return 0;
}

int ethernet_get_netmask(char *buf, int size)
{
    format_ip(ucNetMask, buf, size);
    return 0;
}

int ethernet_get_gateway(char *buf, int size)
{
    format_ip(ucGatewayAddress, buf, size);
    return 0;
}

int ethernet_ping(const char *target_ip)
{
    uint32_t addr = FreeRTOS_inet_addr(target_ip);
    if (addr == 0) {
        rpmsg_log_cpu1_printf("[PING] Invalid IP: %s\r\n", target_ip);
        return -1;
    }
    s_ping_received = 0;
    s_ping_lost = 0;
    BaseType_t ret = FreeRTOS_SendPingRequest(addr, 8, pdMS_TO_TICKS(1000));
    if (ret == pdFALSE) {
        return -1;
    }
    return 0;
}

bool ethernet_is_dhcp(void)
{
#ifdef ETH_USE_DHCP
    return (dhcp_in_use != 0);
#else
    return false;
#endif
}

/* ---- UDP Socket Helper API ---- */

Socket_t ethernet_udp_create(void)
{
    Socket_t sock = FreeRTOS_socket(FREERTOS_AF_INET,
                                    FREERTOS_SOCK_DGRAM,
                                    FREERTOS_IPPROTO_UDP);
    if (sock == FREERTOS_INVALID_SOCKET) {
        rpmsg_log_cpu1_printf("[UDP] Socket create failed\r\n");
    }
    return sock;
}

int ethernet_udp_bind(Socket_t sock, uint16_t port)
{
    struct freertos_sockaddr bind_addr;
    BaseType_t ret;

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_port = FreeRTOS_htons(port);
    bind_addr.sin_addr = 0;  /* INADDR_ANY */

    ret = FreeRTOS_bind(sock, &bind_addr, sizeof(bind_addr));
    if (ret != 0) {
        rpmsg_log_cpu1_printf("[UDP] Bind port %u failed\r\n", port);
        return -1;
    }
    return 0;
}

int ethernet_udp_sendto(Socket_t sock, const void *data, uint32_t len,
                        const char *dest_ip, uint16_t dest_port)
{
    struct freertos_sockaddr dest_addr;

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_port = FreeRTOS_htons(dest_port);
    dest_addr.sin_addr = FreeRTOS_inet_addr(dest_ip);

    int32_t sent = FreeRTOS_sendto(sock, data, len, 0,
                                   &dest_addr, sizeof(dest_addr));
    return (int)sent;
}

int ethernet_udp_recvfrom(Socket_t sock, void *buf, uint32_t buf_len,
                          uint32_t timeout_ms,
                          char *src_ip, int src_ip_len, uint16_t *src_port)
{
    struct freertos_sockaddr sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    TickType_t timeout_ticks;

    if (timeout_ms == 0) {
        timeout_ticks = portMAX_DELAY;
    } else {
        timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    }

    FreeRTOS_setsockopt(sock, 0, FREERTOS_SO_RCVTIMEO,
                        &timeout_ticks, sizeof(timeout_ticks));

    int32_t received = FreeRTOS_recvfrom(sock, buf, buf_len, 0,
                                         &sender_addr, &sender_len);
    if (received > 0) {
        if (src_ip != NULL && src_ip_len > 0) {
            uint32_t ip = sender_addr.sin_addr;
            snprintf(src_ip, src_ip_len, "%d.%d.%d.%d",
                     (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
                     (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
        }
        if (src_port != NULL) {
            *src_port = FreeRTOS_ntohs(sender_addr.sin_port);
        }
    }

    return (int)received;
}

void ethernet_udp_close(Socket_t sock)
{
    if (sock != FREERTOS_INVALID_SOCKET) {
        FreeRTOS_closesocket(sock);
    }
}
