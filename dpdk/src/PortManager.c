#include "PortManager.h"
#include "Common.h"
#include <string.h>
#include "Port.h"
#include "Config.h"

int initialize_ports(struct ports_config *config)
{
    uint16_t port_id;
    int port_count = 0;

    if (!config)
    {
        printf("Error: Invalid configuration pointer\n");
        return -1;
    }

    // Initialize the configuration
    memset(config, 0, sizeof(struct ports_config));

    printf("Scanning for DPDK ports...\n");

    // Iterate through all available ports
    RTE_ETH_FOREACH_DEV(port_id)
    {
        if (port_count >= MAX_PORTS)
        {
            printf("Warning: Maximum ports limit (%d) reached\n", MAX_PORTS);
            break;
        }

        struct port *port = &config->ports[port_count];
        struct rte_eth_dev_info dev_info;

        // Get device information
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret != 0)
        {
            printf("Warning: Cannot get device info for port %u\n", port_id);
            continue;
        }

        // Fill port information
        port->port_id = port_id;
        port->is_valid = true;

        // Get driver name
        if (dev_info.driver_name)
        {
            strncpy(port->driver_name, dev_info.driver_name, 31);
            port->driver_name[31] = '\0';
        }
        else
        {
            strcpy(port->driver_name, "Unknown");
        }

        // Placeholder PCI address
        snprintf(port->pci_addr, PCI_ADDR_LEN, "Port-%u-PCI", port_id);

        // Get MAC address
        ret = rte_eth_macaddr_get(port_id, &port->mac_addr);
        if (ret != 0)
        {
            printf("Warning: Cannot get MAC address for port %u\n", port_id);
            memset(&port->mac_addr, 0, sizeof(struct rte_ether_addr));
        }

        printf("Discovered port %u: %s\n", port_id, port->driver_name);
        port_count++;
    }

    config->nb_ports = port_count;
    printf("Found %d DPDK ports\n", port_count);

    return port_count;
}

void print_ports_info(const struct ports_config *config)
{
    if (!config || config->nb_ports == 0)
    {
        printf("No ports available\n");
        return;
    }

    printf("\n=== Port Information ===\n");
    printf("Total ports: %u\n\n", config->nb_ports);

    for (uint16_t i = 0; i < config->nb_ports; i++)
    {
        const struct port *port = &config->ports[i];

        printf("Port %u:\n", port->port_id);
        printf("  Numa Node: %u\n", port->numa_node);
        printf("  PCI Address: %s\n", port->pci_addr);
        printf("  Driver: %s\n", port->driver_name);
        printf("  MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
               port->mac_addr.addr_bytes[0], port->mac_addr.addr_bytes[1],
               port->mac_addr.addr_bytes[2], port->mac_addr.addr_bytes[3],
               port->mac_addr.addr_bytes[4], port->mac_addr.addr_bytes[5]);

        printf("  Tx cores: ");
        for (uint16_t tx_core = 0; tx_core < NUM_TX_CORES; tx_core++)
        {
            printf("%u ", port->used_tx_cores[tx_core]);
        }
        printf("\n");

        printf("  Rx cores: ");
        for (uint16_t rx_core = 0; rx_core < NUM_RX_CORES; rx_core++)
        {
            printf("%u ", port->used_rx_cores[rx_core]);
        }
        printf("\n");

#if DPDK_EXT_TX_ENABLED
        if (i < DPDK_EXT_TX_PORT_COUNT && port->used_ext_tx_core != 0)
        {
            printf("  Ext TX core: %u\n", port->used_ext_tx_core);
        }
#endif

#if PTP_ENABLED
        if (port->used_ptp_core != 0)
        {
            printf("  PTP core: %u\n", port->used_ptp_core);
        }
#endif

        printf("  Status: %s\n", port->is_valid ? "Valid" : "Invalid");
        printf("\n");
    }
}

