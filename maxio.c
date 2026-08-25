// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/net/phy/maxio.c
 *
 * Maxio MAE0621A Ethernet PHY driver
 *
 * Notes:
 * - Adds match_phy_device() so the driver will bind even if the PHY
 *   ID is mis-read as 0xffff4411 or when DT uses
 *   compatible = "ethernet-phy-id7b74.4411".
 * - Adjusts signature of match_phy_device to (struct phy_device *,
 *   const struct phy_driver *) and drops eee_broken_modes, both
 *   removed/changed from struct phy_device on kernel 6.18+.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/timer.h>
#include <linux/version.h>

#define MAXIO_MAE0621A_PHY_ID          0x7b744411U

/* Common registers */
#define MAXIO_PAGE_SELECT               0x1f

/* Vendor-specific pages/registers */
#define MAXIO_MAE0621A_INER             0x12
#define MAXIO_MAE0621A_INER_LINK_STATUS BIT(4)
#define MAXIO_MAE0621A_INSR             0x1d

/* RGMII delays (2ns) bits at vendor page 0xd96, reg 0x0 */
#define MAXIO_MAE0621A_TX_DELAY         (BIT(6) | BIT(7))
#define MAXIO_MAE0621A_RX_DELAY         (BIT(4) | BIT(5))

/* Vendor clock/work status regs (pages: 0xa43, 0xd92) */
#define MAXIO_MAE0621A_CLK_MODE_REG     0x02
#define MAXIO_MAE0621A_WORK_STATUS_REG  0x1d

/* Clause 45 access via Clause 22 registers */
#define MII_MMD_CTRL                    0x0d
#define MII_MMD_DATA                    0x0e

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static int maxio_read_paged(struct phy_device *phydev, int page, u32 regnum)
{
    int ret, oldpage;

    oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);
    if (oldpage < 0)
        return oldpage;

    ret = phy_write(phydev, MAXIO_PAGE_SELECT, page);
    if (ret < 0)
        goto out_restore;

    ret = phy_read(phydev, regnum);

out_restore:
    phy_write(phydev, MAXIO_PAGE_SELECT, oldpage);
    return ret;
}

static int maxio_write_paged(struct phy_device *phydev, int page, u32 regnum, u16 val)
{
    int ret, oldpage;

    oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);
    if (oldpage < 0)
        return oldpage;

    ret = phy_write(phydev, MAXIO_PAGE_SELECT, page);
    if (ret < 0)
        goto out_restore;

    ret = phy_write(phydev, regnum, val);

out_restore:
    phy_write(phydev, MAXIO_PAGE_SELECT, oldpage);
    return ret;
}

/* Minimal clock init sequence as per vendor driver */
static int maxio_mae0621a_clk_init(struct phy_device *phydev)
{
    u16 workmode, clkmode;
    int oldpage;

    oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);
    if (oldpage == 0xFFFF) /* retry once if bus was noisy */
        oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);

    /* Soft reset on default page */
    phy_write(phydev, MAXIO_PAGE_SELECT, 0x0);
    phy_write(phydev, MII_BMCR, BMCR_RESET | phy_read(phydev, MII_BMCR));

    /* Read work mode */
    phy_write(phydev, MAXIO_PAGE_SELECT, 0xa43);
    workmode = phy_read(phydev, MAXIO_MAE0621A_WORK_STATUS_REG);

    /* Read clock mode */
    phy_write(phydev, MAXIO_PAGE_SELECT, 0xd92);
    clkmode = phy_read(phydev, MAXIO_MAE0621A_CLK_MODE_REG);

    /* If abnormal, toggle oscillator/crystal select bit (bit 8) */
    if ((workmode & BIT(5)) == 0) {
        if ((clkmode & BIT(8)) == 0) {
            /* oscillator: set bit 8 */
            phy_write(phydev, MAXIO_MAE0621A_CLK_MODE_REG, clkmode | BIT(8));
        } else {
            /* crystal: clear bit 8 */
            phy_write(phydev, MAXIO_MAE0621A_CLK_MODE_REG, clkmode & ~BIT(8));
        }
    }

    phy_write(phydev, MAXIO_PAGE_SELECT, oldpage);
    return 0;
}

/* Some builds treat warnings as errors; mark read helper maybe-unused */
__maybe_unused
static int maxio_read_mmd(struct phy_device *phydev, int devnum, u16 regnum)
{
    int ret, oldpage;

    oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);
    if (oldpage < 0)
        return oldpage;

    if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV) {
        phy_write(phydev, MAXIO_PAGE_SELECT, 0);
        phy_write(phydev, MII_MMD_CTRL, MDIO_MMD_AN);
        phy_write(phydev, MII_MMD_DATA, MDIO_AN_EEE_ADV);
        phy_write(phydev, MII_MMD_CTRL, 0x4000 | MDIO_MMD_AN);
        ret = phy_read(phydev, MII_MMD_DATA);
    } else {
        ret = -EOPNOTSUPP;
    }

    phy_write(phydev, MAXIO_PAGE_SELECT, oldpage);
    return ret;
}

static int maxio_write_mmd(struct phy_device *phydev, int devnum, u16 regnum, u16 val)
{
    int ret = 0, oldpage;

    oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);
    if (oldpage < 0)
        return oldpage;

    if (devnum == MDIO_MMD_AN && regnum == MDIO_AN_EEE_ADV) {
        phy_write(phydev, MAXIO_PAGE_SELECT, 0);
        ret |= phy_write(phydev, MII_MMD_CTRL, MDIO_MMD_AN);
        ret |= phy_write(phydev, MII_MMD_DATA, MDIO_AN_EEE_ADV);
        ret |= phy_write(phydev, MII_MMD_CTRL, 0x4000 | MDIO_MMD_AN);
        ret |= phy_write(phydev, MII_MMD_DATA, val);
        msleep(100);
        ret |= genphy_restart_aneg(phydev);
    } else {
        ret = -EOPNOTSUPP;
    }

    phy_write(phydev, MAXIO_PAGE_SELECT, oldpage);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* PHY operations                                                            */
