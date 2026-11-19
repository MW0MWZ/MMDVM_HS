/*
 *   Copyright (C) 2020 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016 by Jim McLaughlin KI6ZUM
 *   Copyright (C) 2016,2017,2018 by Andy Uribe CA6JAU
 *   Copyright (C) 2017 by Danilo DB4PLE 
 *
 *   Some of the code is based on work of Guus Van Dooren PE1PLM:
 *   https://github.com/ki6zum/gmsk-dstar/blob/master/firmware/dvmega/dvmega.ino
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
 
#if !defined(ADF7021_H)
#define  ADF7021_H

#include "Config.h"

#if defined(ENABLE_ADF7021)

/*
- Most of the registers values are obteined from ADI eval software:
http://www.analog.com/en/products/rf-microwave/integrated-transceivers-transmitters-receivers/low-power-rf-transceivers/adf7021.html
- or ADF7021 datasheet formulas:
www.analog.com/media/en/technical-documentation/data-sheets/ADF7021.pdf
*/

/***** Test modes ****/

// Enable SWD pin to access the demodulator output signal:
// (See application note AN-852 and ADF7021 datasheet, page 60)
// #define TEST_DAC

// Transmit the carrier frequency:
// #define TEST_TX

/*********************/

// Disable TX Raised Cosine filter for 4FSK modulation in ADF7021:
// #define ADF7021_DISABLE_RC_4FSK

// Support for ADF7021-N version: 
// #define ADF7021_N_VER

// AFC (automatic frequency control) on receive, D-Star only: it pulls in
// a received signal up to ADF7021_AFC_RANGE Hz either side of the RX
// frequency. The 4FSK modes (DMR, YSF, P25, NXDN, M17) have none: their
// signals come in bursts (DMR is TDMA), and between them the AFC chases
// noise. A hotspot's frequency error is set by calibrating it with the
// user's radio (MMDVM-Host's RX/TX offsets).

// AGC window (AD7021_GAIN_AUTO_REDUCED), on the RSSI ADC's 0-79 scale: the
// AGC lowers the gain when the RSSI is above ADF7021_AGC_HIGH and raises it
// when below ADF7021_AGC_LOW (the ADF7021's defaults are 70 and 30). Set
// low, it drops to its lowest gain (input IP3 -3 dBm) early, and noise
// between transmissions and between TDMA bursts does not pull it back up:
// a hotspot's radios are close by, and only a weak, distant signal raises
// the gain.
#define ADF7021_AGC_HIGH         40U
#define ADF7021_AGC_LOW          10U

// How far above the noise floor a signal must be to count as one, dB (the
// mode scan starts hunting when it hears one)
#define ADF7021_SIGNAL_MARGIN    10

// AFC pull-in range either side of the RX frequency, in Hz (500 Hz steps)
#define ADF7021_AFC_RANGE        1500

// AFC loop gains, as register values (the datasheet allows KI > 6 and
// KP < 7, and recommends KI = 11, KP = 4)
#define ADF7021_AFC_KI           11U
#define ADF7021_AFC_KP           4U

/****** Support for 14.7456 MHz TCXO (modified RF7021SE boards) ******/
// Correlator K for each demodulator (each crystal's DISC_BW values below are
// worked out from them): the demodulator's centre is K x DEMOD_CLK / (4 x DISC_BW), which
// the RX frequency is set from (ifCentre)
#define ADF7021_K_DSTAR          85U
#define ADF7021_K_4FSK           16U
#define ADF7021_K_NXDN           32U
#define ADF7021_K_POCSAG         22U

#if defined(ADF7021_14_7456)

// R = 4
// DEMOD_CLK = 2.4576 MHz (DSTAR)
// DEMOD_CLK = 4.9152 MHz (DMR, YSF_L, P25)
// DEMOD_CLK = 7.3728 MHz (YSF_H, M17)
// DEMOD CLK = 3.6864 MHz (NXDN)
// DEMOD_CLK = 7.3728 MHz (POCSAG)
#define ADF7021_PFD              3686400.0

// PLL (REG 01)
#define ADF7021_REG1_VHF1        0x021F5041
#define ADF7021_REG1_VHF2        0x021F5041
#define ADF7021_REG1_UHF1        0x00575041
#define ADF7021_REG1_UHF2        0x00535041

// Deviation of modulator (REG 02)
#define ADF7021_DEV_DSTAR        43U
#define ADF7021_DEV_DMR          23U
#define ADF7021_DEV_YSF_L        16U
#define ADF7021_DEV_YSF_H        32U
#if defined(ENABLE_P25_WIDE)
#define ADF7021_DEV_P25          32U
#else
#define ADF7021_DEV_P25          22U
#endif
#define ADF7021_DEV_NXDN         13U
#define ADF7021_DEV_M17          28U
#define ADF7021_DEV_POCSAG       160U

