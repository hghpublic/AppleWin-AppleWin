/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2024, Tom Charlesworth, Michael Pohoreski, Nick Westgate

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: SSI263 emulation
 *
 * Extra "spec" that's not obvious from the datasheet: (GH#175)
 * . Writes to regs 0,1,2 (and reg3.CTL=1) all de-assert the IRQ (and writes to reg3.CTL=0 and regs 4..7 don't) (GH#1197)
 * . A phoneme will continue playing back infinitely; unless the phoneme is changed or CTL=1.
 *   . NB. if silenced (Amplitude=0) it's still playing.
 *   . The IRQ is set at the end of the phoneme.
 *   . If IRQ is then cleared, a new IRQ will occur when the phoneme completes again (but need to clear IRQ with a write to reg0, 1 or 2, even for Mockingboard-C).
 * . CTL=1 sets "PD" (Power Down / "standby") mode, also set at power-on.
 *   . Registers can still be changed in this mode.
 *   . IRQ de-asserted & D7=0.
 * . CTL=0 brings device out of "PD" mode, the mode will be set to DR1,DR0 and the phoneme P5-P0 will play.
 * . Setting mode to DR1:0 = %00 just disables A/!R (ie. disables interrupts), but otherwise retains the previous DR1:0 mode.
 *   . If an IRQ was previously asserted then to set DR1:0=%00, you must go via CTL=1, which de-asserts the IRQ.
 * . Mockingboard-C: CTRL+RESET is not connected to !PD/!RST pin 18.
 *   . Phasor: TODO: check with a 'scope.
 *   . Phasor: with SSI263 ints disabled & reg0's DR1:0 != %00, then CTRL+RESET will cause SSI263 to enable ints & assert IRQ.
 *     . it's as if the SSI263 does a CTL H->L to pick-up the new DR1:0. (Bug in SSI263? Assume it should remain in PD mode.)
 *     . but if CTL=1, then CTRL+RESET has no effect.
 * . Power-on: PD=1 (so D7=0), reg4 (Filter Freq)=0xFF (other regs are seemingly random?).
 */

#include "StdAfx.h"

#include "6522.h"
#include "CardManager.h"
#include "Mockingboard.h"
#include "Core.h"
#include "CPU.h"
#include "Log.h"
#include "Memory.h"
#include "SoundCore.h"
#include "SSI263.h"
#include "SSI263Phonemes.h"

#include "YamlHelper.h"

#define LOG_SSI263 0
#define LOG_SSI263B 0	// Alternate SSI263 logging (use in conjunction with CPU.cpp's LOG_IRQ_TAKEN_AND_RTI)
#define LOG_SC01 0

// SSI263A registers:
#define SSI_DURPHON	0x00
#define SSI_INFLECT	0x01
#define SSI_RATEINF	0x02
#define SSI_CTTRAMP	0x03
#define SSI_FILFREQ	0x04

const uint32_t SAMPLE_RATE_SSI263 = 22050;

//-----------------------------------------------------------------------------

#if LOG_SSI263B
static int ssiRegs[5]={-1,-1,-1,-1,-1};
static int totalDuration_ms = 0;

void SSI_Output(void)
{
	int ssi0 = ssiRegs[SSI_DURPHON];
	int ssi2 = ssiRegs[SSI_RATEINF];

	LogOutput("SSI: ");
	for (int i = 0; i <= 4; i++)
	{
		std::string r = (ssiRegs[i] >= 0) ? ByteToHexStr(ssiRegs[i]) : "--";
		LogOutput("%s ", r.c_str());
		ssiRegs[i] = -1;
	}

	if (ssi0 != -1 && ssi2 != -1)
	{
		int phonemeDuration_ms = (((16-(ssi2>>4))*4096)/1023) * (4-(ssi0>>6));
		totalDuration_ms += phonemeDuration_ms;
		LogOutput("/ duration = %d (total = %d) ms", phonemeDuration_ms, totalDuration_ms);
	}

	LogOutput("\n");
}
#endif

//-----------------------------------------------------------------------------

UINT64 SSI263::GetLastCumulativeCycles(void)
{
	return dynamic_cast<MockingboardCard&>(GetCardMgr().GetRef(m_slot)).GetLastCumulativeCycles();
}

void SSI263::UpdateIFR(BYTE nDevice, BYTE clr_mask, BYTE set_mask)
{
	dynamic_cast<MockingboardCard&>(GetCardMgr().GetRef(m_slot)).UpdateIFR(nDevice, clr_mask, set_mask);
}

BYTE SSI263::GetPCR(BYTE nDevice)
{
	return dynamic_cast<MockingboardCard&>(GetCardMgr().GetRef(m_slot)).GetPCR(nDevice);
}

//-----------------------------------------------------------------------------

BYTE SSI263::Read(ULONG nExecutedCycles)
{
	// Regardless of register, just return inverted A/!R in bit7
	// . inverted "A/!R" is high for REQ (ie. Request, as phoneme nearly complete)
	// NB. this doesn't clear the IRQ

	if (m_type == SSI263Empty)
		return MemReadFloatingBus(nExecutedCycles);

	return MemReadFloatingBus(m_currentMode.D7, nExecutedCycles);
}

void SSI263::Write(BYTE nReg, BYTE nValue)
{
#if LOG_SSI263B
	_ASSERT(nReg < 5);
	if (nReg>4) nReg=4;
	if (ssiRegs[nReg]>=0) SSI_Output();	// overwriting a reg
	ssiRegs[nReg] = nValue;
#endif

	if (m_type == SSI263Empty)
		return;

	// SSI263 datasheet is not clear, but a write to DURPHON de-asserts the IRQ and clears D7.
	// . Empirically writes to regs 0,1,2 (and reg3.CTL=1) all de-assert the IRQ (and writes to reg3.CTL=0 and regs 4..7 don't) (GH#1197)
	// NB. The same for Mockingboard as there's no automatic handshake from the 6522 (CA2 isn't connected to the SSI263). So writes to regs 0, 1 or 2 complete the "handshake".
	if (nReg <= SSI_RATEINF)
	{
		CpuIrqDeassert(IS_SPEECH);
		m_currentMode.D7 = 0;
	}

	switch(nReg)
	{
	case SSI_DURPHON:
#if LOG_SSI263
		if (g_fh) fprintf(g_fh, "DUR   = 0x%02X, PHON = 0x%02X\n\n", nValue>>6, nValue&PHONEME_MASK);
		LogOutput("DUR   = %d, PHON = 0x%02X\n", nValue>>6, nValue&PHONEME_MASK);
#endif
#if LOG_SSI263B
		SSI_Output();
#endif

		m_durationPhoneme = nValue;
		m_isVotraxPhoneme = false;

		if ((m_ctrlArtAmp & CONTROL_MASK) == 0)
			Play(m_durationPhoneme & PHONEME_MASK);		// Play phoneme when *not* in power-down / standby mode
		break;
	case SSI_INFLECT:
#if LOG_SSI263
		if (g_fh) fprintf(g_fh, "INF   = 0x%02X\n", nValue);
#endif
		m_inflection = nValue;
		break;

	case SSI_RATEINF:
#if LOG_SSI263
		if (g_fh) fprintf(g_fh, "RATE  = 0x%02X, INF = 0x%02X\n", nValue>>4, nValue&0x0F);
#endif
		m_rateInflection = nValue;
		break;
	case SSI_CTTRAMP:
#if LOG_SSI263
		if (g_fh) fprintf(g_fh, "CTRL  = %d, ART = 0x%02X, AMP=0x%02X\n", nValue>>7, (nValue&ARTICULATION_MASK)>>4, nValue&AMPLITUDE_MASK);
		//
		{
			bool H2L = (m_ctrlArtAmp & CONTROL_MASK) && !(nValue & CONTROL_MASK);
			std::string newMode = StrFormat(" (new mode=%d)", m_durationPhoneme>>6);
			LogOutput("CTRL  = %d->%d, ART = 0x%02X, AMP=0x%02X%s\n", m_ctrlArtAmp>>7, nValue>>7, (nValue&ARTICULATION_MASK)>>4, nValue&AMPLITUDE_MASK, H2L?newMode.c_str() : "");
		}
#endif
#if LOG_SSI263B
		if ( ((m_ctrlArtAmp & CONTROL_MASK) && !(nValue & CONTROL_MASK)) || ((nValue&0xF) == 0x0) )	// H->L or amp=0
			SSI_Output();
#endif
		if ((m_ctrlArtAmp & CONTROL_MASK) && !(nValue & CONTROL_MASK))	// H->L
		{
			// NB. Just changed from CTL=1 (power-down) - where IRQ was already de-asserted & D7=0
			// . So CTL H->L never affects IRQ or D7
			SetDeviceModeAndInts();

			// Device out of power down / "standby" mode, so play phoneme
			m_isVotraxPhoneme = false;
			Play(m_durationPhoneme & PHONEME_MASK);
		}

		m_ctrlArtAmp = nValue;

		// "Setting the Control bit (CTL) to a logic one puts the device into Power Down mode..." (SSI263 datasheet)
		// . this silences the phoneme - actually "turns off the excitation sources and analog circuits"
		if (m_ctrlArtAmp & CONTROL_MASK)
		{
			CpuIrqDeassert(IS_SPEECH);
			m_currentMode.D7 = 0;
			// NB. Don't call Stop() - mb-audit ("Classic Adventure") cuts at end, Berzap! is choppy
		}
		break;
	case SSI_FILFREQ:	// RegAddr.b2=1 (b1 & b0 are: don't care)
	default:
#if LOG_SSI263
		if (g_fh) fprintf(g_fh, "FFREQ = 0x%02X\n", nValue);
#endif
		m_filterFreq = nValue;
		break;
	}
}

void SSI263::SetDeviceModeAndInts(void)
{
	if ((m_durationPhoneme & DURATION_MODE_MASK) != MODE_IRQ_DISABLED)
	{
		m_currentMode.function = (m_durationPhoneme & DURATION_MODE_MASK) >> DURATION_MODE_SHIFT;
		m_currentMode.enableInts = 1;
	}
	else
	{
		// "Disables A/!R output only; does not change previous A/!R response" (SSI263 datasheet)
		m_currentMode.enableInts = 0;
	}
}

//-----------------------------------------------------------------------------

const BYTE SSI263::m_Votrax2SSI263[/*64*/] =
{
	0x02,	// 00: EH3 jackEt -> E1 bEnt
	0x0A,	// 01: EH2 Enlist -> EH nEst
	0x0B,	// 02: EH1 hEAvy -> EH1 bElt
	0x00,	// 03: PA0 no sound -> PA
	0x28,	// 04: DT buTTer -> T Tart
	0x08,	// 05: A2 mAde -> A mAde
	0x08,	// 06: A1 mAde -> A mAde
	0x2F,	// 07: ZH aZure -> Z Zero
	0x0E,	// 08: AH2 hOnest -> AH gOt
	0x07,	// 09: I3 inhibIt -> I sIx
	0x07,	// 0A: I2 Inhibit -> I sIx
	0x07,	// 0B: I1 inhIbit -> I sIx
	0x37,	// 0C: M Mat -> More
	0x38,	// 0D: N suN -> N NiNe
	0x24,	// 0E: B Bag -> B Bag
	0x33,	// 0F: V Van -> V Very
	//
	0x32,	// 10: CH* CHip -> SCH SHip (!)
	0x32,	// 11: SH SHop ->  SCH SHip
	0x2F,	// 12: Z Zoo -> Z Zero
	0x10,	// 13: AW1 lAWful -> AW Office
	0x39,	// 14: NG thiNG -> NG raNG
	0x0F,	// 15: AH1 fAther -> AH1 fAther
	0x13,	// 16: OO1 lOOking -> OO lOOk
	0x13,	// 17: OO bOOK -> OO lOOk
	0x20,	// 18: L Land -> L Lift
	0x29,	// 19: K triCK -> Kit
	0x25,	// 1A: J* juDGe -> D paiD (!)
	0x2C,	// 1B: H Hello -> HF Heart
	0x26,	// 1C: G Get -> KV taG
	0x34,	// 1D: F Fast -> F Four
	0x25,	// 1E: D paiD -> D paiD
	0x30,	// 1F: S paSS -> S Same
	//
	0x08,	// 20: A dAY -> A mAde
	0x09,	// 21: AY dAY -> AI cAre
	0x03,	// 22: Y1 Yard -> YI Year
	0x1B,	// 23: UH3 missIOn -> UH3 nUt
	0x0E,	// 24: AH mOp -> AH gOt
	0x27,	// 25: P Past -> P Pen
	0x11,	// 26: O cOld -> O stOre
	0x07,	// 27: I pIn -> I sIx
	0x16,	// 28: U mOve -> U tUne
	0x05,	// 29: Y anY -> AY plEAse
	0x28,	// 2A: T Tap -> T Tart
	0x1D,	// 2B: R Red -> R Roof
	0x01,	// 2C: E mEEt -> E mEEt
	0x23,	// 2D: W Win -> W Water
	0x0C,	// 2E: AE dAd -> AE dAd
	0x0D,	// 2F: AE1 After -> AE1 After
	//
	0x10,	// 30: AW2 sAlty -> AW Office
	0x1A,	// 31: UH2 About -> UH2 whAt
	0x19,	// 32: UH1 Uncle -> UH1 lOve
	0x18,	// 33: UH cUp -> UH wOnder
	0x11,	// 34: O2 fOr -> O stOre
	0x11,	// 35: O1 abOArd -> O stOre
	0x14,	// 36: IU yOU -> IU yOU
	0x14,	// 37: U1 yOU -> IU yOU
	0x35,	// 38: THV THe -> THV THere
	0x36,	// 39: TH THin -> TH wiTH
	0x1C,	// 3A: ER bIrd -> ER bIrd
	0x0A,	// 3B: EH gEt -> EH nEst
	0x01,	// 3C: E1 bE -> E mEEt
	0x10,	// 3D: AW cAll -> AW Office
	0x00,	// 3E: PA1 no sound -> PA
	0x00,	// 3F: STOP no sound -> PA
};

void SSI263::Votrax_Write(BYTE value)
{
#if LOG_SC01
	LogOutput("SC01: %02X (= SSI263: %02X)\n", value, m_Votrax2SSI263[value & PHONEME_MASK]);
#endif

	if (!m_hasSC01)
		return;

	m_isVotraxPhoneme = true;
	m_votraxPhoneme = value & PHONEME_MASK;

	// !A/R: Acknowledge receipt of phoneme data (signal goes from high to low)
	UpdateIFR(m_device, SY6522::IxR_VOTRAX, 0);

	// NB. Don't set reg0.DUR, as SC01's phoneme duration doesn't change with pitch (empirically determined from MAME's SC01 emulation)
	//m_durationPhoneme = value;	// Set reg0.DUR = I1:0 (inflection or pitch)
	m_durationPhoneme = 0;
	Play(m_Votrax2SSI263[m_votraxPhoneme]);
}

//-----------------------------------------------------------------------------

void SSI263::Play(unsigned int nPhoneme)
{
	if (m_dbgFirst)
	{
		m_dbgStartTime = g_nCumulativeCycles;
#if LOG_SSI263 || LOG_SSI263B || LOG_SC01
		LogOutput("1st phoneme = 0x%02X\n", nPhoneme);
#endif
	}

#if LOG_SSI263 || LOG_SSI263B || LOG_SC01
	if (m_currentActivePhoneme != -1 && !(m_currentActivePhoneme & kPhonemeLeadoutFlag))
		LogOutput("Overlapping phonemes: current=%02X, next=%02X\n", m_currentActivePhoneme&0xff, nPhoneme&0xff);
#endif
	m_currentActivePhoneme = nPhoneme;

	bool bPause = false;

	if (nPhoneme == 1)
		nPhoneme = 2;	// Missing this sample, so map to phoneme-2

	if (nPhoneme == 0)
		bPause = true;
	else
		nPhoneme-=2;	// Missing phoneme-1

	m_phonemeLengthRemaining = g_nPhonemeInfo[nPhoneme].nLength;

	m_phonemeAccurateLengthRemaining = m_phonemeLengthRemaining;
	m_phonemePlaybackAndDebugger = (g_nAppMode == MODE_STEPPING || g_nAppMode == MODE_DEBUG);
	m_phonemeCompleteByFullSpeed = false;
	m_phonemeLeadoutLength = m_phonemeLengthRemaining / 10;	// Arbitrary! (TODO: determine a more accurate factor)

	if (bPause)
	{
		if (!m_pPhonemeData00)
		{
			// 'pause' length is length of 1st phoneme (arbitrary choice, since don't know real length)
			m_pPhonemeData00 = new short [m_phonemeLengthRemaining];
			memset(m_pPhonemeData00, 0x00, m_phonemeLengthRemaining*sizeof(short));
		}

		m_pPhonemeData = m_pPhonemeData00;
	}
	else
	{
		m_pPhonemeData = (const short*) &g_nPhonemeData[g_nPhonemeInfo[nPhoneme].nOffset];
	}

	m_currSampleSum = 0;
	m_currNumSamples = 0;
	m_currSampleMod4 = 0;

	// Set m_lastUpdateCycle, otherwise UpdateAccurateLength() can immediately complete phoneme! (GH#1104)
	m_lastUpdateCycle = GetLastCumulativeCycles();
}

void SSI263::Stop(void)
{
	if (SSI263SingleVoice.lpDSBvoice && SSI263SingleVoice.bActive)
		DSVoiceStop(&SSI263SingleVoice);
}

//-----------------------------------------------------------------------------

void SSI263::PeriodicUpdate(UINT executedCycles)
{
	const UINT kCyclesPerAudioFrame = 1000;
	m_cyclesThisAudioFrame += executedCycles;
	if (m_cyclesThisAudioFrame < kCyclesPerAudioFrame)
		return;

	m_cyclesThisAudioFrame %= kCyclesPerAudioFrame;

	Update();
}

//-----------------------------------------------------------------------------

//#define DBG_SSI263_UPDATE		// NB. This outputs for all active SSI263 ring-buffers (eg. for mb-audit this may be 2 or 4)
//#define DBG_SSI263_UPDATE_RETURN

// Called by:
// . PeriodicUpdate()
void SSI263::Update(void)
{
	if (!IsPhonemeActive())
		return;

	if (!SSI263SingleVoice.lpDSBvoice || !SSI263SingleVoice.bActive)
	{
		if (!DSInit())
			return;
	}

	UpdateAccurateLength();

	if (g_bFullSpeed)	// NB. if true, then it's irrespective of IsPhonemeActive() - see MockingboardCard::IsActiveToPreventFullSpeed()
	{
		if (m_phonemeLengthRemaining)
		{
			// Willy Byte does SSI263 detection with drive motor on
			m_phonemeLengthRemaining = 0;
#if LOG_SSI263 || LOG_SSI263B || LOG_SC01
			if (m_dbgFirst) LogOutput("1st phoneme short-circuited by fullspeed\n");
#endif

			if (m_phonemeAccurateLengthRemaining)
				m_phonemeCompleteByFullSpeed = true;	// Let UpdateAccurateLength() call UpdateIRQ()
			else
				UpdateIRQ();
		}

		m_updateWasFullSpeed = true;
		return;
	}

	//

	const bool nowNormalSpeed = m_updateWasFullSpeed;	// Just transitioned from full-speed to normal speed
	m_updateWasFullSpeed = false;

	// NB. next call to this function: nowNormalSpeed = false
	if (nowNormalSpeed)
		m_byteOffset = (uint32_t)-1;	// ...which resets m_numSamplesError below

	//-------------

	DWORD dwCurrentPlayCursor, dwCurrentWriteCursor;
	HRESULT hr = SSI263SingleVoice.lpDSBvoice->GetCurrentPosition(&dwCurrentPlayCursor, &dwCurrentWriteCursor);
	if (FAILED(hr))
	{
		LogOutput("SSI263::Update() early return: GetCurrentPosition() failed\n");
		return;
	}

	bool prefillBufferOnInit = false;

	if (m_byteOffset == (uint32_t)-1)
	{
		// First time in this func (or transitioned from full-speed to normal speed, or a ring-buffer reset)
#ifdef DBG_SSI263_UPDATE
		double fTicksSecs = (double)GetTickCount() / 1000.0;
		LogOutput("%010.3f: [SSUpdtInit%1d]PC=%08X, WC=%08X, Diff=%08X, Off=%08X xxx\n", fTicksSecs, m_device, dwCurrentPlayCursor, dwCurrentWriteCursor, dwCurrentWriteCursor - dwCurrentPlayCursor, m_byteOffset);
#endif
		m_byteOffset = dwCurrentWriteCursor;
		m_numSamplesError = 0;
		prefillBufferOnInit = true;
	}
	else
	{
		// Check that our offset isn't between Play & Write positions

		if (dwCurrentWriteCursor > dwCurrentPlayCursor)
		{
			// |-----PxxxxxW-----|
			if ((m_byteOffset > dwCurrentPlayCursor) && (m_byteOffset < dwCurrentWriteCursor))
			{
#ifdef DBG_SSI263_UPDATE
				double fTicksSecs = (double)GetTickCount() / 1000.0;
				LogOutput("%010.3f: [SSUpdt%1d]    PC=%08X, WC=%08X, Diff=%08X, Off=%08X xxx\n", fTicksSecs, m_device, dwCurrentPlayCursor, dwCurrentWriteCursor, dwCurrentWriteCursor - dwCurrentPlayCursor, m_byteOffset);
#endif
				m_byteOffset = dwCurrentWriteCursor;
				m_numSamplesError = 0;
			}
		}
		else
		{
			// |xxW----------Pxxx|
			if ((m_byteOffset > dwCurrentPlayCursor) || (m_byteOffset < dwCurrentWriteCursor))
			{
#ifdef DBG_SSI263_UPDATE
				double fTicksSecs = (double)GetTickCount() / 1000.0;
				LogOutput("%010.3f: [SSUpdt%1d]    PC=%08X, WC=%08X, Diff=%08X, Off=%08X XXX\n", fTicksSecs, m_device, dwCurrentPlayCursor, dwCurrentWriteCursor, dwCurrentWriteCursor - dwCurrentPlayCursor, m_byteOffset);
#endif
				m_byteOffset = dwCurrentWriteCursor;
				m_numSamplesError = 0;
			}
		}
	}

	//-------------

	const UINT kMinBytesInBuffer = m_kDSBufferByteSize / 4;	// 25% full
	int nNumSamples = 0;
	double updateInterval = 0.0;

	if (prefillBufferOnInit)
	{
		// Just prefill first 25% of buffer with zeros:
		// . so we have a quarter buffer of silence/lag before the real sample data begins.
		// . NB. this is fine, since it's the steady state; and it's likely that no actual data will ever occur during this initial time.
		// This means that the '1st phoneme playback time' (in cycles) will be a bit longer for subsequent times.

		m_lastUpdateCycle = GetLastCumulativeCycles();

		nNumSamples = kMinBytesInBuffer / sizeof(short);
		memset(&m_mixBufferSSI263[0], 0, nNumSamples);
	}
	else
	{
		// For small timer periods, wait for a period of 500cy before updating DirectSound ring-buffer.
		// NB. A timer period of less than 24cy will yield nNumSamplesPerPeriod=0.
		const double kMinimumUpdateInterval = 500.0;	// Arbitary (500 cycles = 21 samples)
		const double kMaximumUpdateInterval = (double)(0xFFFF + 2);	// Max 6522 timer interval (1372 samples)

		_ASSERT(GetLastCumulativeCycles() >= m_lastUpdateCycle);
		updateInterval = (double)(GetLastCumulativeCycles() - m_lastUpdateCycle);
		if (updateInterval < kMinimumUpdateInterval)
		{
#ifdef DBG_SSI263_UPDATE_RETURN
			LogOutput("SSI263::Update() early return: updateInterval < kMinimumUpdateInterval\n");
#endif
			return;
		}
		if (updateInterval > kMaximumUpdateInterval)
			updateInterval = kMaximumUpdateInterval;

		m_lastUpdateCycle = GetLastCumulativeCycles();

		const double nIrqFreq = g_fCurrentCLK6502 / updateInterval + 0.5;			// Round-up
		const int nNumSamplesPerPeriod = (int)((double)(SAMPLE_RATE_SSI263) / nIrqFreq);	// Eg. For 60Hz this is 367

		nNumSamples = nNumSamplesPerPeriod + m_numSamplesError;						// Apply correction
		if (nNumSamples <= 0)
			nNumSamples = 0;
		if (nNumSamples > 2 * nNumSamplesPerPeriod)
			nNumSamples = 2 * nNumSamplesPerPeriod;

		if (nNumSamples > m_kDSBufferByteSize / sizeof(short))
			nNumSamples = m_kDSBufferByteSize / sizeof(short);	// Clamp to prevent buffer overflow

//		if (nNumSamples)
//		{ /* Generate new sample data - ie. could merge from all the SSI263 sources */ }

		//

		int nBytesRemaining = m_byteOffset - dwCurrentPlayCursor;
		if (nBytesRemaining < 0)
			nBytesRemaining += m_kDSBufferByteSize;

		// Calc correction factor so that play-buffer doesn't under/overflow
		const int nErrorInc = SoundCore_GetErrorInc();
		if (nBytesRemaining < kMinBytesInBuffer)
			m_numSamplesError += nErrorInc;				// < 0.25 of buffer remaining
		else if (nBytesRemaining > m_kDSBufferByteSize / 2)
			m_numSamplesError -= nErrorInc;				// > 0.50 of buffer remaining
		else
			m_numSamplesError = 0;						// Acceptable amount of data in buffer
	}

#if defined(DBG_SSI263_UPDATE)
	double fTicksSecs = (double)GetTickCount() / 1000.0;
	LogOutput("%010.3f: [SSUpdt%1d]    PC=%08X, WC=%08X, Diff=%08X, Off=%08X, NS=%08X, NSE=%08X, Interval=%f\n", fTicksSecs, m_device, dwCurrentPlayCursor, dwCurrentWriteCursor, dwCurrentWriteCursor - dwCurrentPlayCursor, m_byteOffset, nNumSamples, m_numSamplesError, updateInterval);
#endif

	if (nNumSamples == 0)
	{
		if (m_numSamplesError)
		{
			// Reset ring-buffer if we've had a major interruption, eg. F7 (enter debugger), F8 (configure), F11/12 (save-state), Pause, etc
			// - this can cause Apple II SSI263 detection code to fail (when either timing one or a sequence of phonemes)
			// When the AppleWin code restarts and reads the ring-buffer position it'll be at a random point, and maybe nearly full (>50% full)
			// - so the code waits until it drains (nNumSamples=0 each time)
			// - but it takes a large number of calls to this func to drain to an acceptable level
			m_byteOffset = (uint32_t)-1;
#if defined(DBG_SSI263_UPDATE)
			double fTicksSecs = (double)GetTickCount() / 1000.0;
			LogOutput("%010.3f: [SSUpdt%1d]    Reset ring-buffer\n", fTicksSecs, m_device);
#endif
		}
#ifdef DBG_SSI263_UPDATE_RETURN
		LogOutput("SSI263::Update() early return: nNumSamples == 0\n");
#endif
		return;
	}

	//-------------

	const double amplitude = m_isVotraxPhoneme ? 1.0
		: m_ctrlArtAmp & CONTROL_MASK ? 0.0		// Power-down / standby
		: m_filterFreq == FILTER_FREQ_SILENCE ? 0.0
		: (double)(m_ctrlArtAmp & AMPLITUDE_MASK) / (double)AMPLITUDE_MASK;

	bool bSpeechIRQ = false;

	{
		const BYTE DUR = (m_currentMode.function == (MODE_FRAME_IMMEDIATE_INFLECTION >> DURATION_MODE_SHIFT)) ? 3	// Frame timing mode
						: m_durationPhoneme >> DURATION_MODE_SHIFT;	// Phoneme timing mode
		const BYTE numSamplesToAvg = (DUR <= 1) ? 1 :
									 (DUR == 2) ? 2 :
												  4;

		short* pMixBuffer = &m_mixBufferSSI263[0];
		UINT zeroSize = nNumSamples;

		// NB. If SSI263.CONTROL=1 (Power-down) then zeroSize == nNumSamples, and eventually the ring-buffer gets filled with zero samples

		if (m_phonemeLengthRemaining && !prefillBufferOnInit)
		{
			UINT samplesWritten = 0;
			while (samplesWritten < (UINT)nNumSamples)
			{
				double sample = (double)*m_pPhonemeData * amplitude;
				m_currSampleSum += (int)sample;
				m_currNumSamples++;

				m_pPhonemeData++;
				m_phonemeLengthRemaining--;

				if (m_currNumSamples == numSamplesToAvg)
				{
					*pMixBuffer++ = (short)(m_currSampleSum / numSamplesToAvg);
					samplesWritten++;
					m_currSampleSum = 0;
					m_currNumSamples = 0;
				}

				m_currSampleMod4 = (m_currSampleMod4 + 1) & 3;
				if (DUR == 1 && m_currSampleMod4 == 3 && m_phonemeLengthRemaining)
				{
					m_pPhonemeData++;
					m_phonemeLengthRemaining--;
				}

				if (!m_phonemeLengthRemaining)
				{
					bSpeechIRQ = true;
					break;
				}
			}

			zeroSize = nNumSamples - samplesWritten;
			_ASSERT(zeroSize >= 0);
		}

		if (zeroSize)
		{
			memset(pMixBuffer, 0, zeroSize * sizeof(short));

			// Only dec m_phonemeLeadoutLength when m_phonemeAccurateLengthRemaining==0
			// . otherwise when single-stepping can get into the situation where m_phonemeLengthRemaining==0 && m_phonemeAccurateLengthRemaining!=0
			if (!prefillBufferOnInit && !m_phonemeAccurateLengthRemaining)
				m_phonemeLeadoutLength -= (m_phonemeLeadoutLength > zeroSize) ? zeroSize : m_phonemeLeadoutLength;
		}
	}

	//

	DWORD dwDSLockedBufferSize0, dwDSLockedBufferSize1;
	short *pDSLockedBuffer0, *pDSLockedBuffer1;

	hr = DSGetLock(SSI263SingleVoice.lpDSBvoice,
		m_byteOffset, (uint32_t)nNumSamples * sizeof(short) * m_kNumChannels,
		&pDSLockedBuffer0, &dwDSLockedBufferSize0,
		&pDSLockedBuffer1, &dwDSLockedBufferSize1);
	if (FAILED(hr))
	{
		LogOutput("SSI263::Update() early return: DSGetLock() failed\n");
		return;
	}

	memcpy(pDSLockedBuffer0, &m_mixBufferSSI263[0], dwDSLockedBufferSize0);
	if (pDSLockedBuffer1)
		memcpy(pDSLockedBuffer1, &m_mixBufferSSI263[dwDSLockedBufferSize0/sizeof(short)], dwDSLockedBufferSize1);

	// Commit sound buffer
	hr = SSI263SingleVoice.lpDSBvoice->Unlock((void*)pDSLockedBuffer0, dwDSLockedBufferSize0,
											  (void*)pDSLockedBuffer1, dwDSLockedBufferSize1);
	if (FAILED(hr))
	{
		LogOutput("SSI263::Update() early return: UnLock() failed\n");
		return;
	}

	m_byteOffset = (m_byteOffset + (uint32_t)nNumSamples*sizeof(short)*m_kNumChannels) % m_kDSBufferByteSize;

	//

	if (bSpeechIRQ)
	{
		// NB. if m_phonemePlaybackAndDebugger==true, then "m_phonemeAccurateLengthRemaining!=0" must be true.
		// Since in UpdateAccurateLength(), (when m_phonemePlaybackAndDebugger==true) then m_phonemeAccurateLengthRemaining decs to zero.
#if _DEBUG
		if (m_phonemePlaybackAndDebugger)
		{
			_ASSERT(m_phonemeAccurateLengthRemaining);	// Check this!
		}
#endif
		if (!m_phonemePlaybackAndDebugger /*|| m_phonemeAccurateLengthRemaining*/)	// superfluous, so commented out (see above)
		{
			UpdateIRQ();
		}
	}

	RepeatPhoneme();
}

// Called by:
// . Update()
// . LoadSnapshot()
void SSI263::RepeatPhoneme(void)
{
	if (m_phonemeLeadoutLength != 0)
		return;

	if (!m_isVotraxPhoneme)
	{
		if ((m_ctrlArtAmp & CONTROL_MASK) == 0)
		{
			// Only check _ASSERT when SSI263.CONTROL=0 (otherwise kPhonemeLeadoutFlag will be clear 2nd time Update() is called)
			_ASSERT(m_currentActivePhoneme & kPhonemeLeadoutFlag);
			Play(m_durationPhoneme & PHONEME_MASK);		// Repeat this phoneme again
		}

		m_currentActivePhoneme &= PHONEME_MASK;			// Clear kPhonemeLeadoutFlag
	}
	else
	{
		Play(m_Votrax2SSI263[m_votraxPhoneme]);		// Votrax phoneme repeats too (tested in MAME 0.262)
	}
}

//-----------------------------------------------------------------------------

// The primary way for phonemes to generate IRQ is via the ring-buffer in Update(),
// but when single-stepping (eg. timing-sensitive SSI263 detection code), then this secondary method is used.
void SSI263::UpdateAccurateLength(void)
{
	if (!m_phonemeAccurateLengthRemaining)
		return;

	double updateInterval = (double)(GetLastCumulativeCycles() - m_lastUpdateCycle);

	const double nIrqFreq = g_fCurrentCLK6502 / updateInterval + 0.5;			// Round-up
	const int nNumSamplesPerPeriod = (int)((double)(SAMPLE_RATE_SSI263) / nIrqFreq);	// Eg. For 60Hz this is 367

	const BYTE DUR = m_durationPhoneme >> DURATION_MODE_SHIFT;

	const UINT numSamples = nNumSamplesPerPeriod * (DUR+1);
	if (m_phonemeAccurateLengthRemaining > numSamples)
	{
		m_phonemeAccurateLengthRemaining -= numSamples;
	}
	else
	{
		m_phonemeAccurateLengthRemaining = 0;
		if (m_phonemePlaybackAndDebugger || m_phonemeCompleteByFullSpeed)
			UpdateIRQ();
	}
}

// Called by:
// . Update() when m_phonemeLengthRemaining -> 0
// . UpdateAccurateLength() when m_phonemeAccurateLengthRemaining -> 0
// . LoadSnapshot()
void SSI263::UpdateIRQ(void)
{
	m_phonemeLengthRemaining = m_phonemeAccurateLengthRemaining = 0;	// Prevent an IRQ from the other source

	_ASSERT(m_currentActivePhoneme != -1);
	_ASSERT((m_currentActivePhoneme & kPhonemeLeadoutFlag) == 0);
	m_currentActivePhoneme |= kPhonemeLeadoutFlag;

	if (m_dbgFirst && m_dbgStartTime)
	{
#if LOG_SSI263 || LOG_SSI263B || LOG_SC01
		UINT64 diff = g_nCumulativeCycles - m_dbgStartTime;
		LogOutput("1st phoneme playback time = 0x%08X cy\n", (UINT32)diff);
#endif
		m_dbgFirst = false;
	}

	// Phoneme complete, so generate IRQ if necessary
	SetSpeechIRQ();
}

//-----------------------------------------------------------------------------

// Pre: m_isVotraxPhoneme, m_cardMode, m_device
void SSI263::SetSpeechIRQ(void)
{
	if (!m_isVotraxPhoneme && (m_ctrlArtAmp & CONTROL_MASK) == 0)
	{
		if (m_currentMode.enableInts)
		{
			if (m_cardMode == PH_Mockingboard)
			{
				if (m_currentMode.D7 == 0)
				{
					// 6522's PCR = 0x0C (all SSI263 speech routine use this value, but 0x00 will do equally as well!)
					// . b3:1 CA2 Control = b#110 (Low output) - not connected
					// . b0   CA1 Latch/Input = 0 (Negative active edge) - input from SSI263's A/!R
					if ((GetPCR(m_device) & 1) == 0)		// Level change from SSI263's A/!R, latch this as an interrupt
						UpdateIFR(m_device, 0, SY6522::IxR_SSI263);
				}
			}
			else if (m_cardMode == PH_Phasor)
			{
				// Phasor (in native mode): SSI263 IRQ (A/!R) pin is connected directly to the 6502's IRQ
				// . And Mockingboard mode: A/!R is connected to the 6522's CA1
				CpuIrqAssert(IS_SPEECH);
			}
			else
			{
				_ASSERT(m_cardMode == PH_EchoPlus);
				// SSI263 not visible from Echo+ mode, but still continues to operate
			}
		}

		// Always set SSI263's D7 pin regardless of SSI263 mode (DR1:0), including when SSI263 ints are disabled (via MODE_IRQ_DISABLED)
		// NB. Don't set D7 when in power-down / standby mode.
		m_currentMode.D7 = 1;
	}

	//

	if (m_isVotraxPhoneme && GetPCR(m_device) == 0xB0)
	{
		// !A/R: Time-out of old phoneme (signal goes from low to high)
		UpdateIFR(m_device, 0, SY6522::IxR_VOTRAX);
	}
}

//-----------------------------------------------------------------------------

void SSI263::SetCardMode(PHASOR_MODE mode)
{
	const PHASOR_MODE oldCardMode = m_cardMode;
	m_cardMode = mode;

	if (oldCardMode == m_cardMode)
		return;

	// mode change

	if (m_currentMode.D7 == 1)
	{
		m_currentMode.D7 = 0;	// So that \PH_Mockingboard\ path sets IFR. Post: D7=1
		SetSpeechIRQ();
	}

	if (m_cardMode != PH_Phasor)
		CpuIrqDeassert(IS_SPEECH);
}

//-----------------------------------------------------------------------------

// Cf. void MockingboardCardManager::UpdateSoundBuffer(void)
bool SSI263::DSInit(void)
{
	if (!SSI263SingleVoice.lpDSBvoice)
	{
		if (g_bDisableDirectSound || g_bDisableDirectSoundMockingboard)
			return false;

		if (!Init())
			return false;
	}

	if (!SSI263SingleVoice.bActive)
	{
		bool bRes = DSZeroVoiceBuffer(&SSI263SingleVoice, m_kDSBufferByteSize);	// ... and Play()
		LogFileOutput("SSI263: DSZeroVoiceBuffer(), res=%d\n", bRes ? 1 : 0);
		if (!bRes)
			return false;
	}

	return true;
}

// Cf. bool MockingboardCardManager::Init(void)
bool SSI263::Init(void)
{
	if (!DSAvailable())
		return false;

	HRESULT hr = DSGetSoundBuffer(&SSI263SingleVoice, m_kDSBufferByteSize, SAMPLE_RATE_SSI263, m_kNumChannels, "SSI263");
	LogFileOutput("SSI263: DSGetSoundBuffer(), hr=0x%08X\n", hr);
	if (FAILED(hr))
	{
		LogFileOutput("SSI263: DSGetSoundBuffer failed (%08X)\n", hr);
		return false;
	}

	// Don't DirectSoundBuffer::Play() via DSZeroVoiceBuffer() - instead wait until this SSI263 is actually first used
	// . different to Speaker & Mockingboard ring buffers
	// . NB. we have 2x SSI263 per MB card, and it's rare if 1 is used (and *extremely* rare if 2 are used!)
	// . Not so rare, as TotalReplay (at boot) will try to detect an SSI263 (by playing a $00 phoneme).

	hr = SSI263SingleVoice.lpDSBvoice->SetVolume(SSI263SingleVoice.nVolume);
	LogFileOutput("SSI263::DSInit: SetVolume(), hr=0x%08X\n", hr);

	return true;
}

void SSI263::DSUninit(void)
{
	Stop();
	DSReleaseSoundBuffer(&SSI263SingleVoice);
}

//-----------------------------------------------------------------------------

// MB-C/Phasor/SSI263P phoneme continues to play after CTRL+RESET, whereas Phasor/SSI263AP doesn't (tested on real h/w)
// Votrax phoneme continues to play after CTRL+RESET (tested on MAME 0.262)
void SSI263::Reset(const bool powerCycle, const bool isPhasorCard)
{
	if (m_type == SSI263Empty)
		return;

	if (!powerCycle && m_type == SSI263P)
	{
		if (isPhasorCard)
		{
			// [SSI263P] Empirically observed it does CTL H->L to enable ints (and set the device mode?) (GH#175)
			// NB. CTRL+RESET doesn't clear m_ctrlArtAmp.CTL (ie. if the device is in power-down/standby mode then ignore RST)
			// There's a bug in the SSI263P and RST should put the device into power-down/standby mode (ie. silence the device)
			// TODO: Stick a 'scope on !PD/!RST pin 18 to see what the Phasor h/w does.
			if ((m_ctrlArtAmp & CONTROL_MASK) == 0)
				SetDeviceModeAndInts();
		}

		return;
	}

	Stop();
	ResetState(powerCycle);
	CpuIrqDeassert(IS_SPEECH);
}

//-----------------------------------------------------------------------------

void SSI263::Mute(void)
{
	if (SSI263SingleVoice.bActive && !SSI263SingleVoice.bMute)
	{
		SSI263SingleVoice.lpDSBvoice->SetVolume(DSBVOLUME_MIN);
		SSI263SingleVoice.bMute = true;
	}
}

void SSI263::Unmute(void)
{
	if (SSI263SingleVoice.bActive && SSI263SingleVoice.bMute)
	{
		SSI263SingleVoice.lpDSBvoice->SetVolume(SSI263SingleVoice.nVolume);
		SSI263SingleVoice.bMute = false;
	}
}

void SSI263::SetVolume(uint32_t dwVolume, uint32_t dwVolumeMax)
{
	SSI263SingleVoice.dwUserVolume = dwVolume;

	SSI263SingleVoice.nVolume = NewVolume(dwVolume, dwVolumeMax);

	if (SSI263SingleVoice.bActive && !SSI263SingleVoice.bMute)
		SSI263SingleVoice.lpDSBvoice->SetVolume(SSI263SingleVoice.nVolume);
}

//=============================================================================

#define SS_YAML_KEY_SSI263 "SSI263"
// NB. No version - this is determined by the parent "Mockingboard C" or "Phasor" unit

#define SS_YAML_KEY_SSI263_REG_DUR_PHON "Duration / Phoneme"
#define SS_YAML_KEY_SSI263_REG_INF "Inflection"
#define SS_YAML_KEY_SSI263_REG_RATE_INF "Rate / Inflection"
#define SS_YAML_KEY_SSI263_REG_CTRL_ART_AMP "Control / Articulation / Amplitude"
#define SS_YAML_KEY_SSI263_REG_FILTER_FREQ "Filter Frequency"
#define SS_YAML_KEY_SSI263_CURRENT_MODE "Current Mode"
#define SS_YAML_KEY_SSI263_ACTIVE_PHONEME "Active Phoneme"	// v13: deprecated
#define SS_YAML_KEY_SSI263_TYPE "Type"	// v14

#define TYPE_SSI263_EMPTY "Empty"
#define TYPE_SSI263_P "SSI263P"
#define TYPE_SSI263_AP "SSI263AP"

void SSI263::SaveSnapshot(YamlSaveHelper& yamlSaveHelper, UINT subunit)
{
	// Scope for SSI263 subunit (so that SC01 has same indentation)
	{
		YamlSaveHelper::Label label(yamlSaveHelper, "%s:\n", SS_YAML_KEY_SSI263);

		std::string type = m_type == SSI263Empty ? TYPE_SSI263_EMPTY
			: m_type == SSI263P ? TYPE_SSI263_P
			: TYPE_SSI263_AP;

		yamlSaveHelper.SaveString(SS_YAML_KEY_SSI263_TYPE, type);

		if (m_type != SSI263Empty)
		{
			yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SSI263_REG_DUR_PHON, m_durationPhoneme);
			yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SSI263_REG_INF, m_inflection);
			yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SSI263_REG_RATE_INF, m_rateInflection);
			yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SSI263_REG_CTRL_ART_AMP, m_ctrlArtAmp);
			yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SSI263_REG_FILTER_FREQ, m_filterFreq);
			yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SSI263_CURRENT_MODE, m_currentMode.mode);
		}
	}

	if (subunit == 0)	// has SC01
		SC01_SaveSnapshot(yamlSaveHelper);
}

