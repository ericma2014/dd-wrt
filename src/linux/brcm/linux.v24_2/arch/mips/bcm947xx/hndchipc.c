/*
 * BCM47XX support code for some chipcommon facilities (uart, jtagm)
 *
 * Copyright 2007, Broadcom Corporation
 * All Rights Reserved.
 * 
 * THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
 * KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
 * SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.
 *
 * $Id$
 */

#include <typedefs.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <sbutils.h>
#include <bcmdevs.h>
#include <bcmnvram.h>
#include <sbconfig.h>
#include <sbchipc.h>
#include <hndchipc.h>
#include <hndcpu.h>

/* debug/trace */
#define	CC_ERROR(args)

#ifdef BCMDBG
#define	CC_MSG(args)	printf args
#else
#define	CC_MSG(args)
#endif /* BCMDBG */

/* interested chipcommon interrupt source
 *  - GPIO
 *  - EXTIF
 *  - ECI
 *  - PMU
 *  - UART
 */
#define	MAX_CC_INT_SOURCE 5

/* chipc secondary isr info */
typedef struct
{
  uint intmask;			/* int mask */
  cc_isr_fn isr;		/* secondary isr handler */
  void *cbdata;			/* pointer to private data */
} cc_isr_info_t;

static cc_isr_info_t cc_isr_desc[MAX_CC_INT_SOURCE];

/* chip common intmask */
static uint32 cc_intmask = 0;

static bool
BCMINITFN(serial_exists)(osl_t *osh, uint8 *regs)
{
	uint8 save_mcr, status1;

	save_mcr = R_REG(osh, &regs[UART_MCR]);
	W_REG(osh, &regs[UART_MCR], UART_MCR_LOOP | 0x0a);
	status1 = R_REG(osh, &regs[UART_MSR]) & 0xf0;
	W_REG(osh, &regs[UART_MCR], save_mcr);

	return (status1 == 0x90);
}

/*
 * Initializes UART access. The callback function will be called once
 * per found UART.
 */
void BCMINITFN (sb_serial_init) (sb_t * sbh, sb_serial_init_fn add)
{
	osl_t *osh;
	void *regs;
	ulong base;
	uint irq;
	int i, n;

	osh = sb_osh(sbh);

	if ((regs = sb_setcore(sbh, SB_EXTIF, 0))) {
		extifregs_t *eir = (extifregs_t *) regs;
		sbconfig_t *sb;

		/* Determine external UART register base */
		sb = (sbconfig_t *)((ulong) eir + SBCONFIGOFF);
		base = EXTIF_CFGIF_BASE(sb_base(R_REG(osh, &sb->sbadmatch1)));

		/* Determine IRQ */
		irq = sb_irq(sbh);

		/* Disable GPIO interrupt initially */
		W_REG(osh, &eir->gpiointpolarity, 0);
		W_REG(osh, &eir->gpiointmask, 0);

		/* Search for external UARTs */
		n = 2;
		for (i = 0; i < 2; i++) {
			regs = (void *) REG_MAP(base + (i * 8), 8);
			if (serial_exists(osh, regs)) {
				/* Set GPIO 1 to be the external UART IRQ */
				W_REG(osh, &eir->gpiointmask, 2);
				/* XXXDetermine external UART clock */
				if (add)
					add(regs, irq, 13500000, 0);
			}
		}

		/* Add internal UART if enabled */
		if (R_REG(osh, &eir->corecontrol) & CC_UE)
			if (add)
				add((void *) &eir->uartdata, irq, sb_clock(sbh), 2);
	} else if ((regs = sb_setcore(sbh, SB_CC, 0))) {
		chipcregs_t *cc = (chipcregs_t *) regs;
		uint32 rev, cap, pll, baud_base, div;

		/* Default value */
		div = 48;

		/* Determine core revision and capabilities */
		rev = sb_corerev(sbh);
		cap = R_REG(osh, &cc->capabilities);
		pll = cap & CC_CAP_PLL_MASK;
		printk(KERN_INFO "core revision %d\n", rev);
		/* Determine IRQ */
		irq = sb_irq(sbh);

		if (pll == PLL_TYPE1) {
			/* PLL clock */
			baud_base = sb_clock_rate(pll,
			                          R_REG(osh, &cc->clockcontrol_n),
			                          R_REG(osh, &cc->clockcontrol_m2));
			div = 1;
		} else {
			/* 5354 chip common uart uses a constant clock
			 * frequency of 25MHz */
			if (sb_corerev(sbh) == 20) {
				/* Set the override bit so we don't divide it */
				W_REG(osh, &cc->corecontrol, CC_UARTCLKO);
				baud_base = 25000000;
			} else if (rev >= 11 && rev != 15) {
				/* Fixed ALP clock */
				baud_base = sb_alp_clock(sbh);
				div = 1;
				/* Set the override bit so we don't divide it */
				W_REG(osh, &cc->corecontrol, CC_UARTCLKO);
			} else if (rev >= 3) {
				/* Internal backplane clock */
				baud_base = sb_clock(sbh);
				div = 2;	/* Minimum divisor */
				W_REG(osh, &cc->clkdiv,
				      ((R_REG(osh, &cc->clkdiv) & ~CLKD_UART) | div));
			} else {
				/* Fixed internal backplane clock */
				baud_base = 88000000;
				div = 48;
			}

			/* Clock source depends on strapping if UartClkOverride is unset */
			if ((rev > 0) &&
			    ((R_REG(osh, &cc->corecontrol) & CC_UARTCLKO) == 0)) {
				if ((cap & CC_CAP_UCLKSEL) == CC_CAP_UINTCLK) {
					/* Internal divided backplane clock */
					baud_base /= div;
				} else {
					/* Assume external clock of 1.8432 MHz */
					baud_base = 1843200;
				}
			}
		}

		/* Add internal UARTs */
		n = cap & CC_CAP_UARTS_MASK;
		for (i = 0; i < n; i++) {
			/* Register offset changed after revision 0 */
			printk(KERN_INFO "add uart%d at %X\n", i, &cc->uart0data + (i * 256));
			if (rev)
				regs = (void *)((ulong) &cc->uart0data + (i * 256));
			else
				regs = (void *)((ulong) &cc->uart0data + (i * 8));

			if (add)
				add(regs, irq, baud_base, 0);
		}
	}
}