// TX/RX CLOCK register (REG 03)
#define ADF7021_REG3_DSTAR       0x2A4C4193
#if defined(TEST_DAC)
#define ADF7021_REG3_DMR         0x2A4C04D3
#define ADF7021_REG3_YSF_L       0x2A4C04D3
#define ADF7021_REG3_YSF_H       0x2A4C0493
#define ADF7021_REG3_P25         0x2A4C04D3
#define ADF7021_REG3_NXDN        0x2A4C04D3
#define ADF7021_REG3_M17         0x2A4C04D3
#else
#define ADF7021_REG3_DMR         0x2A4C80D3
#define ADF7021_REG3_YSF_L       0x2A4C80D3
#define ADF7021_REG3_YSF_H       0x2A4CC093
#define ADF7021_REG3_P25         0x2A4C80D3
#define ADF7021_REG3_NXDN        0x2A4CC113
#define ADF7021_REG3_M17         0x2A4CC093
#endif
#define ADF7021_REG3_POCSAG      0x2A4F0093

// Discriminator bandwith, demodulator (REG 04)
// Bug in ADI evaluation software, use datasheet formula (4FSK)
#define ADF7021_DISC_BW_DSTAR    522U // K=85
// 4FSK correlator K=16, twice the bandwidth of the datasheet's K (32 at most,
// 28 for YSF_H): about twice the frequency tolerance (+-0.8 x the inner
// deviation instead of +-0.4) for 1-2 dB of sensitivity, the right trade for a
// hotspot. Not NXDN: at half the symbol rate its ideal K is about 68, so 32
// is already twice as wide, and 16 does not decode. DISC_BW = DEMOD_CLK x K / 400 kHz (K=32/28 values: 393, 393, 516,
// 394, 295, 590).
#define ADF7021_DISC_BW_DMR      197U // K=16
#define ADF7021_DISC_BW_YSF_L    197U // K=16
#define ADF7021_DISC_BW_YSF_H    295U // K=16
#define ADF7021_DISC_BW_P25      197U // K=16
#define ADF7021_DISC_BW_NXDN     295U // K=32: NXDN, the narrowest mode, is already twice its ideal K
#define ADF7021_DISC_BW_M17      295U // K=16
#define ADF7021_DISC_BW_POCSAG   406U // K=22

// Post demodulator bandwith (REG 04)
#define ADF7021_POST_BW_DSTAR    10U
#define ADF7021_POST_BW_DMR      80U
#define ADF7021_POST_BW_YSF      20U
#define ADF7021_POST_BW_P25      6U
#define ADF7021_POST_BW_NXDN     7U
#define ADF7021_POST_BW_M17      7U	// Test
#define ADF7021_POST_BW_POCSAG   1U

// IF filter (REG 05)
#define ADF7021_REG5             0x000024F5

// IF CAL (fine cal, defaults) (REG 06)
#define ADF7021_REG6             0x05070E16

// AFC (REG 10): AFC_SCALING_FACTOR = round(2^24 * 500 / XTAL)
#define ADF7021_XTAL             14745600.0
#define ADF7021_AFC_SCALING      569U

/****** Support for 12.2880 MHz TCXO ******/
#elif defined(ADF7021_12_2880)

// R = 2
// DEMOD_CLK = 2.4576 MHz (DSTAR)
// DEMOD_CLK = 6.1440 MHz (DMR, YSF_H, YSF_L, P25, M17)
// DEMOD_CLK = 3.0720 MHz (NXDN)
// DEMOD_CLK = 6.1440 MHz (POCSAG)
#define ADF7021_PFD              6144000.0

// PLL (REG 01)
#define ADF7021_REG1_VHF1        0x021F5021
#define ADF7021_REG1_VHF2        0x021F5021
#define ADF7021_REG1_UHF1        0x00575021
#define ADF7021_REG1_UHF2        0x00535021

// Deviation of modulator (REG 02)
#define ADF7021_DEV_DSTAR        26U
#define ADF7021_DEV_DMR          14U
#define ADF7021_DEV_YSF_L        10U
#define ADF7021_DEV_YSF_H        19U
#if defined(ENABLE_P25_WIDE)
#define ADF7021_DEV_P25          19U
#else
#define ADF7021_DEV_P25          13U
#endif
#define ADF7021_DEV_NXDN         8U
#define ADF7021_DEV_M17          17U
#define ADF7021_DEV_POCSAG       96U