void SSI263::LoadSnapshot(YamlLoadHelper& yamlLoadHelper, PHASOR_MODE mode, UINT version, UINT subunit)
{
	if (!yamlLoadHelper.GetSubMap(SS_YAML_KEY_SSI263))
		throw std::runtime_error("Card: Expected key: " SS_YAML_KEY_SSI263);

	std::string type = TYPE_SSI263_P;	// Default prior to v14
	if (version >= 14)
		type = yamlLoadHelper.LoadString(SS_YAML_KEY_SSI263_TYPE);

	if (type == TYPE_SSI263_EMPTY)
		m_type = SSI263Empty;
	else if (type == TYPE_SSI263_P)
		m_type = SSI263P;
	else // TYPE_SSI263_AP
		m_type = SSI263AP;

	if (m_type != SSI263Empty)
	{
		m_durationPhoneme = yamlLoadHelper.LoadUint(SS_YAML_KEY_SSI263_REG_DUR_PHON);
		m_inflection = yamlLoadHelper.LoadUint(SS_YAML_KEY_SSI263_REG_INF);
		m_rateInflection = yamlLoadHelper.LoadUint(SS_YAML_KEY_SSI263_REG_RATE_INF);
		m_ctrlArtAmp = yamlLoadHelper.LoadUint(SS_YAML_KEY_SSI263_REG_CTRL_ART_AMP);
		m_filterFreq = yamlLoadHelper.LoadUint(SS_YAML_KEY_SSI263_REG_FILTER_FREQ);
		m_currentMode.mode = yamlLoadHelper.LoadUint(SS_YAML_KEY_SSI263_CURRENT_MODE);

		if (version >= 7 && version < 13)
			yamlLoadHelper.LoadBool(SS_YAML_KEY_SSI263_ACTIVE_PHONEME);	// Consume redundant data

		if (version < 12)
		{
			if (m_currentMode.function == 0)	// invalid function (but in older versions this was accepted)
			{
				m_currentMode.function = MODE_PHONEME_TRANSITIONED_INFLECTION >> DURATION_MODE_SHIFT;	// Typically this is used
				m_currentMode.enableInts = 0;
			}
			else
			{
				m_currentMode.enableInts = 1;
			}
		}

		if ((m_ctrlArtAmp & CONTROL_MASK) == 0)
			m_currentActivePhoneme = m_durationPhoneme & PHONEME_MASK;
	}

	yamlLoadHelper.PopMap();

	//

	_ASSERT(m_device != BYTE(-1));
	SetCardMode(mode);

	// Only need to directly assert IRQ for Phasor mode (for Mockingboard mode it's done via UpdateIFR() in parent)
	if (m_cardMode == PH_Phasor && IsPhonemeActive() && m_currentMode.enableInts && m_currentMode.D7 == 1)
		CpuIrqAssert(IS_SPEECH);

	if (subunit == 0)	// has SC01
		SC01_LoadSnapshot(yamlLoadHelper, version);

	// Do this after loading both SSI263 & SC01 state, otherwise IsPhonemeActive() can indicate both are active!
	// . EG. After loading SSI263, SSI263.CONTROL==0 (so active); and after loading SC01, m_isVotraxPhoneme==true (so active)

	if (m_isVotraxPhoneme)
		m_currentActivePhoneme = 0x00;	// For IsPhonemeActive() and SC01 chip

	if (IsPhonemeActive())
	{
		// NB. Save-state doesn't preserve the play-position within the phoneme.
		// It just sets IRQ (and SSI263.D7) for "phoneme complete"; and restarts it from the beginning.
		// This may cause problems for timing sensitive code (eg. mb-audit).
		m_currentActivePhoneme = 0x00;	// Not important which phoneme, since RepeatPhoneme()->Play() sets this
		UpdateIRQ();		// Pre: m_device, m_cardMode
		RepeatPhoneme();
	}

	m_lastUpdateCycle = GetLastCumulativeCycles();
}

