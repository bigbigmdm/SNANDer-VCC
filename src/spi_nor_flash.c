/*
 * Copyright (C) 2018-2021 McMCC <mcmcc@mail.ru>
 * spi_nor_flash.c
 * Adding power supply voltage by Mikhail Medvedev <e-ink-reader@yandex.ru>
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include "spi_controller.h"
#include "snorcmd_api.h"
#include "types.h"
#include "timer.h"

#define min(a,b) (((a)<(b))?(a):(b))

/******************************************************************************
 * SPI FLASH elementray definition and function
 ******************************************************************************/

#define FLASH_PAGESIZE			256

/* Flash opcodes. */
#define OPCODE_WREN			6	/* Write enable */
#define OPCODE_WRDI			4	/* Write disable */
#define OPCODE_RDSR			5	/* Read status register */
#define OPCODE_WRSR			1	/* Write status register */
#define OPCODE_READ			3	/* Read data bytes */
#define OPCODE_PP			2	/* Page program */
#define OPCODE_SE			0xD8	/* Sector erase */
#define OPCODE_RES			0xAB	/* Read Electronic Signature */
#define OPCODE_RDID			0x9F	/* Read JEDEC ID */

#define OPCODE_FAST_READ		0x0B	/* Fast Read */
#define OPCODE_DOR			0x3B	/* Dual Output Read */
#define OPCODE_QOR			0x6B	/* Quad Output Read */
#define OPCODE_DIOR			0xBB	/* Dual IO High Performance Read */
#define OPCODE_QIOR			0xEB	/* Quad IO High Performance Read */
#define OPCODE_READ_ID			0x90	/* Read Manufacturer and Device ID */

#define OPCODE_P4E			0x20	/* 4KB Parameter Sectore Erase */
#define OPCODE_P8E			0x40	/* 8KB Parameter Sectore Erase */
#define OPCODE_BE			0x60	/* Bulk Erase */
#define OPCODE_BE1			0xC7	/* Bulk Erase */
#define OPCODE_QPP			0x32	/* Quad Page Programing */

#define OPCODE_CLSR			0x30
#define OPCODE_RCR			0x35	/* Read Configuration Register */

#define OPCODE_BRRD			0x16
#define OPCODE_BRWR			0x17

/* Status Register bits. */
#define SR_WIP				1	/* Write in progress */
#define SR_WEL				2	/* Write enable latch */
#define SR_BP0				4	/* Block protect 0 */
#define SR_BP1				8	/* Block protect 1 */
#define SR_BP2				0x10	/* Block protect 2 */
#define SR_EPE				0x20	/* Erase/Program error */
#define SR_SRWD				0x80	/* SR write protect */

#define snor_dbg(args...)
/* #define snor_dbg(args...) do { if (1) printf(args); } while(0) */

#define udelay(x)			usleep(x)

struct chip_info {
	char		*name;
	u8		id;
	u32		jedec_id;
	unsigned long	sector_size;
	unsigned int	n_sectors;
	char		addr4b;
	float vcc_min;
    float vcc_max;
};

struct chip_info *spi_chip_info;

static int snor_wait_ready(int sleep_ms);
static int snor_read_sr(u8 *val);
static int snor_write_sr(u8 *val);

extern unsigned int bsize;

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
static inline void snor_write_enable(void)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_WREN);
	SPI_CONTROLLER_Chip_Select_High();
}

static inline void snor_write_disable(void)
{
	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_WRDI);
	SPI_CONTROLLER_Chip_Select_High();
}

/*
 * Set all sectors (global) unprotected if they are protected.
 * Returns negative if error occurred.
 */
static inline int snor_unprotect(void)
{
	u8 sr = 0;

	if (snor_read_sr(&sr) < 0) {
		printf("%s: read_sr fail: %x\n", __func__, sr);
		return -1;
	}

	if ((sr & (SR_BP0 | SR_BP1 | SR_BP2)) != 0) {
		sr = 0;
		snor_write_sr(&sr);
	}
	return 0;
}

/*
 * Service routine to read status register until ready, or timeout occurs.
 * Returns non-zero if error.
 */
static int snor_wait_ready(int sleep_ms)
{
	int count;
	int sr = 0;

	/* one chip guarantees max 5 msec wait here after page writes,
	 * but potentially three seconds (!) after page erase.
	 */
	for (count = 0; count < ((sleep_ms + 1) * 1000); count++) {
		if ((snor_read_sr((u8 *)&sr)) < 0)
			break;
		else if (!(sr & (SR_WIP | SR_EPE | SR_WEL))) {
			return 0;
		}
		udelay(500);
		/* REVISIT sometimes sleeping would be best */
	}
	printf("%s: read_sr fail: %x\n", __func__, sr);
	return -1;
}

/*
 * read status register
 */
static int snor_read_rg(u8 code, u8 *val)
{
	int retval;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(code);
	retval = SPI_CONTROLLER_Read_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return -1;
	}

	return 0;
}

/*
 * write status register
 */
static int snor_write_rg(u8 code, u8 *val)
{
	int retval;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(code);
	retval = SPI_CONTROLLER_Write_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return -1;
	}

	return 0;
}

static int snor_4byte_mode(int enable)
{
	int retval;

	if (snor_wait_ready(1))
		return -1;

	if (spi_chip_info->id == 0x1) /* Spansion */
	{
		u8 br_cfn;
		u8 br = enable ? 0x81 : 0;

		snor_write_rg(OPCODE_BRWR, &br);
		snor_read_rg(OPCODE_BRRD, &br_cfn);
		if (br_cfn != br) {
			printf("4B mode switch failed %s, 0x%02x, 0x%02x\n", enable ? "enable" : "disable" , br_cfn, br);
			return -1;
		}
	} else {
		u8 code = enable ? 0xb7 : 0xe9; /* B7: enter 4B, E9: exit 4B */

		SPI_CONTROLLER_Chip_Select_Low();
		retval = SPI_CONTROLLER_Write_One_Byte(code);
		SPI_CONTROLLER_Chip_Select_High();
		if (retval) {
			printf("%s: ret: %x\n", __func__, retval);
			return -1;
		}
		if ((!enable) && (spi_chip_info->id == 0xef)) /* Winbond */
		{
			code = 0;
			snor_write_enable();
			snor_write_rg(0xc5, &code);
		}
	}
	return 0;
}

