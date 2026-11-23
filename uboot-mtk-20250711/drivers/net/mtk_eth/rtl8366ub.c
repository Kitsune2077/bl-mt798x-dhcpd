// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8366UB / RTL8371C switch driver for U-Boot
 *
 * Board wiring assumed here (GL.iNet GL-MT5000):
 *   GMAC0 <-> switch CPU port 17 (physical 7), 2500BASE-X
 *   lan1/lan2 <-> internal UTP PHYs on ports 0/1 (1G/2.5G)
 *
 * U-Boot has no DSA framework, so instead of mirroring the Linux driver's
 * bridge/VLAN offload this only reprograms the switch back into a flat
 * "everything in VLAN 1, untagged" state after reset, which lets the
 * bootloader network stack (DHCP/web failsafe) reach the LAN ports. Linux
 * reconfigures the switch from scratch when it boots.
 *
 * Register access is Realtek SMI over the SoC MDIO controller
 * (C22: 4 writes + 1 read), matching rtl8366ub_reg.c from the Linux driver.
 */

#include <phy.h>
#include <miiphy.h>
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/mii.h>
#include "mtk_eth.h"

/* MDIO address of the switch (kernel DT: switch@0 under &mdio) */
#define RTL8366UB_MDIO_ADDR			0

/* SMI registers (MDIO-side interface of the switch) */
#define RTL8366UB_MDIO_CTRL0_REG		31
#define RTL8366UB_MDIO_CTRL1_REG		21
#define RTL8366UB_MDIO_ADDRESS_REG		23
#define RTL8366UB_MDIO_DATA_WRITE_REG		24
#define RTL8366UB_MDIO_DATA_READ_REG		25
#define RTL8366UB_MDIO_ADDR_OP			0x000e
#define RTL8366UB_MDIO_READ_OP			0x0001
#define RTL8366UB_MDIO_WRITE_OP			0x0003

/* Chip identification */
#define RTL8366UB_CHIP_ID_REG			0x0004
#define RTL8366UB_CHIP_NAME_REG			0x0006
#define RTL8366UB_CHIP_ID			0x8366
#define RTL8366UB_CHIP_NAME_CHAR1_SHIFT		9
#define RTL8366UB_CHIP_NAME_CHAR2_SHIFT		4
#define RTL8366UB_CHIP_NAME_CHAR_MASK		0x1f
#define RTL8366UB_CHIP_CHAR(c)			((c) + 1 - 'A')

/* Ports */
#define RTL8366UB_NUM_PORTS			4
#define RTL8366UB_MAX_LOGICAL_PORT		18
#define RTL8366UB_CPU_LOGICAL_PORT		17
#define RTL8366UB_CPU_PHYSICAL_PORT		7
#define RTL8366UB_VALID_PHYSICAL_MASK		0xef

/* VLAN / forwarding */
#define RTL8366UB_TABLE_CMD_REG			0x39e0
#define RTL8366UB_TABLE_ADDR_REG		0x39e2
#define RTL8366UB_TABLE_READ_DATA_REG		0x39e8
#define RTL8366UB_TABLE_WRITE_DATA_REG		0x39fc
#define RTL8366UB_TABLE_TYPE_MASK		GENMASK(2, 0)
#define RTL8366UB_TABLE_WRITE			BIT(3)
#define RTL8366UB_TABLE_METHOD_MASK		GENMASK(6, 4)
#define RTL8366UB_TABLE_EXECUTE			BIT(7)
#define RTL8366UB_TABLE_VLAN			3
#define RTL8366UB_TABLE_TIMEOUT			100

#define RTL8366UB_VLAN_CTRL_REG			0x3080
#define RTL8366UB_VLAN_FILTERING		BIT(0)
#define RTL8366UB_VLAN_INGRESS_REG		0x3082
#define RTL8366UB_VLAN_EGRESS_MODE_BASE		0x1fbe
#define RTL8366UB_VLAN_EGRESS_MODE_STRIDE	0x80
#define RTL8366UB_VLAN_EGRESS_MODE_MASK		GENMASK(1, 0)
#define RTL8366UB_VLAN_EGRESS_ORIGINAL		0
#define RTL8366UB_VLAN_EGRESS_KEEP_BASE		0x2ac2
#define RTL8366UB_VLAN_EGRESS_TRANS_BASE	0x3140
#define RTL8366UB_VLAN_EGRESS_PORT_MASK		GENMASK(7, 0)
#define RTL8366UB_VLAN_PVID_BASE		0x3084
#define RTL8366UB_VLAN_PVID_MASK		GENMASK(11, 0)
#define RTL8366UB_VLAN_ACCEPT_REG		0x3094
#define RTL8366UB_VLAN_ACCEPT_MASK		GENMASK(1, 0)
#define RTL8366UB_VLAN_ACCEPT_ALL		0
#define RTL8366UB_IGMP_CTRL_REG			0x35a0
#define RTL8366UB_IGMP_MLD_VLAN_LEAKY		BIT(14)
#define RTL8366UB_IPMC_VLAN_LEAKY_REG		0x31b4

/* L2 */
#define RTL8366UB_L2_LUT_CONFIG_REG		0x3698
#define RTL8366UB_L2_BCAM_DISABLE		BIT(6)
#define RTL8366UB_L2_IPMC_HASH			BIT(4)
#define RTL8366UB_L2_AGE_TIMER_REG		0x369c
#define RTL8366UB_L2_AGE_TIME			300
#define RTL8366UB_L2_LEARN_LIMIT_BASE		0x3674
#define RTL8366UB_L2_LEARN_LIMIT_MASK		GENMASK(11, 0)
#define RTL8366UB_L2_LEARN_LIMIT_MAX		2056

