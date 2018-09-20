/* STELLARIS Ethernet Controller
 *
 * Copyright (c) 2018 Zilogic Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "dev/eth_stellaris"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_ETHERNET_LEVEL

#include <logging/sys_log.h>
#include <net/ethernet.h>
#include <net/net_pkt.h>
#include <net/net_if.h>
#include <device.h>
#include <soc.h>
#include <ethernet/eth_stats.h>
#include "eth_stellaris_priv.h"

static void eth_stellaris_assign_mac(struct device *dev)
{
	u32_t value = 0x0;

	value |= 0xB4;
	value |= 0xEE << 8;
	value |= 0xD4 << 16;
	value |= CONFIG_ETH_MAC_ADDR_3 << 24;
	sys_write32(value, REG_MACIA0);

	value = 0x0;
	value |= CONFIG_ETH_MAC_ADDR_4;
	value |= CONFIG_ETH_MAC_ADDR_5 << 8;
	sys_write32(value, REG_MACIA1);
}

static void eth_stellaris_dev_init(struct device *dev)
{
	u32_t value;

	ARG_UNUSED(dev);

	/* 1. Assign MAC address to Hardware */
	eth_stellaris_assign_mac(dev);

	/* 2. Program MCRCTL to clear RXFIFO */
	value = BIT_MACRCTL_RSTFIFO;
	sys_write32(value, REG_MACRCTL);

	/* 3. Enable transmitter */
	value = BIT_MACTCTL_DUPLEX | BIT_MACTCTL_CRC |
		BIT_MACTCTL_PADEN | BIT_MACTCTL_TXEN;
	sys_write32(value, REG_MACTCTL);

	/* 4. Enable Receiver */
	value = BIT_MACRCTL_BADCRC | BIT_MACRCTL_RXEN;
	sys_write32(value, REG_MACRCTL);
}

static void eth_stellaris_flush(struct device *dev)
{
	struct eth_stellaris_runtime *dev_data = DEV_DATA(dev);

	if (dev_data->tx_pos != 0) {
		sys_write32(dev_data->tx_word, REG_MACDATA);
		dev_data->tx_pos = 0;
		dev_data->tx_word = 0;
	}
}

static void eth_stellaris_send_byte(struct device *dev, u8_t byte)
{
	struct eth_stellaris_runtime *dev_data = DEV_DATA(dev);

	dev_data->tx_word |= byte << (dev_data->tx_pos * 8);
	dev_data->tx_pos++;
	if (dev_data->tx_pos == 4) {
		sys_write32(dev_data->tx_word, REG_MACDATA);
		dev_data->tx_pos = 0;
		dev_data->tx_word = 0;
	}
}

static int eth_stellaris_send(struct net_if *iface, struct net_pkt *pkt)
{
	struct net_buf *frag;
	u16_t head_len_left, i, data_len;
	u8_t *eth_hdr;
	struct net_eth_hdr *pkt_hdr;
	struct device *dev = net_if_get_device(iface);
	struct eth_stellaris_runtime *dev_data = DEV_DATA(dev);

	if (!pkt->frags) {
		SYS_LOG_ERR("No data to send");
		net_pkt_unref(pkt);
		return -ENODATA;
	}

	/* Frame transmission
	 *  First two bytes are data_len for frame,
	 *  Initially send the data_len
	 */
	data_len = net_pkt_get_len(pkt);
	eth_stellaris_send_byte(dev, data_len & 0xff);
	eth_stellaris_send_byte(dev, (data_len & 0xff00) >> 8);

	/* Send the  header, header is 14 bytes */
	head_len_left = net_pkt_ll_reserve(pkt);
	eth_hdr = net_pkt_ll(pkt);
	for (i = 0; i < head_len_left; ++i) {
		eth_stellaris_send_byte(dev, eth_hdr[i]);
	}

	/* Send the payload */
	for (frag = pkt->frags; frag; frag = frag->frags) {
		for (i = 0; i < frag->len; ++i) {
			eth_stellaris_send_byte(dev, frag->data[i]);
		}
	}
	/* will transmit the partial word */
	eth_stellaris_flush(dev);

	/* Enable Txn */
	sys_write32(BIT_MACTR_NEWTX, REG_MACTR);

	/* Wait and check if txn successfull or not */
	k_sem_take(&dev_data->tx_sem, K_FOREVER);

	if (dev_data->tx_err) {
		SYS_LOG_ERR("Txn failed");
		dev_data->tx_err = false;
		net_pkt_unref(pkt);
		return -EIO;
	}

	/* Update statistics counters */
	eth_stats_update_bytes_tx(iface, net_pkt_get_len(pkt));
	eth_stats_update_pkts_tx(iface);
	pkt_hdr = NET_ETH_HDR(pkt);
	if (net_eth_is_addr_multicast(&pkt_hdr->dst)) {
		eth_stats_update_multicast_tx(iface);
	} else if (net_eth_is_addr_broadcast(&pkt_hdr->dst)) {
		eth_stats_update_broadcast_tx(iface);
	}

	SYS_LOG_DBG("pkt send %p len %d", pkt, net_pkt_get_len(pkt));
	net_pkt_unref(pkt);

	return 0;
}