/* ------------------------------------------------------------------------- */

static int maxio_mae0621a_config_aneg(struct phy_device *phydev)
{
    return genphy_config_aneg(phydev);
}

/*
 * Accept match for:
 *  - Correct ID 0x7b744411
 *  - DT override compatible "ethernet-phy-id7b74.4411"
 *  - Broken read 0xffff4411 (PHYSID1 == 0xffff, PHYSID2 == 0x4411)
 *
 * Return 1 for match, 0 for no match.
 */
static int maxio_mae0621a_match(struct phy_device *phydev, const struct phy_driver *phydrv)
{
    struct device_node *np = phydev->mdio.dev.of_node;
    u32 id = phydev->phy_id;

    if (id == MAXIO_MAE0621A_PHY_ID)
        return 1;

    if (np && of_device_is_compatible(np, "ethernet-phy-id7b74.4411"))
        return 1;

    if (id == 0xffff4411)
        return 1;

    return 0;
}

static int maxio_mae0621a_config_init(struct phy_device *phydev)
{
    struct device *dev = &phydev->mdio.dev;
    u16 val;
    int ret;
    struct device_node *np = phydev->mdio.dev.of_node;

    /* Normalize to correct ID if matched via DT override or broken read */
    if (phydev->phy_id == 0xffff4411 ||
        (np && of_device_is_compatible(np, "ethernet-phy-id7b74.4411")))
        phydev->phy_id = MAXIO_MAE0621A_PHY_ID;

    maxio_mae0621a_clk_init(phydev);

    /* Disable EEE */
    maxio_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_EEE_ADV, 0);

    /* Enable vendor auto_speed_down (best-effort) */
    ret = maxio_write_paged(phydev, 0xd8f, 0x0, 0x300);
    if (ret < 0)
        dev_dbg(dev, "MAE0621A: auto_speed_down write failed (%d)\n", ret);

    /* Adjust RGMII TX/RX delays from phy-mode */
    switch (phydev->interface) {
    case PHY_INTERFACE_MODE_RGMII:
        val = 0x0;
        break;
    case PHY_INTERFACE_MODE_RGMII_ID:
        val = MAXIO_MAE0621A_TX_DELAY | MAXIO_MAE0621A_RX_DELAY;
        break;
    case PHY_INTERFACE_MODE_RGMII_RXID:
        val = MAXIO_MAE0621A_RX_DELAY;
        break;
    case PHY_INTERFACE_MODE_RGMII_TXID:
        val = MAXIO_MAE0621A_TX_DELAY;
        break;
    default:
        goto delay_skip;
    }

    ret = maxio_read_paged(phydev, 0xd96, 0x0);
    if (ret < 0) {
        dev_err(dev, "MAE0621A: failed to read RGMII delay reg\n");
        return ret;
    }

    ret = maxio_write_paged(phydev, 0xd96, 0x0, (u16)(val | ret));
    if (ret < 0) {
        dev_err(dev, "MAE0621A: failed to write RGMII delay reg\n");
        return ret;
    } else if (ret == 0) {
        dev_dbg(dev, "MAE0621A: 2ns delays already set per strap/boot\n");
    }

delay_skip:
    /* Reset to apply changes */
    phy_write(phydev, MII_BMCR, BMCR_RESET | phy_read(phydev, MII_BMCR));
    msleep(1);
    phy_write(phydev, MAXIO_PAGE_SELECT, 0x0);

    return 0;
}

static int maxio_mae0621a_resume(struct phy_device *phydev)
{
    int ret = genphy_resume(phydev);

    /* Some MACs require the PHY to provide RXCLK early; re-reset */
    ret |= phy_write(phydev, MII_BMCR, BMCR_RESET | phy_read(phydev, MII_BMCR));
    msleep(20);

    return ret;
}

static int maxio_mae0621a_suspend(struct phy_device *phydev)
{
    genphy_suspend(phydev);
    phy_write(phydev, MAXIO_PAGE_SELECT, 0x0);
    return 0;
}

static int maxio_mae0621a_status(struct phy_device *phydev)
{
    return genphy_read_status(phydev);
}

static int maxio_mae0621a_probe(struct phy_device *phydev)
{
    int ret = maxio_mae0621a_clk_init(phydev);
    mdelay(100);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Driver registration                                                       */
/* ------------------------------------------------------------------------- */

static struct phy_driver maxio_nc_drvs[] = {
    {
        .phy_id             = MAXIO_MAE0621A_PHY_ID,
        /* Not used when .match_phy_device returns true */
        .phy_id_mask        = 0xffffffff,
        .name               = "MAE0621A Gigabit Ethernet",
        .features           = PHY_GBIT_FEATURES,
        .probe              = maxio_mae0621a_probe,
        .config_init        = maxio_mae0621a_config_init,
        .config_aneg        = maxio_mae0621a_config_aneg,
        .read_status        = maxio_mae0621a_status,
        .suspend            = maxio_mae0621a_suspend,
        .resume             = maxio_mae0621a_resume,
        .match_phy_device   = maxio_mae0621a_match,
    },
};

module_phy_driver(maxio_nc_drvs);

MODULE_DESCRIPTION("Maxio MAE0621A PHY driver with DT/ID fallback matching");
MODULE_AUTHOR("Zhao Yang, adapted by community");
MODULE_LICENSE("GPL");