void print_ports_by_card(const struct ports_config *config)
{
    if (!config || config->nb_ports == 0)
    {
        printf("No ports available\n");
        return;
    }

    printf("\n=== Ports Grouped by NIC Card ===\n");

    for (uint16_t i = 0; i < config->nb_ports; i += 2)
    {
        const struct port *port1 = &config->ports[i];

        // Extract bus info from first port
        char bus_info[16] = "Unknown";
        const char *colon = strchr(port1->pci_addr, ':');
        if (colon)
        {
            const char *second_colon = strchr(colon + 1, ':');
            if (second_colon)
            {
                size_t len = second_colon - port1->pci_addr;
                if (len < sizeof(bus_info))
                {
                    strncpy(bus_info, port1->pci_addr, len);
                    bus_info[len] = '\0';
                }
            }
        }

        printf("NIC Card %u (%s):\n", (i / 2) + 1, bus_info);

        // First port
        printf("  Port %u: %s (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
               port1->port_id, port1->pci_addr,
               port1->mac_addr.addr_bytes[0], port1->mac_addr.addr_bytes[1],
               port1->mac_addr.addr_bytes[2], port1->mac_addr.addr_bytes[3],
               port1->mac_addr.addr_bytes[4], port1->mac_addr.addr_bytes[5]);

        // Second port (if exists)
        if (i + 1 < config->nb_ports)
        {
            const struct port *port2 = &config->ports[i + 1];
            printf("  Port %u: %s (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
                   port2->port_id, port2->pci_addr,
                   port2->mac_addr.addr_bytes[0], port2->mac_addr.addr_bytes[1],
                   port2->mac_addr.addr_bytes[2], port2->mac_addr.addr_bytes[3],
                   port2->mac_addr.addr_bytes[4], port2->mac_addr.addr_bytes[5]);
        }
        printf("\n");
    }
}

int configure_port(uint16_t port_id)
{
    struct rte_eth_conf port_conf = {0};
    int ret;

    // Basic port configuration
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    // Configure the port
    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0)
    {
        printf("Error: Cannot configure port %u\n", port_id);
        return ret;
    }

    printf("Port %u configured successfully\n", port_id);
    return 0;
}

void set_manual_pci_addresses(struct ports_config *config)
{
    // Manual PCI address mapping for your 6 NICs (12 ports)
    const char *manual_pci_map[] = {
        "0000:21:00.0", // Port 0 - NIC 1, Port 0
        "0000:21:00.1", // Port 1 - NIC 1, Port 1
        "0000:41:00.0", // Port 2 - NIC 2, Port 0
        "0000:41:00.1", // Port 3 - NIC 2, Port 1
        "0000:64:00.0", // Port 4 - NIC 3, Port 0
        "0000:64:00.1", // Port 5 - NIC 3, Port 1
        "0000:81:00.0", // Port 6 - NIC 4, Port 0
        "0000:81:00.1", // Port 7 - NIC 4, Port 1
        "0000:a1:00.0", // Port 8 - NIC 5, Port 0
        "0000:a1:00.1", // Port 9 - NIC 5, Port 1
        "0000:e1:00.0", // Port 10 - NIC 6, Port 0
        "0000:e1:00.1", // Port 11 - NIC 6, Port 1
    };

    if (!config)
    {
        return;
    }

    printf("Applying manual PCI address mapping...\n");

    size_t map_size = sizeof(manual_pci_map) / sizeof(manual_pci_map[0]);
    for (uint16_t i = 0; i < config->nb_ports && i < map_size; i++)
    {
        strcpy(config->ports[i].pci_addr, manual_pci_map[i]);
        printf("Port %u -> %s\n", config->ports[i].port_id, config->ports[i].pci_addr);
    }
}

void portNumaNodesMatch(struct ports_config *config)
{
    for (uint16_t port = 0; port < config->nb_ports; port++)
    {
        config->ports[port].numa_node = rte_eth_dev_socket_id(port);
    }
}

