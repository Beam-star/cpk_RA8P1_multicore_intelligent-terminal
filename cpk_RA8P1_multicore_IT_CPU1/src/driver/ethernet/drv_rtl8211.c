/*
 * drv_rtl8211.c - RTL8211F PHY initialization for RA8P1 RMAC
 *
 * Provides two hooks called by FSP during PHY init:
 *   rmac_phy_target_rtl8211_initialize()                - LED config, EEE disable
 *   rmac_phy_target_rtl8211_is_support_link_partner_ability() - link partner check
 *
 * These are referenced from ra_gen/common_data.c via:
 *   .p_target_init = rmac_phy_target_rtl8211_initialize
 *   .p_target_link_partner_ability_get = rmac_phy_target_rtl8211_is_support_link_partner_ability
 */

#include "r_rmac_phy.h"

#define RTL_8211F_PAGE_SELECT  0x1F
#define RTL_8211F_EEELCR_ADDR  0x11
#define RTL_8211F_LED_PAGE     0xD04
#define RTL_8211F_LCR_ADDR     0x10

/*
 * rmac_phy_target_rtl8211_initialize
 *
 * Called once during R_LAYER3_SWITCH_Open() after basic PHY reset.
 * Configures RTL8211F LED behavior and disables EEE LED function.
 */
void rmac_phy_target_rtl8211_initialize(rmac_phy_instance_ctrl_t *p_instance_ctrl)
{
    uint32_t val1, val2 = 0;

    /* Switch to LED register page */
    R_RMAC_PHY_Write(p_instance_ctrl, RTL_8211F_PAGE_SELECT, RTL_8211F_LED_PAGE);

    /*
     * LED Configuration Register (LCR) at address 0x10:
     *   bit 5  = 1 : LED1 (green)  on for 10/100/1000M link
     *   bit 8  = 1 : LED1 (green)  on for activity
     *   bit 9  = 0 : LED2 (yellow) disable 100M link indication
     *   bit 10 = 1 : LED2 (yellow) on for 1000M link
     *   bit 11 = 1 : LED2 (yellow) on for activity
     * Result:
     *   LED1 (green)  = Link + Active at all speeds
     *   LED2 (yellow) = Link 1000M + Active
     */
    R_RMAC_PHY_Read(p_instance_ctrl, RTL_8211F_LCR_ADDR, &val1);
    val1 |= (1 << 5);
    val1 |= (1 << 8);
    val1 &= ~(1 << 9);
    val1 |= (1 << 10);
    val1 |= (1 << 11);
    R_RMAC_PHY_Write(p_instance_ctrl, RTL_8211F_LCR_ADDR, val1);

    /*
     * EEE LED Control Register (EEELCR) at address 0x11:
     *   bit 2 = 0 : disable EEE LED function
     * This keeps LED1 solid ON when linked, instead of blinking for EEE.
     */
    R_RMAC_PHY_Read(p_instance_ctrl, RTL_8211F_EEELCR_ADDR, &val2);
    val2 &= ~(1 << 2);
    R_RMAC_PHY_Write(p_instance_ctrl, RTL_8211F_EEELCR_ADDR, val2);

    /* Switch back to default page (MMD access page 0xA42) */
    R_RMAC_PHY_Write(p_instance_ctrl, RTL_8211F_PAGE_SELECT, 0xA42);
}

/*
 * rmac_phy_target_rtl8211_is_support_link_partner_ability
 *
 * Called during link negotiation to check if the link partner's
 * advertised abilities are supported. RTL8211F supports both
 * half and full duplex at all speeds, so always return true.
 */
bool rmac_phy_target_rtl8211_is_support_link_partner_ability(
    rmac_phy_instance_ctrl_t *p_instance_ctrl,
    uint32_t line_speed_duplex)
{
    FSP_PARAMETER_NOT_USED(p_instance_ctrl);
    FSP_PARAMETER_NOT_USED(line_speed_duplex);

    return true;
}