/*
 * Erase one sector of flash memory at offset ``offset'' which is any
 * address within the sector which should be erased.
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int snor_erase_sector(unsigned long offset)
{
	snor_dbg("%s: offset:%x\n", __func__, offset);

	/* Wait until finished previous write command. */
	if (snor_wait_ready(950))
		return -1;

	if (spi_chip_info->addr4b) {
		snor_4byte_mode(1);
	}

	/* Send write enable, then erase commands. */
	snor_write_enable();

	SPI_CONTROLLER_Chip_Select_Low();

	SPI_CONTROLLER_Write_One_Byte(OPCODE_SE);
	if (spi_chip_info->addr4b)
		SPI_CONTROLLER_Write_One_Byte((offset >> 24) & 0xff);
	SPI_CONTROLLER_Write_One_Byte((offset >> 16) & 0xff);
	SPI_CONTROLLER_Write_One_Byte((offset >> 8) & 0xff);
	SPI_CONTROLLER_Write_One_Byte(offset & 0xff);

	SPI_CONTROLLER_Chip_Select_High();

	snor_wait_ready(950);

	if (spi_chip_info->addr4b)
		snor_4byte_mode(0);

	return 0;
}

static int full_erase_chip(void)
{
	timer_start();
	/* Wait until finished previous write command. */
	if (snor_wait_ready(3))
		return -1;

	/* Send write enable, then erase commands. */
	snor_write_enable();
	snor_unprotect();

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_BE1);
	SPI_CONTROLLER_Chip_Select_High();

	snor_wait_ready(950);
	snor_write_disable();
	timer_end();

	return 0;
}