void lcorePortAssign(struct ports_config *config)
{
    for (uint16_t port = 0; port < config->nb_ports; port++)
    {
        uint16_t cores = MAX_LCORE - 1;
        uint16_t *lcore_list = socket_to_lcore[config->ports[port].numa_node];
        uint16_t *unused_lcore_list = unused_socket_to_lcore[config->ports[port].numa_node];
        
        // Assign TX cores
        for (uint16_t tx_core = 0; tx_core < NUM_TX_CORES; tx_core++)
        {
            if (unused_lcore_list[cores] != 0)
            {
                uint16_t lcore = lcore_list[cores];
                unused_lcore_list[cores] = 0;
                config->ports[port].used_tx_cores[tx_core] = lcore;
                cores--;
            }
            else
            {
                while (unused_lcore_list[cores] == 0 && cores > 0)
                {
                    cores--;
                }
                uint16_t lcore = lcore_list[cores];
                unused_lcore_list[cores] = 0;
                config->ports[port].used_tx_cores[tx_core] = lcore;
                cores--;
            }
        }

        // Assign RX cores
        for (uint16_t rx_core = 0; rx_core < NUM_RX_CORES; rx_core++)
        {
            if (unused_lcore_list[cores] != 0)
            {
                uint16_t lcore = lcore_list[cores];
                unused_lcore_list[cores] = 0;
                config->ports[port].used_rx_cores[rx_core] = lcore;
                cores--;
            }
            else
            {
                while (unused_lcore_list[cores] == 0 && cores > 0)
                {
                    cores--;
                }
                uint16_t lcore = lcore_list[cores];
                unused_lcore_list[cores] = 0;
                config->ports[port].used_rx_cores[rx_core] = lcore;
                cores--;
            }
        }

#if DPDK_EXT_TX_ENABLED
        // Assign dedicated External TX core for external TX ports
        // Port 2,3,4,5 → Port 12 | Port 0,6 → Port 13
        config->ports[port].used_ext_tx_core = 0; // Default: not assigned

        // Check if this port is an external TX port
        bool is_ext_tx_port = (port == 0 || port == 2 || port == 3 ||
                               port == 4 || port == 5 || port == 6);

        if (is_ext_tx_port)
        {
            if (unused_lcore_list[cores] != 0)
            {
                uint16_t lcore = lcore_list[cores];
                unused_lcore_list[cores] = 0;
                config->ports[port].used_ext_tx_core = lcore;
                cores--;
            }
            else
            {
                while (unused_lcore_list[cores] == 0 && cores > 0)
                {
                    cores--;
                }
                if (cores > 0)
                {
                    uint16_t lcore = lcore_list[cores];
                    unused_lcore_list[cores] = 0;
                    config->ports[port].used_ext_tx_core = lcore;
                    cores--;
                }
            }
        }
#endif

#if PTP_ENABLED
        // Assign dedicated PTP core for each port
        config->ports[port].used_ptp_core = 0; // Default: not assigned

        if (unused_lcore_list[cores] != 0)
        {
            uint16_t lcore = lcore_list[cores];
            unused_lcore_list[cores] = 0;
            config->ports[port].used_ptp_core = lcore;
            cores--;
        }
        else
        {
            while (unused_lcore_list[cores] == 0 && cores > 0)
            {
                cores--;
            }
            if (cores > 0)
            {
                uint16_t lcore = lcore_list[cores];
                unused_lcore_list[cores] = 0;
                config->ports[port].used_ptp_core = lcore;
                cores--;
            }
        }
#endif
    }
}

void cleanup_ports(struct ports_config *config)
{
    if (!config)
    {
        return;
    }

    printf("Cleaning up ports...\n");

    for (uint16_t i = 0; i < config->nb_ports; i++)
    {
        if (config->ports[i].is_valid)
        {
            rte_eth_dev_stop(config->ports[i].port_id);
            rte_eth_dev_close(config->ports[i].port_id);
        }
    }

    memset(config, 0, sizeof(struct ports_config));
    printf("Ports cleanup completed\n");
}