/* CPU port (SDS1 / HS-GMII) */
#define RTL8366UB_SDS_MODE_CTRL_REG		0x00b0
#define RTL8366UB_SDS1_MODE_MASK		GENMASK(12, 8)
#define RTL8366UB_SDS_MODE_DISABLE		0x1f
#define RTL8366UB_SDS_MODE_HSGMII		0x12
#define RTL8366UB_SDS_MISC_2_REG		0x01da
#define RTL8366UB_SDS_USX_SUBMODE_MASK		GENMASK(11, 7)
#define RTL8366UB_SDS_INDACS_CMD_REG		0x6600
#define RTL8366UB_SDS_INDACS_ADDR_REG		0x6602
#define RTL8366UB_SDS_INDACS_DATA_REG		0x6604
#define RTL8366UB_SDS_INDACS_BUSY		BIT(8)
#define RTL8366UB_SDS1_READ_CMD			0x0081
#define RTL8366UB_SDS1_WRITE_CMD		0x00c1
#define RTL8366UB_SDS_TIMEOUT			100
#define RTL8366UB_SDS1_2500BX_NWAY_REG		0x0040
#define RTL8366UB_SDS1_2500BX_NWAY		BIT(12)
#define RTL8366UB_SDS1_HSGMII_NWAY_REG		0x0001
#define RTL8366UB_SDS1_HSGMII_NWAY_MASK		GENMASK(11, 8)
#define RTL8366UB_SDS1_TGR_NWAY_REG		0x03eb
#define RTL8366UB_SDS1_TGR_NWAY			BIT(4)
#define RTL8366UB_SDS1_USXGMII_NWAY_REG		0x00f1
#define RTL8366UB_SDS1_USXGMII_NWAY_MASK	GENMASK(3, 0)

#define RTL8366UB_MAC_FORCE_BASE		0x283c
#define RTL8366UB_MAC_FORCE_PORT_STRIDE		2
#define RTL8366UB_MAC_FORCE_VALUE		0x03af

/* RTL8_4 CPU tag */
#define RTL8366UB_CPU_TAG_AWARE_REG		0x2642
#define RTL8366UB_CPU_TAG_CTRL_REG		0x2644
#define RTL8366UB_CPU_TAG_ENABLE		BIT(0)

/* PHY / OCP indirect access */
#define RTL8366UB_OCP_MSB_REG			0x0028
#define RTL8366UB_OCP_MSB_MASK			GENMASK(11, 6)
#define RTL8366UB_INDACC_CTRL_REG		0x1f00
#define RTL8366UB_INDACC_STATUS_REG		0x1f02
#define RTL8366UB_INDACC_ADDR_REG		0x1f04
#define RTL8366UB_INDACC_WDATA_REG		0x1f06
#define RTL8366UB_INDACC_RDATA_REG		0x1f08
#define RTL8366UB_INDACC_CMD			BIT(0)
#define RTL8366UB_INDACC_WRITE			BIT(1)
#define RTL8366UB_INDACC_BUSY			BIT(2)
#define RTL8366UB_INDACC_PHY			BIT(13)
#define RTL8366UB_INDACC_TIMEOUT		100
#define RTL8366UB_PHY_ENABLE_REG		0xa610
#define RTL8366UB_PHY_ENABLED			0x2058
#define RTL8366UB_PHY_DISABLED			0x2858

/* Vendor PHY patch plumbing */
#define RTL8366UB_TX_POLARITY_REG		0x06fc
#define RTL8366UB_PATCH_STATE_REG		0xa46a
#define RTL8366UB_PATCH_STATE_MASK		GENMASK(10, 8)
#define RTL8366UB_PATCH_STATE_3			3
#define RTL8366UB_PATCH_STATE_5			5
#define RTL8366UB_PATCH_REQUEST_REG		0xb820
#define RTL8366UB_PATCH_REQUEST_ENABLE		0x0010
#define RTL8366UB_PATCH_READY_REG		0xb800
#define RTL8366UB_PATCH_READY			BIT(6)
#define RTL8366UB_PATCH_POLL_RETRIES		200

struct rtl8366ub_port_info {
	u8 logical_port;
	u8 physical_port;
	bool supports_2p5g;
};

static const struct rtl8366ub_port_info rtl8366ub_ports[] = {
	{ 0, 0, true },
	{ 1, 1, true },
	{ 2, 2, false },
	{ 3, 3, false },
	{ 16, 6, false },
	{ 17, 7, false },
	{ 18, 5, false },
};

struct rtl8366ub_switch_priv {
	struct mtk_eth_switch_priv epriv;
};

struct rtl8366ub_init_patch {
	u16 reg;
	u16 val;
};

static const struct rtl8366ub_init_patch rtl8366ub_afe_patch[] = {
	{ 0xbcc2, 0x0001 },
};

static const struct rtl8366ub_init_patch rtl8366ub_giga_phy_patch[] = {
	{ 0xa436, 0x8048 }, { 0xa438, 0x1903 }, { 0xa436, 0x804e }, { 0xa438, 0x1903 },
	{ 0xa436, 0x8054 }, { 0xa438, 0x1903 }, { 0xa436, 0x805a }, { 0xa438, 0x1903 },
	{ 0xa436, 0x8060 }, { 0xa438, 0x1903 }, { 0xa436, 0x8066 }, { 0xa438, 0x1903 },
	{ 0xa436, 0x806c }, { 0xa438, 0x1903 },
};