static struct chip_info chips_data [] = {
//Please instert this text in file spi_nor_flash.c (replace old table) and recompile the program 
	/* REVISIT: fill in JEDEC ids, for parts that have them */
//     NAME            MANUF.ID  CHIP ID     BL.SIZE  BLOCKS  MODE 
// SPI_FLASH SPANSION --> https://uk.farnell.com/w/c/semiconductors-ics/memory/flash?ic-interface-type=spi
  	{ "FL016AIF",           0x01, 0x02140000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "S25FL016P",          0x01, 0x02144d00, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "S25FL032P",          0x01, 0x02154d00, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "FL064AIF",           0x01, 0x02160000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "S25FL064P",          0x01, 0x02164d00, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "S25FL256S",          0x01, 0x02194d01, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "S25FL128P",          0x01, 0x20180301, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "S25FL129P",          0x01, 0x20184d01, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "S25FL116K",          0x01, 0x40150140, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "S25FL132K",          0x01, 0x40160140, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "S25FL164K",          0x01, 0x40170140, 64 * 1024, 128,  0, 2.70, 3.60 },
// SPI_FLASH XTX --> http://www.xtxtech.com/products_detail/1/ProId-1/
  	{ "XT25F02E",           0x0b, 0x40120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "XT25F04D",           0x0b, 0x40130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "XT25F08B",           0x0b, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "XT25F16B",           0x0b, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "XT25F32F",           0x0b, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "XT25F64F",           0x0b, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "XT25F128F",          0x0b, 0x40180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "XT25W02E",           0x0b, 0x60120000, 64 * 1024, 4,    0, 1.65, 3.60 },
	{ "XT25W04D",           0x0b, 0x60130000, 64 * 1024, 8,    0, 1.65, 3.60 },
	{ "XT25Q08D",           0x0b, 0x60140000, 64 * 1024, 16,   0, 1.65, 2.00 },
	{ "XT25Q16D",           0x0b, 0x60150000, 64 * 1024, 32,   0, 1.65, 2.00 },
	{ "XT25Q64D",           0x0b, 0x60170000, 64 * 1024, 128,  0, 1.65, 2.00 },
	{ "XT25F128D",          0x0b, 0x60180000, 64 * 1024, 256,  0, 1.65, 2.00 },
// SPI_FLASH EON --> https://esmt.com.tw/en/Products/Flash/SPI%20NOR-2-8#8Mb
  	{ "EN25B10T",           0x1c, 0x20110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "EN25B20T",           0x1c, 0x20120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "EN25B40T",           0x1c, 0x20130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "EN25B80T",           0x1c, 0x20140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "EN25B16T",           0x1c, 0x20150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "EN25B32T",           0x1c, 0x20160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25B64T",           0x1c, 0x20170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "EN25F64",            0x1c, 0x20171c20, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "EN25Q40A",           0x1c, 0x30130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "EN25Q80B",           0x1c, 0x30140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "EN25Q16",            0x1c, 0x30151c30, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "EN25Q32C",           0x1c, 0x30160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25Q64",            0x1c, 0x30170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "EN25Q128",           0x1c, 0x30181c30, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "EN25F10A",           0x1c, 0x31110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "EN25F20A",           0x1c, 0x31120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "EN25F40",            0x1c, 0x31130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "EN25F80",            0x1c, 0x31140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "EN25F16",            0x1c, 0x31151c31, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "EN25F32",            0x1c, 0x31161c30, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25S10A",           0x1c, 0x38110000, 64 * 1024, 2,    0, 1.65, 1.95 },
	{ "EN25S20A",           0x1c, 0x38120000, 64 * 1024, 4,    0, 1.65, 1.95 },
	{ "EN25S40A",           0x1c, 0x38130000, 64 * 1024, 8,    0, 1.65, 1.95 },
	{ "EN25S80B",           0x1c, 0x38140000, 64 * 1024, 16,   0, 1.65, 1.95 },
	{ "EN25S16B",           0x1c, 0x38150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "EN25S64A",           0x1c, 0x38170000, 64 * 1024, 128,  0, 1.65, 1.95 },
	{ "EN25QE32A",          0x1c, 0x41160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25E10A",           0x1c, 0x42110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "EN25E40A",           0x1c, 0x42130000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "EN25SE16A",          0x1c, 0x48150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "EN25SE32A",          0x1c, 0x48160000, 64 * 1024, 64,   0, 1.65, 1.95 },
	{ "EN25T80",            0x1c, 0x51140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "EN25QA32B",          0x1c, 0x60160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25QA64A",          0x1c, 0x60170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "EN25QA128A",         0x1c, 0x60180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "EN25QW16A",          0x1c, 0x61150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "EN25QW32A",          0x1c, 0x61160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25QH16",           0x1c, 0x70151c70, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "EN25QH32B",          0x1c, 0x70160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "EN25QH64A",          0x1c, 0x70171c70, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "EN25QH128A",         0x1c, 0x70181c70, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "EN25Q256",           0x1c, 0x70191c70, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "EN25QX64A",          0x1c, 0x71170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "EN25QX128A",         0x1c, 0x71180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "EN25QX256A",         0x1c, 0x71190000, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "EN25QY256A",         0x1c, 0x73190000, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "EN25SX64A",          0x1c, 0x78170000, 64 * 1024, 128,  0, 1.65, 1.95 },
	{ "EN25SX128A",         0x1c, 0x78180000, 64 * 1024, 256,  0, 1.65, 1.95 },
// SPI_FLASH ATMEL --> https://www.microchipdirect.com/newproducttree.aspx?mid=12&catalog=buymicrochip&category=memorytree&treeid=3
  	{ "AT26DF161",          0x1f, 0x46000000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "AT25DF321",          0x1f, 0x47000000, 64 * 1024, 64,   0, 2.70, 3.60 },
// SPI_FLASH MICRON --> https://xmcwh.com/en/site/product
  	{ "M25P10",             0x20, 0x20110000, 64 * 1024, 2,    0, 2.30, 3.60 },
	{ "M25P20",             0x20, 0x20120000, 64 * 1024, 4,    0, 2.30, 3.60 },
	{ "M25P40",             0x20, 0x20130000, 64 * 1024, 8,    0, 2.30, 3.60 },
	{ "M25P80",             0x20, 0x20140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "M25P016",            0x20, 0x20150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "M25P32",             0x20, 0x20160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "M25P64",             0x20, 0x20170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "M25P128",            0x20, 0x20180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "XM25QH10B",          0x20, 0x40110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "XM25QH20B",          0x20, 0x40120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "XM25QH40B",          0x20, 0x40130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "XM25QH80B",          0x20, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "XM25QH16C",          0x20, 0x40150000, 64 * 1024, 32,   0, 2.30, 3.60 },
	{ "XM25QH32B",          0x20, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "XM25QH64C",          0x20, 0x40170000, 64 * 1024, 128,  0, 2.30, 3.60 },
	{ "XM25QH128C",         0x20, 0x40182070, 64 * 1024, 256,  0, 2.30, 3.60 },
	{ "XM25QH256C",         0x20, 0x40190000, 64 * 1024, 512,  1, 2.30, 3.60 },
	{ "XM25QH512C",         0x20, 0x40200000, 64 * 1024, 1024, 1, 2.30, 3.60 },
	{ "XM25LU64C",          0x20, 0x41170000, 64 * 1024, 128,  0, 1.65, 1.95 },
	{ "XM25LU128C",         0x20, 0x41180000, 64 * 1024, 256,  0, 1.65, 1.95 },
	{ "XM25QU256C",         0x20, 0x41190000, 64 * 1024, 512,  1, 1.65, 1.95 },
	{ "XM25QU512C",         0x20, 0x41200000, 64 * 1024, 1024, 1, 1.65, 1.95 },
	{ "XM25QW16C",          0x20, 0x42150000, 64 * 1024, 32,   0, 1.65, 3.60 },
	{ "XM25QW32C",          0x20, 0x42160000, 64 * 1024, 64,   0, 1.65, 3.60 },
	{ "XM25QW64C",          0x20, 0x42170000, 64 * 1024, 128,  0, 1.65, 3.60 },
	{ "XM25QW128C",         0x20, 0x42180000, 64 * 1024, 256,  0, 1.65, 3.60 },
	{ "XM25QW256C",         0x20, 0x42190000, 64 * 1024, 512,  1, 1.65, 3.60 },
	{ "XM25QW512C",         0x20, 0x42200000, 64 * 1024, 1024, 1, 1.65, 3.60 },
	{ "XM25QU41B",          0x20, 0x50130000, 64 * 1024, 8,    0, 1.65, 1.95 },
	{ "XM25QU80B",          0x20, 0x50140000, 64 * 1024, 16,   0, 1.65, 1.95 },
	{ "XM25QU16C",          0x20, 0x50150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "XM25LU32C",          0x20, 0x50160000, 64 * 1024, 64,   0, 1.65, 1.95 },
	{ "XM25QH32A",          0x20, 0x70160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "XM25QH64A",          0x20, 0x70170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "XM25QH128A",         0x20, 0x70182070, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "N25Q032A",           0x20, 0xba160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "N25Q064A",           0x20, 0xba170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "MT25QL64AB",         0x20, 0xba170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "N25Q128A",           0x20, 0xba180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "MT25QL128AB",        0x20, 0xba180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "N25Q256A",           0x20, 0xba190000, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "MT25QL256AB",        0x20, 0xba190000, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "MT25QL512AB",        0x20, 0xba200000, 64 * 1024, 1024, 1, 2.70, 3.60 },
	{ "N25Q032A",           0x20, 0xbb160000, 64 * 1024, 64,   0, 1.70, 2.00 },
	{ "N25Q064A",           0x20, 0xbb170000, 64 * 1024, 128,  0, 1.70, 2.00 },
	{ "MT25QU64AB",         0x20, 0xbb170000, 64 * 1024, 128,  0, 1.70, 2.00 },
	{ "N25Q128A",           0x20, 0xbb180000, 64 * 1024, 256,  0, 1.70, 2.00 },
	{ "MT25QU128AB",        0x20, 0xbb180000, 64 * 1024, 256,  0, 1.70, 2.00 },
	{ "MT25QU256AB",        0x20, 0xbb190000, 64 * 1024, 512,  1, 1.70, 2.00 },
	{ "MT25QU512AB",        0x20, 0xbb200000, 64 * 1024, 1024, 1, 1.70, 2.00 },
// SPI_FLASH AMIC --> http://amictechnology.com/english/flash_spi_flash.html
  	{ "A25L10PU",           0x37, 0x20110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "A25L20PU",           0x37, 0x20120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "A25L40PU",           0x37, 0x20120000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "A25L80PU",           0x37, 0x20140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "A25L16PU",           0x37, 0x20150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "A25L10PT",           0x37, 0x20210000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "A25L20PT",           0x37, 0x20220000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "A25L40PT",           0x37, 0x20220000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "A25L80PT",           0x37, 0x20240000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "A25L16PT",           0x37, 0x20250000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "A25L010",            0x37, 0x30110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "A25L020",            0x37, 0x30120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "A25L040",            0x37, 0x30130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "A25L040",            0x37, 0x30130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "A25L080",            0x37, 0x30140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "A25L016",            0x37, 0x30150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "A25L032",            0x37, 0x30160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "A25LQ080",           0x37, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "A25LQ16",            0x37, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "A25LQ32",            0x37, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "A25LQ64",            0x37, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
// SPI_FLASH EXCELSEMI --> 
  	{ "ES25P10",            0x4a, 0x20110000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "ES25P20",            0x4a, 0x20120000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "ES25P40",            0x4a, 0x20130000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "ES25P80",            0x4a, 0x20140000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "ES25P16",            0x4a, 0x20150000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "ES25P32",            0x4a, 0x20160000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "ES25M40A",           0x4a, 0x32130000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "ES25M80A",           0x4a, 0x32140000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "ES25M16A",           0x4a, 0x32150000, 64 * 1024, 64,   0, 2.70, 3.60 },
// SPI_FLASH DOUQI --> http://www.douqitech.com/pd.jsp?fromColId=129&id=7#_pp=129_348
  	{ "DQ25Q64AS",          0x54, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
// SPI_FLASH Zbit --> https://www.szxghc.com/flash/28.html
  	{ "ZB25LD10A",          0x5e, 0x10110000, 64 * 1024, 2,    0, 1.65, 1.95 },
	{ "ZB25LD20A",          0x5e, 0x10120000, 64 * 1024, 4,    0, 1.65, 1.95 },
	{ "ZB25LD40B",          0x5e, 0x10130000, 64 * 1024, 8,    0, 1.65, 1.95 },
	{ "ZB25LD80",           0x5e, 0x10140000, 64 * 1024, 16,   0, 1.65, 1.95 },
	{ "ZB25D10A",           0x5e, 0x32110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "ZB25D20A",           0x5e, 0x32120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "ZB25D40B",           0x5e, 0x32130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "ZB25D80B",           0x5e, 0x32140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "ZB25VQ16",           0x5e, 0x40150000, 64 * 1024, 32,   0, 2.30, 3.60 },
	{ "ZB25VQ32",           0x5e, 0x40160000, 64 * 1024, 64,   0, 2.30, 3.60 },
	{ "ZB25VQ64",           0x5e, 0x40170000, 64 * 1024, 128,  0, 2.30, 3.60 },
	{ "ZB25VQ128",          0x5e, 0x40180000, 64 * 1024, 256,  0, 2.30, 3.60 },
	{ "ZB25LQ16",           0x5e, 0x50150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "ZB25LQ32",           0x5e, 0x50160000, 64 * 1024, 64,   0, 1.65, 1.95 },
	{ "ZB25LQ64",           0x5e, 0x50170000, 64 * 1024, 128,  0, 1.65, 1.95 },
	{ "ZB25LQ128",          0x5e, 0x50180000, 64 * 1024, 256,  0, 1.65, 1.95 },
	{ "ZB25VQ20A",          0x5e, 0x60120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "ZB25VQ40A",          0x5e, 0x60130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "ZB25VQ80A",          0x5e, 0x60140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "ZB25VQ16A",          0x5e, 0x60150000, 64 * 1024, 32,   0, 2.70, 3.60 },
// SPI_FLASH ONSemi --> https://www.onsemi.com/products/timing-logic-memory/memory/eeprom-memory
  	{ "LE25U20AMB",         0x62, 0x06120000, 64 * 1024, 4,    0, 2.30, 3.60 },
	{ "LE25U40CMC",         0x62, 0x06130000, 64 * 1024, 8,    0, 2.30, 3.60 },
// SPI_FLASH Boya --> http://www.boyamicro.com/SPI_Nor_Flash.php
  	{ "BY25Q05AW",          0x68, 0x10100000, 64 * 1024, 1,    0, 1.65, 3.60 },
	{ "BY25Q10AW",          0x68, 0x10110000, 64 * 1024, 2,    0, 1.65, 3.60 },
	{ "BY25Q20BL",          0x68, 0x10120000, 64 * 1024, 4,    0, 1.65, 2.00 },
	{ "BY25Q40BL",          0x68, 0x10130000, 64 * 1024, 8,    0, 1.65, 2.10 },
	{ "BY25Q80AW",          0x68, 0x10140000, 64 * 1024, 16,   0, 1.65, 2.00 },
	{ "BY25Q16BL",          0x68, 0x10150000, 64 * 1024, 32,   0, 1.65, 2.00 },
	{ "BY25D05AS",          0x68, 0x40100000, 64 * 1024, 1,    0, 2.70, 3.60 },
	{ "BY25D10AS",          0x68, 0x40110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "BY25D20AS",          0x68, 0x40120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "BY25D40AS",          0x68, 0x40130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "BY25Q80BS",          0x68, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "BY25Q16BS",          0x68, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "BY25Q32BS",          0x68, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "BY25Q64AS",          0x68, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "BY25Q128AS",         0x68, 0x40180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "BY25Q256ES",         0x68, 0x40190000, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "BY25Q10AL",          0x68, 0x60110000, 64 * 1024, 2,    0, 1.65, 2.00 },
	{ "BY25Q20AL",          0x68, 0x60120000, 64 * 1024, 4,    0, 1.65, 2.00 },
	{ "BY25Q40AL",          0x68, 0x60130000, 64 * 1024, 8,    0, 1.65, 2.00 },
	{ "BY25Q32AL",          0x68, 0x60160000, 64 * 1024, 64,   0, 1.65, 2.00 },
	{ "BY25Q64AL",          0x68, 0x60170000, 64 * 1024, 128,  0, 1.65, 2.00 },
	{ "BY25Q128EL",         0x68, 0x60180000, 64 * 1024, 256,  0, 1.65, 2.00 },
// SPI_FLASH PFLASH --> 
  	{ "Pm25LQ512B",         0x7f, 0x9d200500, 64 * 1024, 1,    0, 2.70, 3.60 },
	{ "Pm25LQ010B",         0x7f, 0x9d211000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "Pm25LQ020B",         0x7f, 0x9d421100, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "PM25LQ016",          0x7f, 0x9d450000, 64 * 1024, 32,   0, 2.30, 3.60 },
	{ "PM25LQ032",          0x7f, 0x9d460000, 64 * 1024, 64,   0, 2.30, 3.60 },
	{ "PM25LQ064",          0x7f, 0x9d470000, 64 * 1024, 128,  0, 2.30, 3.60 },
	{ "PM25LQ128",          0x7f, 0x9d480000, 64 * 1024, 256,  0, 2.30, 3.60 },
	{ "Pm25LQ040B",         0x7f, 0x9d7e7e00, 64 * 1024, 8,    0, 2.70, 3.60 },
// SPI_FLASH Puya --> https://www.puyasemi.com/cpzx/info_131_aid_198_kid_195.html
  	{ "P25Q06H",            0x85, 0x00100000, 64 * 1024, 1,    0, 2.70, 3.60 },
	{ "P25Q40H",            0x85, 0x20130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "P25Q11H",            0x85, 0x40110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "P25Q21H",            0x85, 0x40120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "P25Q10H",            0x85, 0x60110000, 64 * 1024, 2,    0, 2.30, 3.60 },
	{ "P25Q20H",            0x85, 0x60120000, 64 * 1024, 4,    0, 2.30, 3.60 },
	{ "P25Q40H",            0x85, 0x60130000, 64 * 1024, 8,    0, 2.30, 3.60 },
	{ "P25Q80H",            0x85, 0x60140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "P25Q16H",            0x85, 0x60150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "P25Q32H",            0x85, 0x60160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "P25Q64H",            0x85, 0x60170000, 64 * 1024, 128,  0, 2.30, 3.60 },
	{ "P25Q128H",           0x85, 0x60180000, 64 * 1024, 256,  0, 2.30, 3.60 },
// SPI_FLASH ESMT --> https://www.esmt.com.tw/en/Products
  	{ "F25L004A",           0x8c, 0x20130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "F25L008A",           0x8c, 0x20140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "F25L016",            0x8c, 0x21150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "F25L032",            0x8c, 0x21160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "F25L064",            0x8c, 0x21170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "F25L16QA",           0x8c, 0x41158c41, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "F25L32QA",           0x8c, 0x41168c41, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "F25L64QA",           0x8c, 0x41170000, 64 * 1024, 128,  0, 2.70, 3.60 },
// SPI_FLASH ISSI --> https://www.issi.com/US/product-flash.shtml
  	{ "IS25LQ010",          0x9d, 0x40110000, 64 * 1024, 2,    0, 2.30, 3.60 },
	{ "IS25LQ020",          0x9d, 0x40120000, 64 * 1024, 4,    0, 2.30, 3.60 },
	{ "IS25LP080D",         0x9d, 0x60140000, 64 * 1024, 16,   0, 2.30, 3.60 },
	{ "IS25LP016D",         0x9d, 0x60150000, 64 * 1024, 32,   0, 2.30, 3.60 },
	{ "IS25LP032D",         0x9d, 0x60160000, 64 * 1024, 64,   0, 2.30, 3.60 },
	{ "IS25LP064D",         0x9d, 0x60170000, 64 * 1024, 128,  0, 2.30, 3.60 },
	{ "IS25LP128F",         0x9d, 0x60180000, 64 * 1024, 256,  0, 2.30, 3.60 },
	{ "IS25LP256D",         0x9d, 0x60190000, 64 * 1024, 512,  1, 2.30, 3.60 },
	{ "IS25LP512D",         0x9d, 0x601a0000, 64 * 1024, 1024, 1, 2.30, 3.60 },
	{ "IS25WP040D",         0x9d, 0x70130000, 64 * 1024, 8,    0, 1.65, 1.95 },
	{ "IS25WP080D",         0x9d, 0x70140000, 64 * 1024, 16,   0, 1.65, 1.95 },
	{ "IS25WP016D",         0x9d, 0x70150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "IS25WP032D",         0x9d, 0x70160000, 64 * 1024, 64,   0, 1.65, 1.95 },
	{ "IS25WP064D",         0x9d, 0x70170000, 64 * 1024, 128,  0, 1.65, 1.95 },
	{ "IS25WP128F",         0x9d, 0x70180000, 64 * 1024, 256,  0, 1.65, 1.95 },
	{ "IS25WP256D",         0x9d, 0x70190000, 64 * 1024, 512,  1, 1.65, 1.95 },
	{ "IS25WP512D",         0x9d, 0x701a0000, 64 * 1024, 1024, 1, 1.65, 1.95 },
// SPI_FLASH Fudan --> https://www.fm-chips.com/spi-nor-flash.html
  	{ "FM25W04",            0xa1, 0x28130000, 64 * 1024, 8,    0, 1.65, 3.60 },
	{ "FM25W16",            0xa1, 0x28150000, 64 * 1024, 32,   0, 1.65, 3.60 },
	{ "FM25W32",            0xa1, 0x28160000, 64 * 1024, 64,   0, 1.65, 3.60 },
	{ "FM25W64",            0xa1, 0x28170000, 64 * 1024, 128,  0, 1.65, 3.60 },
	{ "FM25W128",           0xa1, 0x28180000, 64 * 1024, 256,  0, 1.65, 3.60 },
	{ "FM25Q04",            0xa1, 0x40130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "FM25Q08",            0xa1, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "FM25Q16",            0xa1, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "FS25Q32",            0xa1, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "FS25Q64",            0xa1, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "FS25Q128",           0xa1, 0x40180000, 64 * 1024, 256,  0, 2.70, 3.60 },
// SPI_FLASH Zetta --> http://en.zettadevice.com/detail_10.html
  	{ "ZD25Q64B",           0xba, 0x32170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "ZD25LQ128",          0xba, 0x42180000, 64 * 1024, 256,  0, 1.65, 1.95 },
	{ "ZD25LQ64",           0xba, 0x43170000, 64 * 1024, 128,  0, 1.65, 1.95 },
	{ "ZD25WD20B",          0xba, 0x60120000, 64 * 1024, 4,    0, 1.65, 3.60 },
	{ "ZD25WD40B",          0xba, 0x60130000, 64 * 1024, 8,    0, 1.65, 3.60 },
	{ "ZD25Q80C",           0xba, 0x60140000, 64 * 1024, 16,   0, 2.30, 3.60 },
	{ "ZD25Q16B",           0xba, 0x60150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "ZD25Q32C",           0xba, 0x60160000, 64 * 1024, 64,   0, 2.70, 3.60 },
// SPI_FLASH PCT --> https://www.pct.com.tw/product/70
  	{ "PCT25VF016B",        0xbf, 0x25410000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "PCT25VF032B",        0xbf, 0x254a0000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "PCT25VF064C",        0xbf, 0x254b0000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "PCT25VF020B",        0xbf, 0x258c0000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "PCT25VF040B",        0xbf, 0x258d0000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "PCT25VF080B",        0xbf, 0x258e0000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "PCT26VF016",         0xbf, 0x26010000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "PCT26VF032",         0xbf, 0x26020000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "PCT25VF010A",        0xbf, 0x49000000, 64 * 1024, 2,    0, 2.70, 3.60 },
// SPI_FLASH MXIC --> https://www.macronix.com/en-us/products/NOR-Flash/Serial-NOR-Flash/Pages/default.aspx#!tabs=2-5-3V64Mb
  	{ "MX25L8005M",         0xc2, 0x2014c220, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "MX25L1605D",         0xc2, 0x2015c220, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "MX25L3205D",         0xc2, 0x2016c220, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "MX25L6405D",         0xc2, 0x2017c220, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "MX25L12805D",        0xc2, 0x2018c220, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "MX25L25635E",        0xc2, 0x2019c220, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "MX25L51245G",        0xc2, 0x201ac220, 64 * 1024, 1024, 1, 2.70, 3.60 },
	{ "MX25U5121E",         0xc2, 0x25300000, 64 * 1024, 1,    0, 1.65, 2.00 },
	{ "MX25U1001E",         0xc2, 0x25310000, 64 * 1024, 2,    0, 1.65, 2.00 },
	{ "MX25U2035F",         0xc2, 0x25320000, 64 * 1024, 4,    0, 1.65, 2.00 },
	{ "MX25U4035F",         0xc2, 0x25330000, 64 * 1024, 8,    0, 1.65, 2.00 },
	{ "MX25U80356",         0xc2, 0x25340000, 64 * 1024, 16,   0, 1.65, 2.00 },
	{ "MX25U1632F",         0xc2, 0x25350000, 64 * 1024, 32,   0, 1.65, 2.00 },
	{ "MX25U3232F",         0xc2, 0x25360000, 64 * 1024, 64,   0, 1.65, 2.00 },
	{ "MX25U6432F",         0xc2, 0x25370000, 64 * 1024, 128,  0, 1.65, 2.00 },
	{ "MX25U12832F",        0xc2, 0x25380000, 64 * 1024, 256,  0, 1.65, 2.00 },
	{ "MX25U25643G",        0xc2, 0x25390000, 64 * 1024, 512,  1, 1.65, 2.00 },
	{ "MX25U51245G",        0xc2, 0x253a0000, 64 * 1024, 1024, 1, 1.65, 2.00 },
	{ "MX25R2035F",         0xc2, 0x28120000, 64 * 1024, 4,    0, 1.65, 3.60 },
	{ "MX25R4035F",         0xc2, 0x28130000, 64 * 1024, 8,    0, 1.65, 3.60 },
	{ "MX25R8035F",         0xc2, 0x28140000, 64 * 1024, 16,   0, 1.65, 3.60 },
	{ "MX25R1635F",         0xc2, 0x28150000, 64 * 1024, 32,   0, 1.65, 3.60 },
	{ "MX25R3235F",         0xc2, 0x28160000, 64 * 1024, 64,   0, 1.65, 3.60 },
	{ "MX25R6435F",         0xc2, 0x28170000, 64 * 1024, 128,  0, 1.65, 3.60 },
// SPI_FLASH GigaDevice --> https://www.gigadevice.com/product/flash/product-series/spi-nor-flash
  	{ "GD25F40",            0xc8, 0x20130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "GD25F80",            0xc8, 0x20140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "GD25D40",            0xc8, 0x30130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "GD25D80",            0xc8, 0x30140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "GD25D05C",           0xc8, 0x40100000, 64 * 1024, 1,    0, 2.70, 3.60 },
	{ "GD25D10C",           0xc8, 0x40110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "GD25Q20C",           0xc8, 0x40120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "GD25Q40C",           0xc8, 0x40130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "GD25Q80C",           0xc8, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "GD25Q16C",           0xc8, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "GD25Q32",            0xc8, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "GD25Q64CSIG",        0xc8, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "GD25Q128CSIG",       0xc8, 0x4018c840, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "GD25Q256CSIG",       0xc8, 0x4019c840, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "GD25LD05C",          0xc8, 0x60100000, 64 * 1024, 1,    0, 1.65, 2.00 },
	{ "GD25LD10C",          0xc8, 0x60110000, 64 * 1024, 2,    0, 1.65, 2.00 },
	{ "GD25LD20C",          0xc8, 0x60120000, 64 * 1024, 4,    0, 1.65, 2.00 },
	{ "GD25LD40C",          0xc8, 0x60130000, 64 * 1024, 8,    0, 1.65, 2.00 },
	{ "GD25LQ80C",          0xc8, 0x60140000, 64 * 1024, 16,   0, 1.65, 2.10 },
	{ "GD25LQ16C",          0xc8, 0x60150000, 64 * 1024, 32,   0, 1.65, 2.10 },
	{ "GD25LQ32E",          0xc8, 0x60160000, 64 * 1024, 64,   0, 1.65, 2.10 },
	{ "GD25LQ64E",          0xc8, 0x60170000, 64 * 1024, 128,  0, 1.65, 2.00 },
	{ "GD25LQ128",          0xc8, 0x6018c840, 64 * 1024, 256,  0, 1.65, 2.00 },
	{ "GD25LQ256D",         0xc8, 0x60190000, 64 * 1024, 512,  1, 1.65, 2.00 },
	{ "GD25WD05C",          0xc8, 0x64100000, 64 * 1024, 1,    0, 1.65, 3.60 },
	{ "GD25WD10C",          0xc8, 0x64110000, 64 * 1024, 2,    0, 1.65, 3.60 },
	{ "GD25WD20E",          0xc8, 0x64120000, 64 * 1024, 4,    0, 1.65, 3.60 },
	{ "GD25WD40E",          0xc8, 0x64130000, 64 * 1024, 8,    0, 1.65, 3.60 },
	{ "GD25WD80C",          0xc8, 0x64140000, 64 * 1024, 16,   0, 1.65, 3.60 },
	{ "GD25WQ20E",          0xc8, 0x65120000, 64 * 1024, 4,    0, 1.65, 3.60 },
	{ "GD25WQ40E",          0xc8, 0x65130000, 64 * 1024, 8,    0, 1.65, 3.60 },
	{ "GD25WQ80E",          0xc8, 0x65140000, 64 * 1024, 16,   0, 1.65, 3.60 },
	{ "GD25WQ16E",          0xc8, 0x65150000, 64 * 1024, 32,   0, 1.65, 3.60 },
	{ "GD25WQ32E",          0xc8, 0x65160000, 64 * 1024, 64,   0, 1.65, 3.60 },
	{ "GD25WQ64E",          0xc8, 0x65170000, 64 * 1024, 128,  0, 1.65, 3.60 },
	{ "GD25WQ128E",         0xc8, 0x65180000, 64 * 1024, 256,  0, 1.65, 3.60 },
	{ "GD25WB256E",         0xc8, 0x65190000, 64 * 1024, 512,  1, 1.65, 3.60 },
	{ "GD25LB512ME",        0xc8, 0x671a0000, 64 * 1024, 1024, 1, 1.65, 2.00 },
// SPI_FLASH GigaDevice --> https://www.gigadevice.com/product/flash/product-series/spi-nor-flash
  	{ "YC25Q128",           0xd8, 0x4018c840, 64 * 1024, 256,  0, 2.70, 3.60 },
// SPI_FLASH PARAGON  --> 
  	{ "PN25F08",            0xe0, 0x40140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "PN25F16",            0xe0, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "PN25F32",            0xe0, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "PN25F64",            0xe0, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "PN25F128",           0xe0, 0x40180000, 64 * 1024, 256,  0, 2.70, 3.60 },
// SPI_FLASH WINBOND --> https://www.winbond.com/hq/product/code-storage-flash-memory/serial-nor-flash/?locale=en&selected=32Mb#Density
  	{ "W25P80",             0xef, 0x20140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "W25P16",             0xef, 0x20150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "W25P32",             0xef, 0x20160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "W25X05",             0xef, 0x30100000, 64 * 1024, 1,    0, 2.30, 3.60 },
	{ "W25X10",             0xef, 0x30110000, 64 * 1024, 2,    0, 2.70, 3.60 },
	{ "W25X20",             0xef, 0x30120000, 64 * 1024, 4,    0, 2.70, 3.60 },
	{ "W25X40",             0xef, 0x30130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "W25X80",             0xef, 0x30140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "W25X16",             0xef, 0x30150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "W25X32VS",           0xef, 0x30160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "W25X64",             0xef, 0x30170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "W25Q20CL",           0xef, 0x40120000, 64 * 1024, 4,    0, 2.30, 3.60 },
	{ "W25Q40BV",           0xef, 0x40130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "W25Q80BL",           0xef, 0x40140000, 64 * 1024, 16,   0, 2.30, 3.60 },
	{ "W25Q16DV",           0xef, 0x40150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "W25Q32BV",           0xef, 0x40160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "W25Q64BV",           0xef, 0x40170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "W25Q128BV",          0xef, 0x40180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "W25Q256FV",          0xef, 0x40190000, 64 * 1024, 512,  1, 2.70, 3.60 },
	{ "W25Q20BW",           0xef, 0x50120000, 64 * 1024, 4,    0, 1.65, 1.95 },
	{ "W25Q80",             0xef, 0x50140000, 64 * 1024, 16,   0, 2.30, 3.60 },
	{ "W25Q10EW",           0xef, 0x60110000, 64 * 1024, 2,    0, 1.65, 1.95 },
	{ "W25Q20EW",           0xef, 0x60120000, 64 * 1024, 4,    0, 1.65, 1.95 },
	{ "W25Q40EW",           0xef, 0x60130000, 64 * 1024, 8,    0, 1.65, 1.95 },
	{ "W25Q80EW",           0xef, 0x60140000, 64 * 1024, 16,   0, 1.65, 1.95 },
	{ "W25Q16JW",           0xef, 0x60150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "W25Q32FW",           0xef, 0x60160000, 64 * 1024, 64,   0, 1.65, 1.95 },
	{ "W25Q64DW",           0xef, 0x60170000, 64 * 1024, 128,  0, 1.70, 1.95 },
	{ "W25Q128FW",          0xef, 0x60180000, 64 * 1024, 256,  0, 1.65, 1.95 },
	{ "W25Q256JW",          0xef, 0x60190000, 64 * 1024, 512,  1, 1.70, 1.95 },
	{ "W25M512JW",          0xef, 0x61190000, 64 * 1024, 1024, 1, 1.70, 1.95 },
	{ "W25Q512JV",          0xef, 0x70200000, 64 * 1024, 1024, 1, 2.70, 3.60 },
	{ "W25M512JV",          0xef, 0x71190000, 64 * 1024, 1024, 1, 2.70, 3.60 },
	{ "W25Q32JW",           0xef, 0x80160000, 64 * 1024, 64,   0, 1.70, 1.95 },
// SPI_FLASH Fidelix --> http://www.fidelix.co.kr/pages/sub223_en.php
  	{ "FM25Q04A",           0xf8, 0x32130000, 64 * 1024, 8,    0, 2.70, 3.60 },
	{ "FM25Q08A",           0xf8, 0x32140000, 64 * 1024, 16,   0, 2.70, 3.60 },
	{ "FM25Q16A",           0xf8, 0x32150000, 64 * 1024, 32,   0, 2.70, 3.60 },
	{ "FM25Q32A",           0xf8, 0x32160000, 64 * 1024, 64,   0, 2.70, 3.60 },
	{ "FM25Q64A",           0xf8, 0x32170000, 64 * 1024, 128,  0, 2.70, 3.60 },
	{ "FM25Q128A",          0xf8, 0x32180000, 64 * 1024, 256,  0, 2.70, 3.60 },
	{ "FM25M04A",           0xf8, 0x42130000, 64 * 1024, 8,    0, 1.65, 1.95 },
	{ "FM25M08A",           0xf8, 0x42140000, 64 * 1024, 16,   0, 1.65, 1.95 },
	{ "FM25M16A",           0xf8, 0x42150000, 64 * 1024, 32,   0, 1.65, 1.95 },
	{ "FM25M32B",           0xf8, 0x42160000, 64 * 1024, 64,   0, 1.65, 1.95 },
	{ "FM25M64A",           0xf8, 0x42170000, 64 * 1024, 128,  0, 1.65, 1.95 }
};

/*
 * read SPI flash device ID
 */
static int snor_read_devid(u8 *rxbuf, int n_rx)
{
	int retval = 0;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_RDID);

	retval = SPI_CONTROLLER_Read_NByte(rxbuf, n_rx, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return retval;
	}

	return 0;
}

/*
 * read status register
 */
static int snor_read_sr(u8 *val)
{
	int retval = 0;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_RDSR);

	retval = SPI_CONTROLLER_Read_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return retval;
	}

	return 0;
}

/*
 * write status register
 */
static int snor_write_sr(u8 *val)
{
	int retval = 0;

	SPI_CONTROLLER_Chip_Select_Low();
	SPI_CONTROLLER_Write_One_Byte(OPCODE_WRSR);

	retval = SPI_CONTROLLER_Write_NByte(val, 1, SPI_CONTROLLER_SPEED_SINGLE);
	SPI_CONTROLLER_Chip_Select_High();
	if (retval) {
		printf("%s: ret: %x\n", __func__, retval);
		return retval;
	}
	return 0;
}

struct chip_info *chip_prob(void)
{
	struct chip_info *info = NULL, *match = NULL;
	u8 buf[5];
	u32 jedec, jedec_strip, weight;
	int i;