void eth_stellaris_rx_error(struct net_if *iface)
{
	u32_t val;

	struct device *dev = net_if_get_device(iface);

	eth_stats_update_errors_rx(iface);
	/* Clear the rx_frame buffer,
	 * otherwise it could lead to underflow errors
	 */
	sys_write32(0x0, REG_MACRCTL);
	sys_write32(BIT_MACRCTL_RSTFIFO, REG_MACRCTL);
	val = BIT_MACRCTL_BADCRC | BIT_MACRCTL_RXEN;
	sys_write32(val, REG_MACRCTL);
}

static int eth_stellaris_rx_pkt(struct device *dev, struct net_pkt *pkt)
{
	u32_t reg_val;
	int frame_len, bytes_left;

	/*Read the First Word */
	reg_val = sys_read32(REG_MACDATA);
	frame_len = reg_val & 0x0000ffff;
	if (!net_pkt_append(pkt, 2, (u8_t *)&reg_val + 2, K_NO_WAIT)) {
		return -1;
	}

	/* A word have been read already, Thus minus 4 bytes to be read */
	bytes_left = frame_len - 4;

	/* Read the rest of words, minus the partial word and FCS byte */
	for (; bytes_left > 7; bytes_left -= 4) {
		reg_val = sys_read32(REG_MACDATA);
		if (!net_pkt_append(pkt, 4, (u8_t *)&reg_val, K_NO_WAIT)) {
			return -1;
		}
	}

	/* Handle the last partial word and Discard the 4 Byte FCS */
	while (bytes_left > 0) {
		reg_val = sys_read32(REG_MACDATA);
		/* Discard the last FCS word */
		if (bytes_left <= 4) {
			bytes_left = 0;
			break;
		}
		/* Read the the partial word */
		if (!net_pkt_append(pkt, bytes_left - 4,
				    (u8_t *)&reg_val, K_NO_WAIT)) {
			return -1;
		}
		bytes_left -= 4;
	}
	return frame_len;
}

static void eth_stellaris_rx(struct device *dev)
{
	struct net_pkt *pkt = NULL;
	struct eth_stellaris_runtime *dev_data = DEV_DATA(dev);
	struct net_if *iface = dev_data->iface;
	struct net_eth_hdr *pkt_hdr;
	int frame_len, ret;

	/* Obtain the packet to be populated */
	pkt = net_pkt_get_reserve_rx(0, K_NO_WAIT);
	if (!pkt) {
		SYS_LOG_ERR("Could not allocate pkt");
		goto err_mem;
	}

	frame_len = eth_stellaris_rx_pkt(dev, pkt);
	if (frame_len == -1) {
		goto err_rx;
	}

	ret = net_recv_data(iface, pkt);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to place frame in RX Queue");
		goto pkt_unref;
	}

	/* Update statistics counters */
	eth_stats_update_bytes_rx(iface, frame_len - 6);
	eth_stats_update_pkts_rx(iface);
	pkt_hdr = NET_ETH_HDR(pkt);
	if (net_eth_is_addr_broadcast(&pkt_hdr->dst)) {
		eth_stats_update_broadcast_rx(iface);
	} else if (net_eth_is_addr_multicast(&pkt_hdr->dst)) {
		eth_stats_update_multicast_rx(iface);
	}
	return;