static const struct rtl8366ub_init_patch rtl8366ub_dataram_patch[] = {
	{ 0xb88e, 0xc201 }, { 0xb890, 0x8e01 }, { 0xb88e, 0xc203 }, { 0xb890, 0x2001 },
	{ 0xb88e, 0xc205 }, { 0xb890, 0x5902 }, { 0xb88e, 0xc207 }, { 0xb890, 0x2802 },
	{ 0xb88e, 0xc209 }, { 0xb890, 0xd803 }, { 0xb88e, 0xc20b }, { 0xb890, 0xa504 },
	{ 0xb88e, 0xc20d }, { 0xb890, 0x1004 }, { 0xb88e, 0xc20f }, { 0xb890, 0xc005 },
	{ 0xb88e, 0xc211 }, { 0xb890, 0x7506 }, { 0xb88e, 0xc213 }, { 0xb890, 0x6f07 },
	{ 0xb88e, 0xc215 }, { 0xb890, 0x9208 }, { 0xb88e, 0xc217 }, { 0xb890, 0xe00a },
	{ 0xb88e, 0xc219 }, { 0xb890, 0x900c }, { 0xb88e, 0xc21b }, { 0xb890, 0x000d },
	{ 0xb88e, 0xc21d }, { 0xb890, 0xbe10 }, { 0xb88e, 0xc21f }, { 0xb890, 0x52f1 },
	{ 0xb88e, 0xc221 }, { 0xb890, 0xf1ef }, { 0xb88e, 0xc223 }, { 0xb890, 0xebea },
	{ 0xb88e, 0xc225 }, { 0xb890, 0xeae6 }, { 0xb88e, 0xc227 }, { 0xb890, 0xe3e2 },
	{ 0xb88e, 0xc229 }, { 0xb890, 0xe2df }, { 0xb88e, 0xc22b }, { 0xb890, 0xdddc },
	{ 0xb88e, 0xc22d }, { 0xb890, 0xd9d8 }, { 0xb88e, 0xc22f }, { 0xb890, 0xd800 },
	{ 0xb88e, 0xc23f }, { 0xb890, 0x0003 }, { 0xb88e, 0xc241 }, { 0xb890, 0x0304 },
	{ 0xb88e, 0xc243 }, { 0xb890, 0x0507 }, { 0xb88e, 0xc245 }, { 0xb890, 0x0b0c },
	{ 0xb88e, 0xc247 }, { 0xb890, 0x0e10 }, { 0xb88e, 0xc249 }, { 0xb890, 0x1213 },
	{ 0xb88e, 0xc24b }, { 0xb890, 0x1516 }, { 0xb88e, 0xc24d }, { 0xb890, 0x1717 },
	{ 0xb88e, 0xc24f }, { 0xb890, 0x180d }, { 0xb88e, 0xc25f }, { 0xb890, 0x0d0e },
	{ 0xb88e, 0xc261 }, { 0xb890, 0x0e10 }, { 0xb88e, 0xc263 }, { 0xb890, 0x1517 },
	{ 0xb88e, 0xc265 }, { 0xb890, 0x1820 }, { 0xb88e, 0xc267 }, { 0xb890, 0x232b },
	{ 0xb88e, 0xc269 }, { 0xb890, 0x3142 }, { 0xb88e, 0xc26b }, { 0xb890, 0x444f },
	{ 0xb88e, 0xc26d }, { 0xb890, 0x6d75 }, { 0xb88e, 0xc26f }, { 0xb890, 0x7517 },
	{ 0xb88e, 0xc271 }, { 0xb890, 0x1719 }, { 0xb88e, 0xc273 }, { 0xb890, 0x1a1d },
	{ 0xb88e, 0xc275 }, { 0xb890, 0x1f21 }, { 0xb88e, 0xc277 }, { 0xb890, 0x2223 },
	{ 0xb88e, 0xc279 }, { 0xb890, 0x2525 }, { 0xb88e, 0xc27b }, { 0xb890, 0x2526 },
	{ 0xb88e, 0xc27d }, { 0xb890, 0x2626 }, { 0xb88e, 0xc27f }, { 0xb890, 0x274a },
	{ 0xb88e, 0xc281 }, { 0xb890, 0x4a4d }, { 0xb88e, 0xc283 }, { 0xb890, 0x4d4f },
	{ 0xb88e, 0xc285 }, { 0xb890, 0x5254 }, { 0xb88e, 0xc287 }, { 0xb890, 0x5456 },
	{ 0xb88e, 0xc289 }, { 0xb890, 0x5655 }, { 0xb88e, 0xc28b }, { 0xb890, 0x5554 },
	{ 0xb88e, 0xc28d }, { 0xb890, 0x5454 }, { 0xb88e, 0xc28f }, { 0xb890, 0x54df },
	{ 0xb87c, 0x81f3 }, { 0xb87e, 0x1f8f },
};

static const struct rtl8366ub_init_patch rtl8366ub_alg_giga_patch[] = {
	{ 0xa436, 0x80a8 }, { 0xa438, 0xc5c1 }, { 0xa436, 0x80be }, { 0xa438, 0xc6be },
	{ 0xa436, 0x8090 }, { 0xa438, 0xb008 }, { 0xa436, 0x8092 }, { 0xa438, 0x049c },
	{ 0xa436, 0x8094 }, { 0xa438, 0xf81f },
};

static const struct rtl8366ub_init_patch rtl8366ub_ncrp_patch[] = {
	{ 0xa436, 0xa016 }, { 0xa438, 0x0000 }, { 0xa436, 0xa012 }, { 0xa438, 0x0ff8 },
	{ 0xa436, 0xa014 }, { 0xa438, 0x8a00 }, { 0xa438, 0x0a07 }, { 0xa438, 0x0000 },
	{ 0xa438, 0x0000 }, { 0xa438, 0x0000 }, { 0xa438, 0x0000 }, { 0xa438, 0x0000 },
	{ 0xa438, 0x0000 }, { 0xa436, 0xa152 }, { 0xa438, 0x1344 }, { 0xa436, 0xa154 },
	{ 0xa438, 0x0f51 }, { 0xa436, 0xa156 }, { 0xa438, 0x3fff }, { 0xa436, 0xa158 },
	{ 0xa438, 0x3fff }, { 0xa436, 0xa15a }, { 0xa438, 0x3fff }, { 0xa436, 0xa15c },
	{ 0xa438, 0x3fff }, { 0xa436, 0xa15e }, { 0xa438, 0x3fff }, { 0xa436, 0xa160 },
	{ 0xa438, 0x3fff }, { 0xa436, 0xa150 }, { 0xa438, 0x0003 },
};

static const struct rtl8366ub_init_patch rtl8366ub_alg_2p5g_patch[] = {
	{ 0xb87c, 0x84f8 },
	{ 0xb87e, 0x031e },
};

static const struct rtl8366ub_port_info *rtl8366ub_get_port_info(int port)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rtl8366ub_ports); i++) {
		if (rtl8366ub_ports[i].logical_port == port)
			return &rtl8366ub_ports[i];
	}

	return NULL;
}

static int rtl8366ub_port_to_phys(int port)
{
	const struct rtl8366ub_port_info *info = rtl8366ub_get_port_info(port);

	return info ? info->physical_port : -EINVAL;
}