// TX/RX CLOCK register (REG 03)
#define ADF7021_REG3_DSTAR       0x29EC4153
#if defined(TEST_DAC)
#define ADF7021_REG3_DMR         0x29EC0493
#define ADF7021_REG3_YSF_L       0x29EC0493
#define ADF7021_REG3_YSF_H       0x29EC0493
#define ADF7021_REG3_P25         0x29EC0493
#define ADF7021_REG3_NXDN        0x29EC0493
#define ADF7021_REG3_M17         0x29EC0493
#else
#define ADF7021_REG3_DMR         0x29ECA093
#define ADF7021_REG3_YSF_L       0x29ECA093
#define ADF7021_REG3_YSF_H       0x29ECA093
#define ADF7021_REG3_P25         0x29ECA093
#define ADF7021_REG3_NXDN        0x29ECA113
#define ADF7021_REG3_M17         0x29ECA093
#endif
#define ADF7021_REG3_POCSAG      0x29EE8093

// Discriminator bandwith, demodulator (REG 04)
// Bug in ADI evaluation software, use datasheet formula (4FSK)
#define ADF7021_DISC_BW_DSTAR    522U // K=85
// 4FSK correlator K=16 (see the 14.7456 MHz section; K=32/28 values: 491,
// 491, 430, 493, 246, 492)
#define ADF7021_DISC_BW_DMR      246U // K=16
#define ADF7021_DISC_BW_YSF_L    246U // K=16
#define ADF7021_DISC_BW_YSF_H    246U // K=16
#define ADF7021_DISC_BW_P25      246U // K=16
#define ADF7021_DISC_BW_NXDN     246U // K=32 (see the 14.7456 MHz section)
#define ADF7021_DISC_BW_M17      246U // K=16
#define ADF7021_DISC_BW_POCSAG   338U // K=22

// Post demodulator bandwith (REG 04)
#define ADF7021_POST_BW_DSTAR    10U
#define ADF7021_POST_BW_DMR      80U
#define ADF7021_POST_BW_YSF      20U
#define ADF7021_POST_BW_P25      6U
#define ADF7021_POST_BW_NXDN     8U
#define ADF7021_POST_BW_M17      8U	// Test
#define ADF7021_POST_BW_POCSAG   1U

// IF filter (REG 05)
#define ADF7021_REG5             0x00001ED5

// IF CAL (fine cal, defaults) (REG 06)
#define ADF7021_REG6             0x0505EBB6

// AFC (REG 10): AFC_SCALING_FACTOR = round(2^24 * 500 / XTAL)
#define ADF7021_XTAL             12288000.0
#define ADF7021_AFC_SCALING      683U

#endif

// Slicer threshold for 4FSK demodulator (REG 13). It scales with the
// correlator's K: 78 x (outer deviation x K) / 100 kHz. These are for K=16,
// half the values for the datasheet's K=32 (YSF_H 16/28 of its K=28 value).
#if defined(ADF7021_N_VER)

#define ADF7021_SLICER_TH_DSTAR  0U
#define ADF7021_SLICER_TH_DMR    26U  // K=32: 51
#define ADF7021_SLICER_TH_YSF_L  18U  // K=32: 35
#define ADF7021_SLICER_TH_YSF_H  39U  // K=28: 69
#define ADF7021_SLICER_TH_P25    22U  // K=32: 43
#define ADF7021_SLICER_TH_NXDN   26U  // K=32 (NXDN stays at K=32)
#define ADF7021_SLICER_TH_M17    30U  // K=32: 59

#else

#define ADF7021_SLICER_TH_DSTAR  0U
#define ADF7021_SLICER_TH_DMR    29U  // K=32: 57
#define ADF7021_SLICER_TH_YSF_L  19U  // K=32: 38
#define ADF7021_SLICER_TH_YSF_H  43U  // K=28: 75
#define ADF7021_SLICER_TH_P25    24U  // K=32: 47
#define ADF7021_SLICER_TH_NXDN   26U  // K=32 (NXDN stays at K=32)
#define ADF7021_SLICER_TH_M17    30U  // K=32: 59

#endif

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

void Send_AD7021_control(bool doSle = true);
#if defined(DUPLEX)
void Send_AD7021_control2(bool doSle = true);
#endif

#if defined(ADF7021_DISABLE_RC_4FSK)
#define ADF7021_EVEN_BIT  true
#else
#define ADF7021_EVEN_BIT  false
#endif

#endif

#endif
