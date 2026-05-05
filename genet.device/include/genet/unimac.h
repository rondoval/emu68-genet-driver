/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __UNIMAC_H
#define __UNIMAC_H

#define CMD_TX_EN BIT(0)
#define CMD_RX_EN BIT(1)
#define CMD_SPEED_10 0
#define CMD_SPEED_100 1
#define CMD_SPEED_1000 2
#define CMD_SPEED_2500 3
#define CMD_SPEED_SHIFT 2
#define CMD_SPEED_MASK 3
#define CMD_PROMISC BIT(4)
#define CMD_PAD_EN BIT(5)
#define CMD_CRC_FWD BIT(6)
#define CMD_PAUSE_FWD BIT(7)
#define CMD_RX_PAUSE_IGNORE BIT(8)
#define CMD_TX_ADDR_INS BIT(9)
#define CMD_HD_EN BIT(10)
#define CMD_SW_RESET_OLD BIT(11)
#define CMD_SW_RESET BIT(13)
#define CMD_LCL_LOOP_EN BIT(15)
#define CMD_AUTO_CONFIG BIT(22)
#define CMD_CNTL_FRM_EN BIT(23)
#define CMD_NO_LEN_CHK BIT(24)
#define CMD_RMT_LOOP_EN BIT(25)
#define CMD_RX_ERR_DISC BIT(26)
#define CMD_PRBL_EN BIT(27)
#define CMD_TX_PAUSE_IGNORE BIT(28)
#define CMD_TX_RX_EN BIT(29)
#define CMD_RUNT_FILTER_DIS BIT(30)

#endif