/* ------------------------------------------------------------------ */
/* SMI register access                                                 */
/* ------------------------------------------------------------------ */

static int rtl8366ub_reg_read(struct mtk_eth_priv *eth, u16 reg, u16 *val)
{
	int ret;

	ret = mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_CTRL0_REG,
			    RTL8366UB_MDIO_ADDR_OP);
	if (ret)
		return ret;

	ret = mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_ADDRESS_REG, reg);
	if (ret)
		return ret;

	ret = mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_CTRL1_REG,
			    RTL8366UB_MDIO_READ_OP);
	if (ret)
		return ret;

	ret = mtk_mii_read(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_DATA_READ_REG);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int rtl8366ub_reg_write(struct mtk_eth_priv *eth, u16 reg, u16 val)
{
	int ret;

	ret = mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_CTRL0_REG,
			    RTL8366UB_MDIO_ADDR_OP);
	if (ret)
		return ret;

	ret = mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_ADDRESS_REG, reg);
	if (ret)
		return ret;

	ret = mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_DATA_WRITE_REG, val);
	if (ret)
		return ret;

	return mtk_mii_write(eth, RTL8366UB_MDIO_ADDR, RTL8366UB_MDIO_CTRL1_REG,
			     RTL8366UB_MDIO_WRITE_OP);
}

static int rtl8366ub_update_bits(struct mtk_eth_priv *eth, u16 reg, u16 mask, u16 val)
{
	u16 data;
	int ret;

	ret = rtl8366ub_reg_read(eth, reg, &data);
	if (ret)
		return ret;

	data = (data & ~mask) | (val & mask);

	return rtl8366ub_reg_write(eth, reg, data);
}

/* ------------------------------------------------------------------ */
/* PHY / OCP indirect access                                           */
/* ------------------------------------------------------------------ */

static int rtl8366ub_ocp_prepare(struct mtk_eth_priv *eth, int port, u16 addr)
{
	const struct rtl8366ub_port_info *info;
	u16 val;
	int ret;

	info = rtl8366ub_get_port_info(port);
	if (!info || port >= RTL8366UB_NUM_PORTS || (addr & 1))
		return -EINVAL;

	ret = rtl8366ub_reg_read(eth, RTL8366UB_OCP_MSB_REG, &val);
	if (ret)
		return ret;

	val &= ~RTL8366UB_OCP_MSB_MASK;
	val |= FIELD_PREP(RTL8366UB_OCP_MSB_MASK, addr >> 10);
	ret = rtl8366ub_reg_write(eth, RTL8366UB_OCP_MSB_REG, val);
	if (ret)
		return ret;

	val = RTL8366UB_INDACC_PHY | ((addr >> 6 & 0xf) << 8) |
	      (info->physical_port << 5) | (addr >> 1 & 0x1f);

	return rtl8366ub_reg_write(eth, RTL8366UB_INDACC_ADDR_REG, val);
}

static int rtl8366ub_ocp_wait(struct mtk_eth_priv *eth)
{
	u16 val;
	int ret;
	int i;

	for (i = 0; i < RTL8366UB_INDACC_TIMEOUT; i++) {
		ret = rtl8366ub_reg_read(eth, RTL8366UB_INDACC_STATUS_REG, &val);
		if (ret)
			return ret;
		if (!(val & RTL8366UB_INDACC_BUSY))
			return 0;
	}

	return -ETIMEDOUT;
}

static int rtl8366ub_ocp_read(struct mtk_eth_priv *eth, int port, u16 addr, u16 *val)
{
	int ret;

	ret = rtl8366ub_ocp_prepare(eth, port, addr);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_INDACC_CTRL_REG, RTL8366UB_INDACC_CMD);
	if (ret)
		return ret;

	ret = rtl8366ub_ocp_wait(eth);
	if (ret)
		return ret;

	return rtl8366ub_reg_read(eth, RTL8366UB_INDACC_RDATA_REG, val);
}

static int rtl8366ub_ocp_write(struct mtk_eth_priv *eth, int port, u16 addr, u16 val)
{
	int ret;

	ret = rtl8366ub_ocp_prepare(eth, port, addr);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_INDACC_WDATA_REG, val);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_INDACC_CTRL_REG,
				  RTL8366UB_INDACC_WRITE | RTL8366UB_INDACC_CMD);
	if (ret)
		return ret;

	return rtl8366ub_ocp_wait(eth);
}

static int rtl8366ub_port_set_enabled(struct mtk_eth_priv *eth, int port, bool enabled)
{
	return rtl8366ub_ocp_write(eth, port, RTL8366UB_PHY_ENABLE_REG,
				   enabled ? RTL8366UB_PHY_ENABLED :
					     RTL8366UB_PHY_DISABLED);
}

/* ------------------------------------------------------------------ */
/* Vendor PHY patch sequence (same as the Linux driver's hw_init)      */
/* ------------------------------------------------------------------ */

static int rtl8366ub_init_apply(struct mtk_eth_priv *eth, int port,
				const struct rtl8366ub_init_patch *patch,
				size_t count, const char *stage)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = rtl8366ub_ocp_write(eth, port, patch[i].reg, patch[i].val);
		if (ret) {
			printf("rtl8366ub: %s patch failed on port %d entry %zu\n",
			       stage, port, i);
			return ret;
		}
	}

	return 0;
}