	snor_read_devid(buf, 5);
	jedec = (u32)((u32)(buf[1] << 24) | ((u32)buf[2] << 16) | ((u32)buf[3] <<8) | (u32)buf[4]);
	jedec_strip = jedec & 0xffff0000;

	printf("spi device id: %x %x %x %x %x (%x)\n", buf[0], buf[1], buf[2], buf[3], buf[4], jedec);

	// FIXME, assign default as AT25D
	weight = 0xffffffff;
	match = &chips_data[0];
	for (i = 0; i < sizeof(chips_data)/sizeof(chips_data[0]); i++) {
		info = &chips_data[i];
		if (info->id == buf[0]) {
			if ((info->jedec_id == jedec) || ((info->jedec_id & 0xffff0000) == jedec_strip)) {
				printf("Detected SPI NOR Flash:\e[93m %s\e[0m, Flash Size:\e[93m %ld \e[0mMB\n", info->name, (info->sector_size * info->n_sectors) >> 20);
				printf ("VCC: %2.2f ... %2.2fV\n", info->vcc_min, info->vcc_max);
                if (info->vcc_max < 3.0) printf ("Please use the 1.8V adapter!\n");
				return info;
			}

			if (weight > (info->jedec_id ^ jedec)) {
				weight = info->jedec_id ^ jedec;
				match = info;
			}
		}
	}
	printf("SPI NOR Flash Not Detected!\n");
	match = NULL; /* Not support JEDEC calculate info */