err_rx:
	SYS_LOG_ERR("Failed to append data to buffer");

pkt_unref:
	net_pkt_unref(pkt);

err_mem:
	eth_stellaris_rx_error(iface);
}

static void rx_isr(void *arg)
{
	/* Read the interrupt status */
	struct device *dev = (struct device *)arg;
	struct eth_stellaris_runtime *dev_data = DEV_DATA(dev);
	int isr_val = sys_read32(REG_MACRIS);
	u32_t lock;

	lock = irq_lock();

	/* Acknowledge the interrupt */
	sys_write32(isr_val, REG_MACRIS);

	if (isr_val & BIT_MACRIS_RXINT) {
		eth_stellaris_rx(dev);
	}

	if (isr_val & BIT_MACRIS_TXEMP) {
		dev_data->tx_err = false;
		k_sem_give(&dev_data->tx_sem);
	}

	if (isr_val & BIT_MACRIS_TXER) {
		SYS_LOG_ERR("Txn Frame Error");
		eth_stats_update_errors_tx(dev_data->iface);
		dev_data->tx_err = true;
		k_sem_give(&dev_data->tx_sem);
	}

	if (isr_val & BIT_MACRIS_RXER) {
		SYS_LOG_ERR("Error Frame Recieved");
		eth_stellaris_rx_error(dev_data->iface);
	}

	irq_unlock(lock);
}

static void eth_stellaris_init(struct net_if *iface)
{
	struct device *dev;
	struct eth_stellaris_config *dev_conf;
	struct eth_stellaris_runtime *dev_data;

	dev = net_if_get_device(iface);
	dev_data = DEV_DATA(dev);
	dev_data->iface = iface;
	dev_conf = DEV_CFG(dev);

	/*1. Assign link local address */
	net_if_set_link_addr(iface,
			     dev_data->mac_addr, 6, NET_LINK_ETHERNET);

	ethernet_init(iface);

	/* Initialize semaphore */
	k_sem_init(&dev_data->tx_sem, 0, 1);

	/* Initialize Interrupts */
	dev_conf->config_func(dev);
}

static struct net_stats_eth *eth_stellaris_stats(struct device *dev)
{
	return &(DEV_DATA(dev)->stats);
}

static void eth_stellaris_irq_config(struct device *dev);

struct eth_stellaris_config eth_cfg = {
	.mac_base = CONFIG_ETH_BASE_ADDR,
	.config_func = eth_stellaris_irq_config,
};

struct eth_stellaris_runtime eth_data = {
	.mac_addr = {
		0xB4,
		0xEE,
		0xD4,
		(u8_t)CONFIG_ETH_MAC_ADDR_3,
		(u8_t)CONFIG_ETH_MAC_ADDR_4,
		(u8_t)CONFIG_ETH_MAC_ADDR_5
	},
	.tx_err = false,
	.tx_word = 0,
	.tx_pos = 0,
};

static const struct ethernet_api eth_stellaris_apis = {
	.iface_api.init	= eth_stellaris_init,
	.iface_api.send = eth_stellaris_send,
	.get_stats = eth_stellaris_stats,
};

NET_DEVICE_INIT(eth_stellaris, CONFIG_ETH_DRV_NAME,
		eth_stellaris_dev_init, &eth_data, &eth_cfg,
		CONFIG_ETH_INIT_PRIORITY,
		&eth_stellaris_apis, ETHERNET_L2,
		NET_L2_GET_CTX_TYPE(ETHERNET_L2), ETH_MTU);


static void eth_stellaris_irq_config(struct device *dev)
{
	ARG_UNUSED(dev);
	/* Enable Interrupt */
	IRQ_CONNECT(CONFIG_ETH_IRQ,
		    CONFIG_ETH_IRQ_PRIO,
		    rx_isr, DEVICE_GET(eth_stellaris), 0);
	irq_enable(CONFIG_ETH_IRQ);
}
