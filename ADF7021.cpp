/*
 *   Copyright (C) 2020,2021 by Jonathan Naylor G4KLX
 *   Copyright (C) 2016 by Jim McLaughlin KI6ZUM
 *   Copyright (C) 2016,2017,2018,2019,2020 by Andy Uribe CA6JAU
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

#include "Config.h"

#if defined(ENABLE_ADF7021)

#include "Globals.h"
#include "IO.h"
#include "Debug.h"
#include "ADF7021.h"
#include <math.h>

volatile bool totx_request = false;
volatile bool torx_request = false;
volatile bool even = true;
static uint32_t last_clk = 2U;

volatile uint32_t  AD7021_control_word;

uint32_t           ADF7021_RX_REG0;
uint32_t           ADF7021_TX_REG0;
uint32_t           ADF7021_REG1;
uint32_t           div2;
uint32_t           f_div;

// The demodulator's centre frequency (its IF), Hz: K x DEMOD_CLK / (4 x
// DISC_BW), DEMOD_CLK being the crystal over REG3's DEMOD_CLK_DIVIDE (bits
// 9:6). DISC_BW is a whole number, so the centre is not exactly 100 kHz:
// 99.80 kHz for DMR, P25 and YSF (low deviation) at K=16 on a 14.7456 MHz
// board. The RX LO is set this far below the RX frequency, so each mode
// receives centred and calibration finds the board's true frequency error.
static uint32_t demodCentre(uint32_t reg3, uint32_t discBW, uint32_t k)
{
  uint32_t div = (reg3 >> 6) & 0x0FU;
  if (div == 0U || discBW == 0U)
    return 100000U;
  return (uint32_t)((double)k * (ADF7021_XTAL / (double)div) / (4.0 * (double)discBW) + 0.5);
}

uint32_t CIO::ifCentre(MMDVM_STATE modemState)
{
  switch (modemState) {
    case STATE_DSTAR:
      return demodCentre(ADF7021_REG3_DSTAR, ADF7021_DISC_BW_DSTAR, ADF7021_K_DSTAR);
    case STATE_DMR:
      return demodCentre(ADF7021_REG3_DMR, ADF7021_DISC_BW_DMR, ADF7021_K_4FSK);
    case STATE_YSF:
      return m_LoDevYSF ? demodCentre(ADF7021_REG3_YSF_L, ADF7021_DISC_BW_YSF_L, ADF7021_K_4FSK)
                        : demodCentre(ADF7021_REG3_YSF_H, ADF7021_DISC_BW_YSF_H, ADF7021_K_4FSK);
    case STATE_P25:
      return demodCentre(ADF7021_REG3_P25, ADF7021_DISC_BW_P25, ADF7021_K_4FSK);
    case STATE_NXDN:
      return demodCentre(ADF7021_REG3_NXDN, ADF7021_DISC_BW_NXDN, ADF7021_K_NXDN);
    case STATE_M17:
      return demodCentre(ADF7021_REG3_M17, ADF7021_DISC_BW_M17, ADF7021_K_4FSK);
    default:
      return 100000U;     // TX only (POCSAG, CW ID), calibration states
  }
}

// AFC (REG 10). MAX_AFC_RANGE is the total span the AFC may pull, in 500 Hz
// steps, which the RF divide-by-2 halves (so is doubled when it is on).
static uint32_t afcControl(bool on)
{
  uint32_t range = ((2U * ADF7021_AFC_RANGE) / 500U) * f_div;
  if (range > 255U)
    range = 255U;

  uint32_t reg = 0x0AU;                               // register 10
  if (on)
    reg |= 1U << 4;                                   // AFC_EN
  reg |= (uint32_t) ADF7021_AFC_SCALING   << 5;       // AFC_SCALING_FACTOR
  reg |= (uint32_t) ADF7021_AFC_KI        << 17;      // KI
  reg |= (uint32_t) ADF7021_AFC_KP        << 21;      // KP
  reg |= range                            << 24;      // MAX_AFC_RANGE

  return reg;
}

uint16_t           m_dstarDev;
uint16_t           m_dmrDev;
uint16_t           m_ysfDev;
uint16_t           m_p25Dev;
uint16_t           m_nxdnDev;
uint16_t           m_m17Dev;
uint16_t           m_pocsagDev;

static void Send_AD7021_control_shift()
{
  for (int AD7021_counter = 31; AD7021_counter >= 0; AD7021_counter--) {
    if (bitRead(AD7021_control_word, AD7021_counter) == HIGH)
      io.SDATA_pin(HIGH);
    else
      io.SDATA_pin(LOW);

    io.dlybit();
    io.SCLK_pin(HIGH);
    io.dlybit();
    io.SCLK_pin(LOW);
  }

  // to keep SDATA signal at defined level when idle (not required)
  io.SDATA_pin(LOW);
}

static void Send_AD7021_control_slePulse()
{
  io.SLE_pin(HIGH);
  io.dlybit();
  io.SLE_pin(LOW);
}

void Send_AD7021_control(bool doSle)
{
  Send_AD7021_control_shift();

  if (doSle)
    Send_AD7021_control_slePulse();
}

#if defined(DUPLEX)
static  void Send_AD7021_control_sle2Pulse()
{
  io.SLE2_pin(HIGH);
  io.dlybit();
  io.SLE2_pin(LOW);
}

void Send_AD7021_control2(bool doSle)
{
  Send_AD7021_control_shift();

  if (doSle)
    Send_AD7021_control_sle2Pulse();
}
#endif

// Read a 16 bit word back from the receiving ADF7021 (REG 7, readback)
uint16_t CIO::readBack(uint16_t control)
{
  uint16_t RB_word = 0U;

  // Send control register
  for (int AD7021_counter = 8; AD7021_counter >= 0; AD7021_counter--) {
    if (bitRead(control, AD7021_counter) == HIGH)
      SDATA_pin(HIGH);
    else
      SDATA_pin(LOW);

    dlybit();
    SCLK_pin(HIGH);
    dlybit();
    SCLK_pin(LOW);
  }

  SDATA_pin(LOW);

#if defined(DUPLEX)
  if (m_duplex || m_calState == STATE_RSSICAL)
    SLE2_pin(HIGH);
  else
    SLE_pin(HIGH);
#else
  SLE_pin(HIGH);
#endif

  dlybit();

  // Read SREAD pin
  for (int AD7021_counter = 17; AD7021_counter >= 0; AD7021_counter--) {
    SCLK_pin(HIGH);
    dlybit();

    if ((AD7021_counter != 17) && (AD7021_counter != 0))
      RB_word |= ((SREAD_pin() & 0x01) << (AD7021_counter - 1));

    SCLK_pin(LOW);
    dlybit();
  }

#if defined(DUPLEX)
  if (m_duplex || m_calState == STATE_RSSICAL)
    SLE2_pin(LOW);
  else
    SLE_pin(LOW);
#else
  SLE_pin(LOW);
#endif

  return RB_word;
}

// RSSI (-dBm, larger is weaker) of the receiving ADF7021
uint16_t CIO::readRSSI()
{
  uint8_t RB_code, gain_code, gain_corr;

  // Register 7, readback enable, ADC RSSI mode
  uint16_t RB_word = readBack(0x0147U);

  // Process RSSI code
  RB_code = RB_word & 0x7f;
  gain_code = (RB_word >> 7) & 0x0f;

  switch(gain_code) {
    case 0b1010:
      gain_corr = 0U;
      break;
    case 0b0110:
      gain_corr = 24U;
      break;
    case 0b0101:
      gain_corr = 38U;
      break;
    case 0b0100:
      gain_corr = 58U;
      break;
    case 0b0000:
      gain_corr = 86U;
      break;
    default:
      gain_corr = 0U;
      break;
  }

  return ( 130 - (RB_code + gain_corr)/2 );

}

// A register 0 for a TX/RX switch has been shifted in and waits for the
// interrupt to latch it: the shared serial lines must not be used until then.
static bool latchPending()
{
  return totx_request || torx_request;
}

// The receiving ADF7021's RSSI now, for a frame's report: last when a TX/RX
// switch is waiting to latch (the shared serial lines are busy).
uint16_t CIO::sampleRSSI(uint16_t last)
{
  if (latchPending() && last != 0U)
    return last;
  return readRSSI();
}

// Is someone transmitting? The receiving ADF7021's signal level (RSSI) is
// polled every SIGNAL_POLL_BITS received bits against a noise floor that
// is followed while nothing is decoded: ADF7021_SIGNAL_MARGIN dB above it
// is a signal. The mode scan uses it to start hunting for the mode at once.
#define SIGNAL_POLL_BITS   12U

static uint32_t signal_poll_bits = 0U;
static int16_t  signal_floor     = 0;      // noise floor, dBm x 16
static bool     signal_floor_set = false;
static bool     signal_on        = false;

bool CIO::signalPresent()
{
  return signal_on;
}

void CIO::signalPoll(bool decoding)
{
  if (m_tx && !m_duplex)
    return;

  if (++signal_poll_bits < SIGNAL_POLL_BITS || latchPending())
    return;
  signal_poll_bits = 0U;

  int16_t level = -(int16_t) readRSSI();
  if (!signal_floor_set) {
    signal_floor     = level * 16;
    signal_floor_set = true;
  }
  signal_on = level * 16 >= signal_floor + ADF7021_SIGNAL_MARGIN * 16;

  // Follow the noise floor
  if (!decoding && !signal_on)
    signal_floor += (level * 16 - signal_floor) / 16;
}

// AGC (REG 9), as the board's config selects; the same for both ADF7021s
// of a duplex board.
static uint32_t agcControl()
{
#if defined(AD7021_GAIN_AUTO)
  return 0x000231E9U;   // AGC ON, normal operation
#elif defined(AD7021_GAIN_AUTO_LIN)
  return 0x100231E9U;   // AGC ON, LNA high linearity
#elif defined(AD7021_GAIN_AUTO_REDUCED)
  // AGC ON, LNA reduced gain mode (x10 at most), mixer high linearity, AGC
  // window ADF7021_AGC_LOW..ADF7021_AGC_HIGH
  return 0x12000009U | ((uint32_t)ADF7021_AGC_HIGH << 11) | ((uint32_t)ADF7021_AGC_LOW << 4);
#elif defined(AD7021_GAIN_LOW)
  return 0x120631E9U;   // AGC OFF, low gain, LNA high linearity
#elif defined(AD7021_GAIN_HIGH)
  return 0x00A631E9U;   // AGC OFF, high gain
#else
  return 0x000231E9U;   // AGC ON, normal operation
#endif
}

// Full set-up (full): reset (CE), VCO, IF filter calibration and every
// register. A mode switch while scanning (!full) only writes what differs
// between modes: TX/RX clocks (3), demodulator (4), RX frequency (0, the
// same for every mode), modulation (2), AFC (10)
// and 4FSK slicer (13). Every mode uses the same IF filter (25 kHz), so it
// needs no recalibration: a switch takes some 100 us instead of 11 ms, and
// the AGC carries on where it was.
void CIO::ifConf(MMDVM_STATE modemState, bool reset, bool full)
{
  float    divider;

  uint32_t ADF7021_REG2  = 0U;
  uint32_t ADF7021_REG3  = 0U;
  uint32_t ADF7021_REG4  = 0U;
  uint32_t ADF7021_REG10 = 0U;
  uint32_t ADF7021_REG13 = 0U;

  uint32_t frequency_tx_tmp = 0U, frequency_rx_tmp = 0U;

  // POCSAG and CW ID change frequency and transmit: always the full set-up
  if (modemState == STATE_CWID || modemState == STATE_POCSAG)
    full = true;

  if (modemState != STATE_CWID && modemState != STATE_POCSAG)
    m_modemState_prev = modemState;

  // Change frequency for POCSAG mode, store a backup of DV frequencies
  if (modemState == STATE_POCSAG) {
    frequency_tx_tmp = m_frequency_tx;
    frequency_rx_tmp = m_frequency_rx;
    m_frequency_tx   = m_pocsag_freq_tx;
    m_frequency_rx   = m_pocsag_freq_tx;
  }

  #if defined (ZUMSPOT_ADF7021) || defined(SKYBRIDGE_HS)
  io.checkBand(m_frequency_rx, m_frequency_tx);
  #endif

  // Toggle CE pin for ADF7021 reset
  if (reset && full) {
    CE_pin(LOW);
    delay_reset();
    CE_pin(HIGH);
    delay_reset();
  }

  // Check frequency band
  if ((m_frequency_tx >= VHF1_MIN) && (m_frequency_tx < VHF1_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_VHF1;         // VHF1, external VCO
    div2 = 1U;
  } else if ((m_frequency_tx >= VHF2_MIN) && (m_frequency_tx < VHF2_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_VHF2;         // VHF1, external VCO
    div2 = 1U;
  } else if ((m_frequency_tx >= UHF1_MIN)&&(m_frequency_tx < UHF1_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_UHF1;         // UHF1, internal VCO
    div2 = 1U;
  } else if ((m_frequency_tx >= UHF2_MIN)&&(m_frequency_tx < UHF2_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_UHF2;         // UHF2, internal VCO
    div2 = 2U;
  } else {
    ADF7021_REG1 = ADF7021_REG1_UHF1;         // UHF1, internal VCO
    div2 = 1U;
  }

  if (div2 == 1U)
    f_div = 2U;
  else
    f_div = 1U;

  // The RX LO, the demodulator's centre below the RX frequency
  m_rxIF = ifCentre(modemState);
  if (div2 == 1U)
    divider = (m_frequency_rx - m_rxIF) / (ADF7021_PFD / 2U);
  else
    divider = (m_frequency_rx - m_rxIF) / ADF7021_PFD;

  m_RX_N_divider = floor(divider);
  divider = (divider - m_RX_N_divider) * 32768;
  m_RX_F_divider = floor(divider + 0.5);

  ADF7021_RX_REG0  = (uint32_t) 0b0000;

#if defined(BIDIR_DATA_PIN)
  ADF7021_RX_REG0 |= (uint32_t) 0b01001   << 27;   // mux regulator/receive
#else
  ADF7021_RX_REG0 |= (uint32_t) 0b01011   << 27;   // mux regulator/uart-spi enabled/receive
#endif

  ADF7021_RX_REG0 |= (uint32_t) m_RX_N_divider << 19;   // frequency;
  ADF7021_RX_REG0 |= (uint32_t) m_RX_F_divider << 4;    // frequency;

  if (div2 == 1U)
    divider = m_frequency_tx / (ADF7021_PFD / 2U);
  else
    divider = m_frequency_tx / ADF7021_PFD;

  m_TX_N_divider = floor(divider);
  divider = (divider - m_TX_N_divider) * 32768;
  m_TX_F_divider = floor(divider + 0.5);

  ADF7021_TX_REG0  = (uint32_t) 0b0000;            // register 0

#if defined(BIDIR_DATA_PIN)
  ADF7021_TX_REG0 |= (uint32_t) 0b01000   << 27;   // mux regulator/transmit
#else
  ADF7021_TX_REG0 |= (uint32_t) 0b01010   << 27;   // mux regulator/uart-spi enabled/transmit
#endif

  ADF7021_TX_REG0 |= (uint32_t) m_TX_N_divider << 19;   // frequency;
  ADF7021_TX_REG0 |= (uint32_t) m_TX_F_divider << 4;    // frequency;

#if defined(TEST_TX)
  modemState = STATE_DSTAR;
#endif

  switch (modemState) {
    case STATE_CWID:
      // CW ID base configuration: DMR
      // Dev: +1 symb (variable), symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_DMR;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_DMR       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_DMR       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_DMR    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_cwIdTXLevel / div2)    << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
      break;

    case STATE_POCSAG:
      // Dev: 4500 Hz, symb rate = 1200

      ADF7021_REG3 = ADF7021_REG3_POCSAG;
      ADF7021_REG10 = afcControl(false);  // TX only: no AFC

      ADF7021_REG4  = (uint32_t) 0b0100                     << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b000                      << 4;   // 2FSK linear demodulator
      ADF7021_REG4 |= (uint32_t) 0b1                        << 7;
      ADF7021_REG4 |= (uint32_t) 0b10                       << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_POCSAG     << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_POCSAG     << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                       << 30;  // IF filter (25 kHz)

      // Register 13 not used with 2FSK
      ADF7021_REG13 = (uint32_t) 0b1101                     << 0;   // register 13

      ADF7021_REG2  = (uint32_t) 0b10                       << 28;  // inverted data, clock normal
      ADF7021_REG2 |= (uint32_t) (m_pocsagDev / div2)       << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b000                      << 4;   // modulation (2FSK)
      break;

    case STATE_DSTAR:
      // Dev: 1200 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_DSTAR;
      ADF7021_REG10 = afcControl(true);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b001                     << 4;   // mode, GMSK
      ADF7021_REG4 |= (uint32_t) 0b1                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b10                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_DSTAR     << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_DSTAR     << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz, every mode: tolerance, and no IF change between modes)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_DSTAR  << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b00                       << 28;  // clock normal
      ADF7021_REG2 |= (uint32_t) (m_dstarDev / div2)       << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b001                     << 4;   // modulation (GMSK)
      break;

    case STATE_DMR:
      // Dev: +1 symb 648 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_DMR;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_DMR       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_DMR       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_DMR    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_dmrDev / div2)         << 19;  // deviation
#if defined(ADF7021_DISABLE_RC_4FSK)
      ADF7021_REG2 |= (uint32_t) 0b011                     << 4;   // modulation (4FSK)
#else
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
#endif
      break;

    case STATE_YSF:
      // Dev: +1 symb 900/450 Hz, symb rate = 4800

      ADF7021_REG3 = (m_LoDevYSF ? ADF7021_REG3_YSF_L : ADF7021_REG3_YSF_H);
      ADF7021_REG10 = afcControl(false);

      // K=28
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) (m_LoDevYSF ? ADF7021_DISC_BW_YSF_L : ADF7021_DISC_BW_YSF_H) << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_YSF       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) (m_LoDevYSF ? ADF7021_SLICER_TH_YSF_L : ADF7021_SLICER_TH_YSF_H) << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_ysfDev / div2)         << 19;  // deviation
#if defined(ADF7021_DISABLE_RC_4FSK)
      ADF7021_REG2 |= (uint32_t) 0b011                     << 4;   // modulation (4FSK)
#else
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
#endif
      break;

    case STATE_P25:
      // Dev: +1 symb 600 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_P25;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_P25       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_P25       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz, every mode: tolerance, and no IF change between modes)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_P25    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_p25Dev / div2)         << 19;  // deviation
#if defined(ENABLE_P25_WIDE) || defined(ADF7021_DISABLE_RC_4FSK)
      ADF7021_REG2 |= (uint32_t) 0b011                     << 4;   // modulation (4FSK)
#else
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
#endif
      break;

    case STATE_NXDN:
      // Dev: +1 symb 350 Hz, symb rate = 2400

      ADF7021_REG3 = ADF7021_REG3_NXDN;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_NXDN      << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_NXDN      << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz, every mode: tolerance, and no IF change between modes)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_NXDN   << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_nxdnDev / div2)        << 19;  // deviation
#if defined(ADF7021_DISABLE_RC_4FSK)
      ADF7021_REG2 |= (uint32_t) 0b011                     << 4;   // modulation (4FSK)
#else
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
#endif
      break;

    case STATE_M17:
      // Dev: +1 symb 800 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_M17;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_M17       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_M17       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_M17    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_m17Dev / div2)         << 19;  // deviation
#if defined(ADF7021_DISABLE_RC_4FSK)
      ADF7021_REG2 |= (uint32_t) 0b011                     << 4;   // modulation (4FSK)
#else
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
#endif
      break;

    default:
      break;
  }

  // VCO/OSCILLATOR (REG1)
  if (full) {
    AD7021_control_word = ADF7021_REG1;
    Send_AD7021_control();
  }

  // TX/RX CLOCK (3)
  AD7021_control_word = ADF7021_REG3;
  Send_AD7021_control();

  // DEMOD (4)
  AD7021_control_word = ADF7021_REG4;
  Send_AD7021_control();

  if (full) {
    // IF fine cal (6)
    AD7021_control_word = ADF7021_REG6;
    Send_AD7021_control();

    // IF coarse cal (5)
    AD7021_control_word = ADF7021_REG5;
    Send_AD7021_control();

    // Delay for filter calibration
    delay_IFcal();

    // Frequency RX (0)
    setRX();
  } else {
    // Frequency RX (0, each mode's own: see ifCentre): scanning only happens while receiving
    AD7021_control_word = ADF7021_RX_REG0;
    Send_AD7021_control();
  }

  // MODULATION (2)
  ADF7021_REG2 |= (uint32_t) 0b0010;               // register 2
  ADF7021_REG2 |= (uint32_t) m_power       << 13;  // power level
  ADF7021_REG2 |= (uint32_t) 0b110001      << 7;   // PA
  AD7021_control_word = ADF7021_REG2;
  Send_AD7021_control();

  if (full) {
    // TEST DAC (14)
#if defined(TEST_DAC)
    AD7021_control_word = 0x0000001E;
#else
    AD7021_control_word = 0x0000000E;
#endif
    Send_AD7021_control();

    // AGC (9)
    AD7021_control_word = agcControl();
    Send_AD7021_control();
  }

  // AFC (10)
  AD7021_control_word = ADF7021_REG10;
  Send_AD7021_control();

  if (full) {
    // SYNC WORD DET (11)
    AD7021_control_word = 0x0000003B;
    Send_AD7021_control();

    // SWD/THRESHOLD (12)
    AD7021_control_word = 0x0000010C;
    Send_AD7021_control();
  }

  // 3FSK/4FSK DEMOD (13)
  AD7021_control_word = ADF7021_REG13;
  Send_AD7021_control();

  if (!full) {
#if defined(DUPLEX)
    if (m_duplex)
      ifConf2(modemState, false);
#endif
    return;
  }

#if defined(TEST_TX)
  PTT_pin(HIGH);
  AD7021_control_word = ADF7021_TX_REG0;
  Send_AD7021_control();
  // TEST MODE (TX carrier only) (15)
  AD7021_control_word = 0x000E010F;
#else
  // TEST MODE (disabled) (15)
  AD7021_control_word = 0x000E000F;
#endif
  Send_AD7021_control();

  // Restore normal DV frequencies
  if (modemState == STATE_POCSAG) {
    m_frequency_tx = frequency_tx_tmp;
    m_frequency_rx = frequency_rx_tmp;
  }

#if defined(DUPLEX)
if (m_duplex && (modemState != STATE_CWID && modemState != STATE_POCSAG))
  ifConf2(modemState);
#endif
}

#if defined(DUPLEX)
void CIO::ifConf2(MMDVM_STATE modemState, bool full)
{
  uint32_t ADF7021_REG2  = 0U;
  uint32_t ADF7021_REG3  = 0U;
  uint32_t ADF7021_REG4  = 0U;
  uint32_t ADF7021_REG10 = 0U;
  uint32_t ADF7021_REG13 = 0U;

  switch (modemState) {
    case STATE_DSTAR:
      // Dev: 1200 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_DSTAR;
      ADF7021_REG10 = afcControl(true);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b001                     << 4;   // mode, GMSK
      ADF7021_REG4 |= (uint32_t) 0b1                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b10                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_DSTAR     << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_DSTAR     << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz, every mode: tolerance, and no IF change between modes)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_DSTAR  << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b00                       << 28;  // clock normal
      ADF7021_REG2 |= (uint32_t) (m_dstarDev / div2)<< 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b001                     << 4;   // modulation (GMSK)
      break;

    case STATE_DMR:
      // Dev: +1 symb 648 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_DMR;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_DMR       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_DMR       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_DMR    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_dmrDev / div2)  << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
      break;

    case STATE_YSF:
      // Dev: +1 symb 900/450 Hz, symb rate = 4800

      ADF7021_REG3 = (m_LoDevYSF ? ADF7021_REG3_YSF_L : ADF7021_REG3_YSF_H);
      ADF7021_REG10 = afcControl(false);

      // K=28
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) (m_LoDevYSF ? ADF7021_DISC_BW_YSF_L : ADF7021_DISC_BW_YSF_H) << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_YSF       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) (m_LoDevYSF ? ADF7021_SLICER_TH_YSF_L : ADF7021_SLICER_TH_YSF_H) << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_ysfDev / div2)  << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
      break;

    case STATE_P25:
      // Dev: +1 symb 600 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_P25;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_P25       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_P25       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz, every mode: tolerance, and no IF change between modes)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_P25    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_p25Dev / div2)  << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
      break;

    case STATE_NXDN:
      // Dev: +1 symb 350 Hz, symb rate = 2400

      ADF7021_REG3 = ADF7021_REG3_NXDN;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_NXDN      << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_NXDN      << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz, every mode: tolerance, and no IF change between modes)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_NXDN   << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_nxdnDev / div2) << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
      break;

    case STATE_M17:
      // Dev: +1 symb 800 Hz, symb rate = 4800

      ADF7021_REG3 = ADF7021_REG3_M17;
      ADF7021_REG10 = afcControl(false);

      // K=32
      ADF7021_REG4  = (uint32_t) 0b0100                    << 0;   // register 4
      ADF7021_REG4 |= (uint32_t) 0b011                     << 4;   // mode, 4FSK
      ADF7021_REG4 |= (uint32_t) 0b0                       << 7;
      ADF7021_REG4 |= (uint32_t) 0b11                      << 8;
      ADF7021_REG4 |= (uint32_t) ADF7021_DISC_BW_M17       << 10;  // Disc BW
      ADF7021_REG4 |= (uint32_t) ADF7021_POST_BW_M17       << 20;  // Post dem BW
      ADF7021_REG4 |= (uint32_t) 0b10                      << 30;  // IF filter (25 kHz)

      ADF7021_REG13 = (uint32_t) 0b1101                    << 0;   // register 13
      ADF7021_REG13 |= (uint32_t) ADF7021_SLICER_TH_M17    << 4;   // slicer threshold

      ADF7021_REG2 = (uint32_t) 0b10                       << 28;  // invert data (and RC alpha = 0.5)
      ADF7021_REG2 |= (uint32_t) (m_m17Dev / div2)  << 19;  // deviation
      ADF7021_REG2 |= (uint32_t) 0b111                     << 4;   // modulation (RC 4FSK)
      break;

    default:
      break;
  }

  // VCO/OSCILLATOR (1)
  if (full) {
    AD7021_control_word = ADF7021_REG1;
    Send_AD7021_control2();
  }

  // TX/RX CLOCK (3)
  AD7021_control_word = ADF7021_REG3;
  Send_AD7021_control2();

  // DEMOD (4)
  AD7021_control_word = ADF7021_REG4;
  Send_AD7021_control2();

  if (full) {
    // IF fine cal (6)
    AD7021_control_word = ADF7021_REG6;
    Send_AD7021_control2();

    // IF coarse cal (5)
    AD7021_control_word = ADF7021_REG5;
    Send_AD7021_control2();

    // Delay for coarse IF filter calibration
    delay_IFcal();
  }

  // Frequency RX (0) and set to RX only
  AD7021_control_word = ADF7021_RX_REG0;
  Send_AD7021_control2();

  // MODULATION (2)
  ADF7021_REG2 |= (uint32_t) 0b0010;                  // register 2
  ADF7021_REG2 |= (uint32_t) (m_power & 0x3F) << 13;  // power level
  ADF7021_REG2 |= (uint32_t) 0b110001         << 7;   // PA
  AD7021_control_word = ADF7021_REG2;
  Send_AD7021_control2();

  if (full) {
    // TEST DAC (14)
    AD7021_control_word = 0x0000000E;
    Send_AD7021_control2();

    // AGC (9): as the board's config selects, like the first ADF7021
    AD7021_control_word = agcControl();
    Send_AD7021_control2();
  }

  // AFC (10)
  AD7021_control_word = ADF7021_REG10;
  Send_AD7021_control2();

  if (full) {
    // SYNC WORD DET (11)
    AD7021_control_word = 0x0000003B;
    Send_AD7021_control2();

    // SWD/THRESHOLD (12)
    AD7021_control_word = 0x0000010C;
    Send_AD7021_control2();
  }

  // 3FSK/4FSK DEMOD (13)
  AD7021_control_word = ADF7021_REG13;
  Send_AD7021_control2();

  if (full) {
    // TEST MODE (disabled) (15)
    AD7021_control_word = 0x000E000F;
    Send_AD7021_control2();
  }
}
#endif

void CIO::interrupt()
{
  uint8_t bit = 0U;

  if (!m_started)
    return;

  uint8_t clk = CLK_pin();

  // this is to prevent activation by spurious interrupts
  // which seem to happen if you send out an control word
  // needs investigation
  // this workaround will fail if only rising or falling edge
  // is used to trigger the interrupt !!!!
  // TODO: figure out why sending the control word seems to issue interrupts
  // possibly this is a design problem of the RF7021 board or too long wires
  // on the breadboard build
  // but normally this will not hurt too much
  if (clk == last_clk)
    return;
  else
    last_clk = clk;

  // we set the TX bit at TXD low, sampling of ADF7021 happens at rising clock
  if (m_tx && clk == 0U) {
    m_txBuffer.get(bit, m_control);
    even = !even;

#if defined(BIDIR_DATA_PIN)
    if (bit)
      RXD_pin_write(HIGH);
    else
      RXD_pin_write(LOW);
#else
    if (bit)
      TXD_pin(HIGH);
    else
      TXD_pin(LOW);
#endif

    // wait a brief period before raising SLE
    if (totx_request == true) {
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );

      // SLE Pulse, should be moved out of here into class method
      // according to datasheet in 4FSK we have to deliver this before 1/4 tbit == 26uS
      SLE_pin(HIGH);
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );
      SLE_pin(LOW);
      SDATA_pin(LOW);

      // now do housekeeping
      totx_request = false;
      // first tranmittted bit is always the odd bit
      even = ADF7021_EVEN_BIT;
    }
  }

  // we sample the RX bit at rising TXD clock edge, so TXD must be 1 and we are not in tx mode
  if (!m_tx && clk == 1U && !m_duplex) {
    if (RXD_pin())
      bit = 1U;
    else
      bit = 0U;

    m_rxBuffer.put(bit, m_control);
  }

  if (torx_request && even == ADF7021_EVEN_BIT && m_tx && clk == 0U) {
      // that is absolutely crucial in 4FSK, see datasheet:
      // enable sle after 1/4 tBit == 26uS when sending MSB (even == false) and clock is low
      delay_us(26U);

      // SLE Pulse, should be moved out of here into class method
      SLE_pin(HIGH);
      asm volatile("nop          \n\t"
                   "nop          \n\t"
                   "nop          \n\t"
                   );
      SLE_pin(LOW);
      SDATA_pin(LOW);

      // now do housekeeping
      m_tx = false;
      torx_request = false;
      // last tranmittted bit is always the even bit
      // since the current bit is a transitional "don't care" bit, never transmitted
      even = !ADF7021_EVEN_BIT;
  }

  m_watchdog++;
  m_modeTimerCnt++;
  m_int1counter++;

  if (m_scanPauseCnt >= SCAN_PAUSE)
    m_scanPauseCnt = 0U;

  if (m_scanPauseCnt != 0U)
    m_scanPauseCnt++;
}

#if defined(DUPLEX)
void CIO::interrupt2()
{
  uint8_t bit = 0U;

  if (m_duplex) {
    if (RXD2_pin())
      bit = 1U;
    else
      bit = 0U;

    m_rxBuffer.put(bit, m_control);
  }

  m_int2counter++;
}
#endif

void CIO::setTX()
{
  // PTT pin on (doing it earlier helps to measure timing impact)
  PTT_pin(HIGH);

  // Send register 0 for TX operation, but do not activate yet.
  // This is done in the interrupt at the correct time
  AD7021_control_word = ADF7021_TX_REG0;
  Send_AD7021_control(false);

#if defined(BIDIR_DATA_PIN)
  Data_dir_out(true);  // Data pin output mode
#endif

  totx_request = true;
  while(CLK_pin());
}

void CIO::setRX(bool doSle)
{
  // PTT pin off (doing it earlier helps to measure timing impact)
  PTT_pin(LOW);

  // Send register 0 for RX operation, but do not activate yet.
  // This is done in the interrupt at the correct time
  AD7021_control_word = ADF7021_RX_REG0;
  Send_AD7021_control(doSle);

#if defined(BIDIR_DATA_PIN)
  Data_dir_out(false);  // Data pin input mode
#endif

  if (!doSle) {
    torx_request = true;
    while(torx_request) { asm volatile ("nop"); }
  }
}

void CIO::setPower(uint8_t power)
{
  m_power = power >> 2;
}

void CIO::setDeviations(uint8_t dstarTXLevel, uint8_t dmrTXLevel, uint8_t ysfTXLevel, uint8_t p25TXLevel, uint8_t nxdnTXLevel, uint8_t m17TXLevel, uint8_t pocsagTXLevel, bool ysfLoDev)
{
  m_dstarDev = uint16_t((ADF7021_DEV_DSTAR * uint16_t(dstarTXLevel)) / 128U);
  m_dmrDev = uint16_t((ADF7021_DEV_DMR * uint16_t(dmrTXLevel)) / 128U);

  if (ysfLoDev)
    m_ysfDev = uint16_t((ADF7021_DEV_YSF_L * uint16_t(ysfTXLevel)) / 128U);
  else
    m_ysfDev = uint16_t((ADF7021_DEV_YSF_H * uint16_t(ysfTXLevel)) / 128U);

  m_p25Dev = uint16_t((ADF7021_DEV_P25 * uint16_t(p25TXLevel)) / 128U);
  m_nxdnDev = uint16_t((ADF7021_DEV_NXDN * uint16_t(nxdnTXLevel)) / 128U);
  m_m17Dev = uint16_t((ADF7021_DEV_M17 * uint16_t(m17TXLevel)) / 128U);
  m_pocsagDev = uint16_t((ADF7021_DEV_POCSAG * uint16_t(pocsagTXLevel)) / 128U);
}

void CIO::updateCal()
{
  uint32_t ADF7021_REG2;
  float    divider;

  // Check frequency band
  if ((m_frequency_tx >= VHF1_MIN) && (m_frequency_tx < VHF1_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_VHF1;         // VHF1, external VCO
    div2 = 1U;
  } else if ((m_frequency_tx >= VHF2_MIN) && (m_frequency_tx < VHF2_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_VHF2;         // VHF1, external VCO
    div2 = 1U;
  } else if ((m_frequency_tx >= UHF1_MIN)&&(m_frequency_tx < UHF1_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_UHF1;         // UHF1, internal VCO
    div2 = 1U;
  } else if ((m_frequency_tx >= UHF2_MIN)&&(m_frequency_tx < UHF2_MAX)) {
    ADF7021_REG1 = ADF7021_REG1_UHF2;         // UHF2, internal VCO
    div2 = 2U;
  } else {
    ADF7021_REG1 = ADF7021_REG1_UHF1;         // UHF1, internal VCO
    div2 = 1U;
  }

  if (div2 == 1U)
    f_div = 2U;
  else
    f_div = 1U;

  // VCO/OSCILLATOR (REG1)
  AD7021_control_word = ADF7021_REG1;
  Send_AD7021_control();

  ADF7021_REG2  = (uint32_t) 0b10              << 28;  // invert data (and RC alpha = 0.5)

  if (m_modemState == STATE_DMR) {
    ADF7021_REG2 |= (uint32_t) (m_dmrDev / div2)    << 19;  // DMR deviation
    ADF7021_REG2 |= (uint32_t) 0b111                << 4;   // modulation (RC 4FSK)
  } else if (m_modemState == STATE_POCSAG) {
    ADF7021_REG2 |= (uint32_t) (m_pocsagDev / div2) << 19;  // POCSAG deviation
    ADF7021_REG2 |= (uint32_t) 0b000                << 4;   // modulation (2FSK)
  }

  ADF7021_REG2 |= (uint32_t) 0b0010;                   // register 2
  ADF7021_REG2 |= (uint32_t) m_power           << 13;  // power level
  ADF7021_REG2 |= (uint32_t) 0b110001          << 7;   // PA

  AD7021_control_word = ADF7021_REG2;
  Send_AD7021_control();

  if (div2 == 1U)
    divider = m_frequency_tx / (ADF7021_PFD / 2U);
  else
    divider = m_frequency_tx / ADF7021_PFD;

  m_TX_N_divider = floor(divider);
  divider = (divider - m_TX_N_divider) * 32768;
  m_TX_F_divider = floor(divider + 0.5);

  ADF7021_TX_REG0  = (uint32_t) 0b0000;            // register 0

#if defined(BIDIR_DATA_PIN)
  ADF7021_TX_REG0 |= (uint32_t) 0b01000   << 27;   // mux regulator/transmit
#else
  ADF7021_TX_REG0 |= (uint32_t) 0b01010   << 27;   // mux regulator/uart-spi enabled/transmit
#endif

  ADF7021_TX_REG0 |= (uint32_t) m_TX_N_divider << 19;   // frequency;
  ADF7021_TX_REG0 |= (uint32_t) m_TX_F_divider << 4;    // frequency;

  if (m_tx)
    setTX();
  else
    setRX();
}

#if defined(ENABLE_DEBUG)

uint32_t CIO::RXfreq()
{
  return (uint32_t)((float)(ADF7021_PFD / f_div) * ((float)((32768 * m_RX_N_divider) + m_RX_F_divider) / 32768.0)) + m_rxIF;
}

uint32_t CIO::TXfreq()
{
  return (uint32_t)((float)(ADF7021_PFD / f_div) * ((float)((32768 * m_TX_N_divider) + m_TX_F_divider) / 32768.0));
}

uint16_t CIO::devDSTAR()
{
  return (uint16_t)((ADF7021_PFD * m_dstarDev) / (f_div * 65536));
}

uint16_t CIO::devDMR()
{
  return (uint16_t)((ADF7021_PFD * m_dmrDev) / (f_div * 65536));
}

uint16_t CIO::devYSF()
{
  return (uint16_t)((ADF7021_PFD * m_ysfDev) / (f_div * 65536));
}

uint16_t CIO::devP25()
{
  return (uint16_t)((ADF7021_PFD * m_p25Dev) / (f_div * 65536));
}

uint16_t CIO::devNXDN()
{
  return (uint16_t)((ADF7021_PFD * m_nxdnDev) / (f_div * 65536));
}

uint16_t CIO::devM17()
{
  return (uint16_t)((ADF7021_PFD * m_m17Dev) / (f_div * 65536));
}

uint16_t CIO::devPOCSAG()
{
  return (uint16_t)((ADF7021_PFD * m_pocsagDev) / (f_div * 65536));
}

void CIO::printConf()
{
  DEBUG1("MMDVM_HS FW configuration:");
  DEBUG2I("TX freq (Hz):", TXfreq());
  DEBUG2I("RX freq (Hz):", RXfreq());
  DEBUG2("Power set:", m_power);
  DEBUG2("D-Star dev (Hz):", devDSTAR());
  DEBUG2("DMR +1 sym dev (Hz):", devDMR());
  DEBUG2("YSF +1 sym dev (Hz):", devYSF());
  DEBUG2("P25 +1 sym dev (Hz):", devP25());
  DEBUG2("NXDN +1 sym dev (Hz):", devNXDN());
  DEBUG2("M17 +1 sym dev (Hz):", devM17());
  DEBUG2("POCSAG dev (Hz):", devPOCSAG());
}

#endif

#endif