	return match;
}

long snor_init(void)
{
	spi_chip_info = chip_prob();

	if(spi_chip_info == NULL)
		return -1;

	bsize = spi_chip_info->sector_size;

	return spi_chip_info->sector_size * spi_chip_info->n_sectors;
}

int snor_erase(unsigned long offs, unsigned long len)
{
	unsigned long plen = len;
	snor_dbg("%s: offs:%x len:%x\n", __func__, offs, len);

	/* sanity checks */
	if (len == 0)
		return -1;

	if(!offs && len == (spi_chip_info->sector_size * spi_chip_info->n_sectors))
	{
		printf("Please Wait......\n");
		return full_erase_chip();
	}

	timer_start();

	snor_unprotect();

	/* now erase those sectors */
	while (len > 0) {
		if (snor_erase_sector(offs)) {
			return -1;
		}

		offs += spi_chip_info->sector_size;
		len -= spi_chip_info->sector_size;
		if( timer_progress() )
		{
			printf("\bErase %ld%% [%lu] of [%lu] bytes      ", 100 * (plen - len) / plen, plen - len, plen);
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
			fflush(stdout);
		}
	}
	printf("Erase 100%% [%lu] of [%lu] bytes      \n", plen - len, plen);
	timer_end();

	return 0;
}

int snor_read(unsigned char *buf, unsigned long from, unsigned long len)
{
	u32 read_addr, physical_read_addr, remain_len, data_offset;

	snor_dbg("%s: from:%x len:%x \n", __func__, from, len);

	/* sanity checks */
	if (len == 0)
		return 0;

	timer_start();
	/* Wait till previous write/erase is done. */
	if (snor_wait_ready(1)) {
		/* REVISIT status return?? */
		return -1;
	}

	read_addr = from;
	remain_len = len;

	while(remain_len > 0) {

		physical_read_addr = read_addr;
		data_offset = (physical_read_addr % (spi_chip_info->sector_size));

		if (spi_chip_info->addr4b)
			snor_4byte_mode(1);

		SPI_CONTROLLER_Chip_Select_Low();

		/* Set up the write data buffer. */
		SPI_CONTROLLER_Write_One_Byte(OPCODE_READ);

		if (spi_chip_info->addr4b)
			SPI_CONTROLLER_Write_One_Byte((physical_read_addr >> 24) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((physical_read_addr >> 16) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((physical_read_addr >> 8) & 0xff);
		SPI_CONTROLLER_Write_One_Byte(physical_read_addr & 0xff);

		if( (data_offset + remain_len) < spi_chip_info->sector_size )
		{
			if(SPI_CONTROLLER_Read_NByte(&buf[len - remain_len], remain_len, SPI_CONTROLLER_SPEED_SINGLE)) {
				SPI_CONTROLLER_Chip_Select_High();
				if (spi_chip_info->addr4b)
					snor_4byte_mode(0);
				len = -1;
				break;
			}
			remain_len = 0;
		} else {
			if(SPI_CONTROLLER_Read_NByte(&buf[len - remain_len], spi_chip_info->sector_size - data_offset, SPI_CONTROLLER_SPEED_SINGLE)) {
				SPI_CONTROLLER_Chip_Select_High();
				if (spi_chip_info->addr4b)
					snor_4byte_mode(0);
				len = -1;
				break;
			}
			remain_len -= spi_chip_info->sector_size - data_offset;
			read_addr += spi_chip_info->sector_size - data_offset;
			if( timer_progress() ) {
				printf("\bRead %ld%% [%lu] of [%lu] bytes      ", 100 * (len - remain_len) / len, len - remain_len, len);
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				fflush(stdout);
			}
		}

		SPI_CONTROLLER_Chip_Select_High();

		if (spi_chip_info->addr4b)
			snor_4byte_mode(0);
	}
	printf("Read 100%% [%lu] of [%lu] bytes      \n", len - remain_len, len);
	timer_end();

	return len;
}

int snor_write(unsigned char *buf, unsigned long to, unsigned long len)
{
	u32 page_offset, page_size;
	int rc = 0, retlen = 0;
	unsigned long plen = len;

	snor_dbg("%s: to:%x len:%x \n", __func__, to, len);

	/* sanity checks */
	if (len == 0)
		return 0;

	if (to + len > spi_chip_info->sector_size * spi_chip_info->n_sectors)
		return -1;

	timer_start();
	/* Wait until finished previous write command. */
	if (snor_wait_ready(2)) {
		return -1;
	}


	/* what page do we start with? */
	page_offset = to % FLASH_PAGESIZE;

	if (spi_chip_info->addr4b)
		snor_4byte_mode(1);

	/* write everything in PAGESIZE chunks */
	while (len > 0) {
		page_size = min(len, FLASH_PAGESIZE - page_offset);
		page_offset = 0;
		/* write the next page to flash */

		snor_wait_ready(3);
		snor_write_enable();
		snor_unprotect();

		SPI_CONTROLLER_Chip_Select_Low();
		/* Set up the opcode in the write buffer. */
		SPI_CONTROLLER_Write_One_Byte(OPCODE_PP);

		if (spi_chip_info->addr4b)
			SPI_CONTROLLER_Write_One_Byte((to >> 24) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((to >> 16) & 0xff);
		SPI_CONTROLLER_Write_One_Byte((to >> 8) & 0xff);
		SPI_CONTROLLER_Write_One_Byte(to & 0xff);

		if(!SPI_CONTROLLER_Write_NByte(buf, page_size, SPI_CONTROLLER_SPEED_SINGLE))
			rc = page_size;
		else
			rc = 1;

		SPI_CONTROLLER_Chip_Select_High();

		snor_dbg("%s: to:%x page_size:%x ret:%x\n", __func__, to, page_size, rc);

		if( timer_progress() ) {
			printf("\bWritten %ld%% [%lu] of [%lu] bytes      ", 100 * (plen - len) / plen, plen - len, plen);
			printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
			fflush(stdout);
		}

		if (rc > 0) {
			retlen += rc;
			if (rc < page_size) {
				printf("%s: rc:%x page_size:%x\n",
						__func__, rc, page_size);
				snor_write_disable();
				return retlen - rc;
			}
		}

		len -= page_size;
		to += page_size;
		buf += page_size;
	}

	if (spi_chip_info->addr4b)
		snor_4byte_mode(0);

	snor_write_disable();

	printf("Written 100%% [%ld] of [%ld] bytes      \n", plen - len, plen);
	timer_end();

	return retlen;
}

void support_snor_list(void)
{
	int i;

	printf("SPI NOR Flash Support List:\n");
	for ( i = 0; i < (sizeof(chips_data)/sizeof(struct chip_info)); i++)
	{
		printf("%03d. %s\n", i + 1, chips_data[i].name);
	}
}
/* End of [spi_nor_flash.c] package */