static int rtl8366ub_init_patch_request(struct mtk_eth_priv *eth, bool enable)
{
	int port;

	for (port = 0; port < RTL8366UB_NUM_PORTS; port++) {
		u16 val;
		int ret;
		int i;

		if (enable) {
			ret = rtl8366ub_ocp_read(eth, port, RTL8366UB_PATCH_STATE_REG, &val);
			if (ret)
				return ret;
			val = FIELD_GET(RTL8366UB_PATCH_STATE_MASK, val);
			if (val != RTL8366UB_PATCH_STATE_3 && val != RTL8366UB_PATCH_STATE_5) {
				printf("rtl8366ub: unexpected PHY patch state %u on port %d\n",
				       val, port);
				return -EIO;
			}
		}

		ret = rtl8366ub_ocp_write(eth, port, RTL8366UB_PATCH_REQUEST_REG,
					  enable ? RTL8366UB_PATCH_REQUEST_ENABLE : 0);
		if (ret)
			return ret;
		if (!enable)
			continue;

		for (i = 0; i < RTL8366UB_PATCH_POLL_RETRIES; i++) {
			ret = rtl8366ub_ocp_read(eth, port, RTL8366UB_PATCH_READY_REG, &val);
			if (ret)
				return ret;
			if (val & RTL8366UB_PATCH_READY)
				break;
		}
		if (i == RTL8366UB_PATCH_POLL_RETRIES) {
			printf("rtl8366ub: PHY patch request timed out on port %d\n", port);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int rtl8366ub_init_selected_patch(struct mtk_eth_priv *eth, bool want_2p5g,
					 const struct rtl8366ub_init_patch *patch,
					 size_t count, const char *stage)
{
	int port;

	for (port = 0; port < RTL8366UB_NUM_PORTS; port++) {
		const struct rtl8366ub_port_info *info;
		int ret;

		info = rtl8366ub_get_port_info(port);
		if (!info || info->supports_2p5g != want_2p5g)
			continue;

		ret = rtl8366ub_init_apply(eth, port, patch, count, stage);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366ub_init_dataram(struct mtk_eth_priv *eth)
{
	int port;

	for (port = 0; port < RTL8366UB_NUM_PORTS; port++) {
		const struct rtl8366ub_port_info *info;
		u16 val;
		int ret;

		info = rtl8366ub_get_port_info(port);
		if (!info || !info->supports_2p5g)
			continue;

		ret = rtl8366ub_ocp_read(eth, port, 0xb820, &val);
		if (ret)
			return ret;
		ret = rtl8366ub_ocp_write(eth, port, 0xb820, val & ~BIT(7));
		if (ret)
			return ret;

		ret = rtl8366ub_ocp_read(eth, port, 0xb896, &val);
		if (ret)
			return ret;
		ret = rtl8366ub_ocp_write(eth, port, 0xb896, val & ~BIT(0));
		if (ret)
			return ret;

		ret = rtl8366ub_ocp_read(eth, port, 0xb892, &val);
		if (ret)
			return ret;
		ret = rtl8366ub_ocp_write(eth, port, 0xb892, val & ~GENMASK(15, 8));
		if (ret)
			return ret;

		ret = rtl8366ub_init_apply(eth, port, rtl8366ub_dataram_patch,
					   ARRAY_SIZE(rtl8366ub_dataram_patch),
					   "data RAM");
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366ub_init_alg_giga(struct mtk_eth_priv *eth)
{
	int port;

	for (port = 0; port < RTL8366UB_NUM_PORTS; port++) {
		const struct rtl8366ub_port_info *info;
		u16 val;
		int ret;

		info = rtl8366ub_get_port_info(port);
		if (!info || info->supports_2p5g)
			continue;

		ret = rtl8366ub_ocp_read(eth, port, 0xa87e, &val);
		if (ret)
			return ret;
		val = (val & ~GENMASK(12, 8)) | FIELD_PREP(GENMASK(12, 8), 0x10);
		ret = rtl8366ub_ocp_write(eth, port, 0xa87e, val);
		if (ret)
			return ret;

		ret = rtl8366ub_init_apply(eth, port, rtl8366ub_alg_giga_patch,
					   ARRAY_SIZE(rtl8366ub_alg_giga_patch),
					   "ALG Giga");
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366ub_init_ncrp(struct mtk_eth_priv *eth)
{
	int port;

	for (port = 0; port < RTL8366UB_NUM_PORTS; port++) {
		const struct rtl8366ub_port_info *info;
		u16 val;
		int ret;

		info = rtl8366ub_get_port_info(port);
		if (!info || !info->supports_2p5g)
			continue;

		ret = rtl8366ub_ocp_read(eth, port, 0xb820, &val);
		if (ret)
			return ret;
		ret = rtl8366ub_ocp_write(eth, port, 0xb820, val | BIT(7));
		if (ret)
			return ret;

		ret = rtl8366ub_init_apply(eth, port, rtl8366ub_ncrp_patch,
					   ARRAY_SIZE(rtl8366ub_ncrp_patch), "NCRP");
		if (ret)
			return ret;

		ret = rtl8366ub_ocp_read(eth, port, 0xb820, &val);
		if (ret)
			return ret;
		ret = rtl8366ub_ocp_write(eth, port, 0xb820, val & ~BIT(7));
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366ub_hw_init(struct mtk_eth_priv *eth)
{
	int ret;

	ret = rtl8366ub_init_patch_request(eth, true);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_TX_POLARITY_REG, 0x00a5);
	if (ret)
		return ret;

	ret = rtl8366ub_init_selected_patch(eth, true, rtl8366ub_afe_patch,
					    ARRAY_SIZE(rtl8366ub_afe_patch), "AFE");
	if (ret)
		return ret;

	ret = rtl8366ub_init_selected_patch(eth, false, rtl8366ub_giga_phy_patch,
					    ARRAY_SIZE(rtl8366ub_giga_phy_patch),
					    "Giga PHY");
	if (ret)
		return ret;

	ret = rtl8366ub_init_dataram(eth);
	if (ret)
		return ret;

	ret = rtl8366ub_init_alg_giga(eth);
	if (ret)
		return ret;

	ret = rtl8366ub_init_ncrp(eth);
	if (ret)
		return ret;

	ret = rtl8366ub_init_selected_patch(eth, true, rtl8366ub_alg_2p5g_patch,
					    ARRAY_SIZE(rtl8366ub_alg_2p5g_patch),
					    "ALG 2.5G");
	if (ret)
		return ret;

	return rtl8366ub_init_patch_request(eth, false);
}

/* ------------------------------------------------------------------ */
/* VLAN table                                                          */
/* ------------------------------------------------------------------ */

static int rtl8366ub_table_wait(struct mtk_eth_priv *eth)
{
	u16 cmd;
	int ret;
	int i;

	for (i = 0; i < RTL8366UB_TABLE_TIMEOUT; i++) {
		ret = rtl8366ub_reg_read(eth, RTL8366UB_TABLE_CMD_REG, &cmd);
		if (ret)
			return ret;
		if (!(cmd & RTL8366UB_TABLE_EXECUTE))
			return 0;
	}

	return -ETIMEDOUT;
}

static int rtl8366ub_table_xfer(struct mtk_eth_priv *eth, bool write, u16 addr,
				u16 *data, int words)
{
	u16 cmd;
	int ret;
	int i;

	if (addr > 0xfff || words < 1 || words > 10)
		return -EINVAL;

	ret = rtl8366ub_table_wait(eth);
	if (ret)
		return ret;

	if (write) {
		for (i = 0; i < words; i++) {
			ret = rtl8366ub_reg_write(eth, RTL8366UB_TABLE_WRITE_DATA_REG + i * 2,
						  data[i]);
			if (ret)
				return ret;
		}
	}

	ret = rtl8366ub_reg_write(eth, RTL8366UB_TABLE_ADDR_REG, addr);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_read(eth, RTL8366UB_TABLE_CMD_REG, &cmd);
	if (ret)
		return ret;

	cmd &= ~(RTL8366UB_TABLE_TYPE_MASK | RTL8366UB_TABLE_WRITE |
		 RTL8366UB_TABLE_METHOD_MASK | RTL8366UB_TABLE_EXECUTE);
	cmd |= RTL8366UB_TABLE_VLAN | RTL8366UB_TABLE_EXECUTE;
	if (write)
		cmd |= RTL8366UB_TABLE_WRITE;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_TABLE_CMD_REG, cmd);
	if (ret)
		return ret;

	ret = rtl8366ub_table_wait(eth);
	if (ret || write)
		return ret;

	for (i = 0; i < words; i++) {
		ret = rtl8366ub_reg_read(eth, RTL8366UB_TABLE_READ_DATA_REG + i * 2, &data[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366ub_vlan_set(struct mtk_eth_priv *eth, u16 vid, u8 member, u8 untag,
			      bool ivl)
{
	u16 data[2];

	if (vid > 4095 || (member & ~RTL8366UB_VALID_PHYSICAL_MASK) ||
	    (untag & ~member))
		return -EINVAL;

	data[0] = member | untag << 8;
	data[1] = ivl ? BIT(4) : 0;

	return rtl8366ub_table_xfer(eth, true, vid, data, ARRAY_SIZE(data));
}

static int rtl8366ub_vlan_set_pvid(struct mtk_eth_priv *eth, int port, u16 vid)
{
	int physical = rtl8366ub_port_to_phys(port);

	if (physical < 0 || vid > 4095)
		return -EINVAL;

	return rtl8366ub_update_bits(eth, RTL8366UB_VLAN_PVID_BASE + physical * 2,
				     RTL8366UB_VLAN_PVID_MASK, vid);
}

static int rtl8366ub_vlan_set_accept(struct mtk_eth_priv *eth, int port, u16 accept)
{
	int physical = rtl8366ub_port_to_phys(port);
	u16 shift;
	u16 mask;

	if (physical < 0)
		return -EINVAL;

	shift = physical * 2;
	mask = RTL8366UB_VLAN_ACCEPT_MASK << shift;

	return rtl8366ub_update_bits(eth, RTL8366UB_VLAN_ACCEPT_REG, mask, accept << shift);
}

static int rtl8366ub_vlan_set_egress_original(struct mtk_eth_priv *eth, int port)
{
	int physical = rtl8366ub_port_to_phys(port);

	if (physical < 0)
		return -EINVAL;

	return rtl8366ub_update_bits(eth, RTL8366UB_VLAN_EGRESS_MODE_BASE +
						 physical * RTL8366UB_VLAN_EGRESS_MODE_STRIDE,
				     RTL8366UB_VLAN_EGRESS_MODE_MASK,
				     RTL8366UB_VLAN_EGRESS_ORIGINAL);
}

static int rtl8366ub_vlan_clear_egress_overrides(struct mtk_eth_priv *eth, int port)
{
	int physical = rtl8366ub_port_to_phys(port);
	int ret;

	if (physical < 0)
		return -EINVAL;

	ret = rtl8366ub_update_bits(eth, RTL8366UB_VLAN_EGRESS_KEEP_BASE + physical * 2,
				    RTL8366UB_VLAN_EGRESS_PORT_MASK, 0);
	if (ret)
		return ret;

	return rtl8366ub_update_bits(eth, RTL8366UB_VLAN_EGRESS_TRANS_BASE + physical * 2,
				     RTL8366UB_VLAN_EGRESS_PORT_MASK, 0);
}

static int rtl8366ub_vlan_set_filtering(struct mtk_eth_priv *eth, bool enabled)
{
	int ret;

	ret = rtl8366ub_update_bits(eth, RTL8366UB_VLAN_CTRL_REG, RTL8366UB_VLAN_FILTERING,
				    enabled ? RTL8366UB_VLAN_FILTERING : 0);
	if (ret)
		return ret;

	return rtl8366ub_reg_write(eth, RTL8366UB_VLAN_INGRESS_REG,
				   enabled ? RTL8366UB_VALID_PHYSICAL_MASK : 0);
}

static int rtl8366ub_vlan_disable_leaky(struct mtk_eth_priv *eth)
{
	int ret;

	ret = rtl8366ub_update_bits(eth, RTL8366UB_IGMP_CTRL_REG,
				    RTL8366UB_IGMP_MLD_VLAN_LEAKY, 0);
	if (ret)
		return ret;

	return rtl8366ub_update_bits(eth, RTL8366UB_IPMC_VLAN_LEAKY_REG,
				     RTL8366UB_VALID_PHYSICAL_MASK, 0);
}

/* ------------------------------------------------------------------ */
/* L2 (learning / aging)                                              */
/* ------------------------------------------------------------------ */

static int rtl8366ub_l2_set_learning_limit(struct mtk_eth_priv *eth, int port, u16 limit)
{
	const struct rtl8366ub_port_info *info;

	if (limit > RTL8366UB_L2_LEARN_LIMIT_MAX)
		return -EINVAL;

	info = rtl8366ub_get_port_info(port);
	if (!info)
		return -EINVAL;

	return rtl8366ub_update_bits(eth, RTL8366UB_L2_LEARN_LIMIT_BASE +
						  info->physical_port * 2,
				     RTL8366UB_L2_LEARN_LIMIT_MASK, limit);
}

static int rtl8366ub_l2_setup(struct mtk_eth_priv *eth, int cpu_port)
{
	int port;
	int ret;

	ret = rtl8366ub_update_bits(eth, RTL8366UB_L2_LUT_CONFIG_REG,
				    RTL8366UB_L2_IPMC_HASH, 0);
	if (ret)
		return ret;

	ret = rtl8366ub_update_bits(eth, RTL8366UB_L2_LUT_CONFIG_REG,
				    RTL8366UB_L2_BCAM_DISABLE, 0);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_L2_AGE_TIMER_REG,
				  RTL8366UB_L2_AGE_TIME * 10);
	if (ret)
		return ret;

	for (port = 0; port <= RTL8366UB_MAX_LOGICAL_PORT; port++) {
		if (!rtl8366ub_get_port_info(port))
			continue;
		ret = rtl8366ub_l2_set_learning_limit(eth, port,
						      RTL8366UB_L2_LEARN_LIMIT_MAX);
		if (ret)
			return ret;
	}

	return rtl8366ub_l2_set_learning_limit(eth, cpu_port, 0);
}

/* ------------------------------------------------------------------ */
/* CPU port (SDS1 / HS-GMII) and CPU tag                              */
/* ------------------------------------------------------------------ */

static int rtl8366ub_sds1_set_mode(struct mtk_eth_priv *eth, u16 mode)
{
	return rtl8366ub_update_bits(eth, RTL8366UB_SDS_MODE_CTRL_REG,
				     RTL8366UB_SDS1_MODE_MASK,
				     FIELD_PREP(RTL8366UB_SDS1_MODE_MASK, mode));
}

static int rtl8366ub_sds_wait(struct mtk_eth_priv *eth)
{
	u16 val;
	int ret;
	unsigned int i;

	for (i = 0; i < RTL8366UB_SDS_TIMEOUT; i++) {
		ret = rtl8366ub_reg_read(eth, RTL8366UB_SDS_INDACS_CMD_REG, &val);
		if (ret)
			return ret;
		if (!(val & RTL8366UB_SDS_INDACS_BUSY))
			return 0;
	}

	return -ETIMEDOUT;
}

static int rtl8366ub_sds1_read(struct mtk_eth_priv *eth, u16 addr, u16 *val)
{
	int ret;

	ret = rtl8366ub_sds_wait(eth);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_SDS_INDACS_ADDR_REG, addr);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_SDS_INDACS_CMD_REG, RTL8366UB_SDS1_READ_CMD);
	if (ret)
		return ret;

	ret = rtl8366ub_sds_wait(eth);
	if (ret)
		return ret;

	return rtl8366ub_reg_read(eth, RTL8366UB_SDS_INDACS_DATA_REG, val);
}

static int rtl8366ub_sds1_write(struct mtk_eth_priv *eth, u16 addr, u16 val)
{
	int ret;

	ret = rtl8366ub_sds_wait(eth);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_SDS_INDACS_DATA_REG, val);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_SDS_INDACS_ADDR_REG, addr);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_SDS_INDACS_CMD_REG, RTL8366UB_SDS1_WRITE_CMD);
	if (ret)
		return ret;

	return rtl8366ub_sds_wait(eth);
}

static int rtl8366ub_sds1_update_bits(struct mtk_eth_priv *eth, u16 addr, u16 mask, u16 val)
{
	u16 data;
	int ret;

	ret = rtl8366ub_sds1_read(eth, addr, &data);
	if (ret)
		return ret;

	data = (data & ~mask) | (val & mask);

	return rtl8366ub_sds1_write(eth, addr, data);
}

static int rtl8366ub_sds1_disable_nway(struct mtk_eth_priv *eth)
{
	static const struct {
		u16 reg;
		u16 mask;
	} nway_regs[] = {
		{ RTL8366UB_SDS1_2500BX_NWAY_REG, RTL8366UB_SDS1_2500BX_NWAY },
		{ RTL8366UB_SDS1_HSGMII_NWAY_REG, RTL8366UB_SDS1_HSGMII_NWAY_MASK },
		{ RTL8366UB_SDS1_TGR_NWAY_REG, RTL8366UB_SDS1_TGR_NWAY },
		{ RTL8366UB_SDS1_USXGMII_NWAY_REG, RTL8366UB_SDS1_USXGMII_NWAY_MASK },
	};
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(nway_regs); i++) {
		ret = rtl8366ub_sds1_update_bits(eth, nway_regs[i].reg,
						 nway_regs[i].mask, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366ub_cpu_port_config(struct mtk_eth_priv *eth)
{
	static const struct rtl8366ub_init_patch sds1_hsgmii_patch[] = {
		{ 0x0530, 0x1e58 },
		{ 0x0521, 0x8023 },
	};
	int ret;
	int i;

	ret = rtl8366ub_sds1_set_mode(eth, RTL8366UB_SDS_MODE_DISABLE);
	if (ret)
		return ret;
	ret = rtl8366ub_sds1_set_mode(eth, RTL8366UB_SDS_MODE_HSGMII);
	if (ret)
		return ret;
	ret = rtl8366ub_update_bits(eth, RTL8366UB_SDS_MISC_2_REG,
				    RTL8366UB_SDS_USX_SUBMODE_MASK, 0);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(sds1_hsgmii_patch); i++) {
		ret = rtl8366ub_sds1_write(eth, sds1_hsgmii_patch[i].reg,
					   sds1_hsgmii_patch[i].val);
		if (ret)
			return ret;
	}

	ret = rtl8366ub_sds1_set_mode(eth, RTL8366UB_SDS_MODE_DISABLE);
	if (ret)
		return ret;
	ret = rtl8366ub_sds1_set_mode(eth, RTL8366UB_SDS_MODE_HSGMII);
	if (ret)
		return ret;
	ret = rtl8366ub_update_bits(eth, RTL8366UB_SDS_MISC_2_REG,
				    RTL8366UB_SDS_USX_SUBMODE_MASK, 0);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_MAC_FORCE_BASE +
					   RTL8366UB_CPU_PHYSICAL_PORT *
						   RTL8366UB_MAC_FORCE_PORT_STRIDE,
				  RTL8366UB_MAC_FORCE_VALUE);
	if (ret)
		return ret;

	return rtl8366ub_sds1_disable_nway(eth);
}

static int rtl8366ub_cpu_tag_disable(struct mtk_eth_priv *eth)
{
	int ret;

	ret = rtl8366ub_reg_write(eth, RTL8366UB_CPU_TAG_AWARE_REG, 0);
	if (ret)
		return ret;

	return rtl8366ub_update_bits(eth, RTL8366UB_CPU_TAG_CTRL_REG,
				     RTL8366UB_CPU_TAG_ENABLE, 0);
}

/* ------------------------------------------------------------------ */
/* mtk_eth switch framework                                            */
/* ------------------------------------------------------------------ */

static int rtl8366ub_detect(struct mtk_eth_priv *eth)
{
	u16 chip_id;
	u16 chip_name;
	u32 char1;
	u32 char2;
	int ret;

	ret = rtl8366ub_reg_read(eth, RTL8366UB_CHIP_ID_REG, &chip_id);
	if (ret)
		return ret;

	ret = rtl8366ub_reg_read(eth, RTL8366UB_CHIP_NAME_REG, &chip_name);
	if (ret)
		return ret;

	char1 = (chip_name >> RTL8366UB_CHIP_NAME_CHAR1_SHIFT) &
		RTL8366UB_CHIP_NAME_CHAR_MASK;
	char2 = (chip_name >> RTL8366UB_CHIP_NAME_CHAR2_SHIFT) &
		RTL8366UB_CHIP_NAME_CHAR_MASK;

	if (chip_id != RTL8366UB_CHIP_ID ||
	    char1 != RTL8366UB_CHIP_CHAR('U') ||
	    char2 != RTL8366UB_CHIP_CHAR('B'))
		return -ENODEV;

	return 0;
}

static int rtl8366ub_setup(struct mtk_eth_switch_priv *swpriv)
{
	struct mtk_eth_priv *eth = swpriv->eth;
	int port;
	int ret;

	/* 1. vendor PHY patch tables */
	ret = rtl8366ub_hw_init(eth);
	if (ret) {
		printf("rtl8366ub: hw init failed (%d)\n", ret);
		return ret;
	}

	/* 2. L2: learning on user ports, off on the CPU port */
	ret = rtl8366ub_l2_setup(eth, RTL8366UB_CPU_LOGICAL_PORT);
	if (ret)
		return ret;

	/*
	 * 3. Flat forwarding state: VLAN 1 spans every physical port
	 *    (UTP 0-3 + EXT 5-7, i.e. the CPU port too) and is untagged
	 *    everywhere, PVID 1, accept untagged frames. This is the switch's
	 *    own power-on default, restored explicitly after reset so the
	 *    bootloader network stack can talk to lan1/lan2 without DSA.
	 */
	ret = rtl8366ub_vlan_set(eth, 1, RTL8366UB_VALID_PHYSICAL_MASK,
				 RTL8366UB_VALID_PHYSICAL_MASK, true);
	if (ret) {
		printf("rtl8366ub: VLAN 1 setup failed (%d)\n", ret);
		return ret;
	}

	for (port = 0; port <= RTL8366UB_MAX_LOGICAL_PORT; port++) {
		if (!rtl8366ub_get_port_info(port))
			continue;

		ret = rtl8366ub_vlan_set_accept(eth, port, RTL8366UB_VLAN_ACCEPT_ALL);
		if (ret)
			return ret;

		ret = rtl8366ub_vlan_set_pvid(eth, port, 1);
		if (ret)
			return ret;

		ret = rtl8366ub_vlan_set_egress_original(eth, port);
		if (ret)
			return ret;

		ret = rtl8366ub_vlan_clear_egress_overrides(eth, port);
		if (ret)
			return ret;
	}

	ret = rtl8366ub_vlan_set_filtering(eth, true);
	if (ret)
		return ret;

	ret = rtl8366ub_vlan_disable_leaky(eth);
	if (ret)
		return ret;

	/* 4. CPU port: HS-GMII 2500BASE-X towards GMAC0, no RTL8_4 tag */
	ret = rtl8366ub_cpu_port_config(eth);
	if (ret) {
		printf("rtl8366ub: CPU port config failed (%d)\n", ret);
		return ret;
	}

	ret = rtl8366ub_cpu_tag_disable(eth);
	if (ret)
		return ret;

	/* 5. power up the UTP PHYs */
	for (port = 0; port < RTL8366UB_NUM_PORTS; port++) {
		ret = rtl8366ub_port_set_enabled(eth, port, true);
		if (ret) {
			printf("rtl8366ub: failed to enable port %d (%d)\n", port, ret);
			return ret;
		}
	}

	printf("rtl8366ub: switch configured (VLAN 1, CPU port 2500BASE-X)\n");

	return 0;
}

static void rtl8366ub_mac_control(struct mtk_eth_switch_priv *swpriv, bool enable)
{
	struct mtk_eth_priv *eth = swpriv->eth;

	/* CPU port MAC is forced statically; keep it forced while running */
	if (!enable)
		return;

	rtl8366ub_reg_write(eth, RTL8366UB_MAC_FORCE_BASE +
				     RTL8366UB_CPU_PHYSICAL_PORT *
					     RTL8366UB_MAC_FORCE_PORT_STRIDE,
			    RTL8366UB_MAC_FORCE_VALUE);
}

MTK_ETH_SWITCH(rtl8366ub) = {
	.name = "rtl8366ub",
	.desc = "Realtek RTL8366UB",
	.priv_size = sizeof(struct rtl8366ub_switch_priv),
	.reset_wait_time = 100,

	.detect = rtl8366ub_detect,
	.setup = rtl8366ub_setup,
	.mac_control = rtl8366ub_mac_control,
};