//=============================================================================

#define SS_YAML_KEY_SC01 "SC01"
// NB. No version - this is determined by the parent "Mockingboard C" or "Phasor" unit

#define SS_YAML_KEY_SC01_PHONEME "SC01 Phoneme"
#define SS_YAML_KEY_SC01_ACTIVE_PHONEME "SC01 Active Phoneme"
#define SS_YAML_KEY_SC01_TYPE "Type"	// v14

#define TYPE_SC01_EMPTY "Empty"
#define TYPE_SC01 "SC01"

void SSI263::SC01_SaveSnapshot(YamlSaveHelper& yamlSaveHelper)
{
	YamlSaveHelper::Label label(yamlSaveHelper, "%s:\n", SS_YAML_KEY_SC01);

	std::string type = !m_hasSC01 ? TYPE_SC01_EMPTY : TYPE_SC01;
	yamlSaveHelper.SaveString(SS_YAML_KEY_SC01_TYPE, type);

	if (m_hasSC01)
	{
		yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_SC01_PHONEME, m_votraxPhoneme);
		yamlSaveHelper.SaveBool(SS_YAML_KEY_SC01_ACTIVE_PHONEME, m_isVotraxPhoneme);
	}
}

void SSI263::SC01_LoadSnapshot(YamlLoadHelper& yamlLoadHelper, UINT version)
{
	if (version < 12)
	{
		m_votraxPhoneme = 0;
		// NB. m_isVotraxPhoneme already set by SetVotraxPhoneme() by parent
		return;
	}

	if (!yamlLoadHelper.GetSubMap(SS_YAML_KEY_SC01))
		throw std::runtime_error("Card: Expected key: " SS_YAML_KEY_SC01);

	std::string type = TYPE_SC01;	// Default prior to v14
	if (version >= 14)
		type = yamlLoadHelper.LoadString(SS_YAML_KEY_SC01_TYPE);

	m_hasSC01 = (type == TYPE_SC01);

	if (m_hasSC01)
	{
		m_votraxPhoneme = yamlLoadHelper.LoadUint(SS_YAML_KEY_SC01_PHONEME);
		m_isVotraxPhoneme = yamlLoadHelper.LoadBool(SS_YAML_KEY_SC01_ACTIVE_PHONEME);
	}

	yamlLoadHelper.PopMap();
}
