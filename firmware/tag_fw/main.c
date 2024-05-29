#define __packed
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asmUtil.h"
#include "comms.h"  // for mLastLqi and mLastRSSI
#include "eeprom.h"
#include "i2c.h"
#include "i2cdevices.h"
#include "powermgt.h"
#include "printf.h"

#include "radio.h"
#include "screen.h"
#include "settings.h"
#include "syncedproto.h"
#include "timer.h"
#include "userinterface.h"
#include "wdt.h"

#include "flash.h"

#include "uart.h"

#include "../shared/oepl-definitions.h"
#include "../shared/oepl-proto.h"

// #define DEBUG_MODE


#ifdef STOCKFWOPTIONS
#define FWMAGICOFFSET 0x048b
#else
#define FWMAGICOFFSET 0x008b
#endif

static const uint64_t __code __at(FWMAGICOFFSET) firmwaremagic = (0xdeadd0d0beefcafeull) + HW_TYPE;


//from zigbee_synced_clock


static uint8_t __xdata mCommsBuf[127];
static uint8_t __xdata mRxBuf[COMMS_MAX_PACKET_SZ];
uint32_t current_unix_time = 0;
uint32_t one_second_trimmed = TIMER_TICKS_PER_SECOND;
uint32_t last_clock_increase = 0;
__xdata uint8_t last_minute = 0;
__xdata uint8_t last_hour = 0;
uint8_t was_never_synced = 0;

__xdata uint32_t last_synced_time = 0;
__xdata int32_t last_time_offset = 0;
__xdata uint32_t seconds_since_last_sync = 0;
uint8_t dsn = 0;
uint8_t __xdata packetp[128];

#ifdef DEBUGGUI
void displayLoop() {
    powerUp(INIT_BASE | INIT_UART);

    pr("Splash screen\n");
    powerUp(INIT_EPD);
    showSplashScreen();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);

    pr("Update screen\n");
    powerUp(INIT_EPD);
    showApplyUpdate();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);

    wdt30s();

    pr("Failed update screen\n");
    powerUp(INIT_EPD);
    showFailedUpdate();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);
    wdt30s();

    pr("AP Found\n");
    powerUp(INIT_EPD);
    showAPFound();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);

    wdt30s();

    pr("AP NOT Found\n");
    powerUp(INIT_EPD);
    showNoAP();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);

    wdt30s();

    pr("Longterm sleep screen\n");
    powerUp(INIT_EPD);
    showLongTermSleep();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);

    wdt30s();

    pr("NO EEPROM\n");
    powerUp(INIT_EPD);
    showNoEEPROM();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);

    wdt30s();

    pr("NO MAC\n");
    powerUp(INIT_EPD);
    showNoMAC();
    timerDelay(TIMER_TICKS_PER_SECOND * 4);
    wdtDeviceReset();
}

#endif

void validateMacAddress() {
    // check if the mac contains at least some non-0xFF values
    for (uint8_t __xdata c = 0; c < 8; c++) {
        if (mSelfMac[c] != 0xFF) goto macIsValid;
    }
// invalid mac address. Display warning screen and sleep forever
#ifdef DEBUGMAIN
    pr("Mac can't be all FF's.\n");
#endif
    powerUp(INIT_EPD);
    showNoMAC();
    powerDown(INIT_EPD | INIT_UART | INIT_EEPROM);
    doSleep(-1);
    wdtDeviceReset();
macIsValid:
    return;
}
uint8_t getFirstWakeUpReason() {
    if (RESET & 0x01) {
#ifdef DEBUGMAIN
        pr("WDT reset!\n");
#endif
        return WAKEUP_REASON_WDT_RESET;
    }
    return WAKEUP_REASON_FIRSTBOOT;
}

void detectButtonOrJig() {
    switch (checkButtonOrJig()) {
        case DETECT_P1_0_BUTTON:
            capabilities |= CAPABILITY_HAS_WAKE_BUTTON;
            break;
        case DETECT_P1_0_JIG:
            wdt120s();
            // show splashscreen
            powerUp(INIT_EPD);
            afterFlashScreenSaver();
            while (1)
                ;
            break;
        case DETECT_P1_0_NOTHING:
            break;
        default:
            break;
    }
}

void executeCommand(uint8_t cmd) {
    (void)cmd;
}

void request_time_via_zigbee()
{
	if (!boardGetOwnMac(mSelfMac))
	{
		pr("failed to get MAC. Aborting\n");
		return;
	}
	radioInit();

	radioRxFilterCfg(mSelfMac, 0x10000, 0x3443);
	radioSetChannel(RADIO_FIRST_CHANNEL);
	radioSetTxPower(10);
	radioRxEnable(true, true);

	pr("Sending Time request to AP now\r\n");
	packetp[0] = 0x01;
	packetp[1] = 0xC8;
	packetp[2] = dsn;

	packetp[3] = 0xFF;
	packetp[4] = 0xFF;

	packetp[5] = 0xFF;
	packetp[6] = 0xFF;

	packetp[7] = 0x34;
	packetp[8] = 0x43;

	packetp[9] = mSelfMac[0];
	packetp[10] = mSelfMac[1];
	packetp[11] = mSelfMac[2];
	packetp[12] = mSelfMac[3];
	packetp[13] = mSelfMac[4];
	packetp[14] = mSelfMac[5];
	packetp[15] = mSelfMac[6];
	packetp[16] = mSelfMac[7];
	packetp[17] = 0xC8;
	packetp[18] = 0xBC;
	packetp[19] = current_unix_time >> 24;
	packetp[20] = current_unix_time >> 16;
	packetp[21] = current_unix_time >> 8;
	packetp[22] = current_unix_time;
	packetp[23] = temperature;
	dsn++;
	commsTxUnencrypted(packetp, 24/*LEN*/);
	uint32_t start_time = timerGet();
	while (timerGet() - start_time < (TIMER_TICKS_PER_SECOND / 10))// 100ms timeout waiting for an answer from AP
	{
		int8_t ret = commsRxUnencrypted(mRxBuf);
		if (ret > 1)
		{
			pr("RX Len: %d Data:", ret);
			for (int8_t len = 0; len < ret; len++)
			{
				pr(" 0x%02X", mRxBuf[len]);
			}
			pr("\r\nradio off\r\n");
			radioRxEnable(false, true);
			if (mRxBuf[21] == 0xC9 && mRxBuf[22] == 0xCA)// random magic bytes
			{
				seconds_since_last_sync = current_unix_time - last_synced_time;
				last_synced_time = current_unix_time;
				current_unix_time = (uint32_t)mRxBuf[23] << 24 | (uint32_t)mRxBuf[24] << 16 | (uint32_t)mRxBuf[25] << 8 | (uint32_t)mRxBuf[26];
				pr("Setting the new time now: %lu\n", current_unix_time);
				if (was_never_synced >= 2)// only the second sync can be used to trim the time for now.
				{
					last_time_offset = current_unix_time - last_synced_time;
					pr("My offset was: %ld in %lu seconds\n", last_time_offset, seconds_since_last_sync);
					if (last_time_offset > 0 || last_time_offset < 0)// if the time was not correct try to align it
					{
						int32_t temp_off = last_time_offset * 1000;
						temp_off = temp_off / seconds_since_last_sync;
						pr("Wrong time: %ld\n", temp_off);
						one_second_trimmed = one_second_trimmed + (TIMER_TICKS_PER_SECOND / temp_off);
					}
				}
				was_never_synced++;
				if (was_never_synced >= 2)
					was_never_synced = 2;
			}
			return;
		}
	}
	pr("\r\nTIMEOUT radio off\r\n");
	radioRxEnable(false, true);
}

void handle_time()
{
	while (timerGet() - last_clock_increase > one_second_trimmed)
	{
		last_clock_increase += one_second_trimmed;
		current_unix_time++;
	}
}




void main() {
    setupPortsInitial();
    powerUp(INIT_BASE | INIT_UART);
    pr("BOOTED> %04X%s\n",fwVersion, fwVersionSuffix);

#ifdef DEBUGGUI
    displayLoop();  // remove me
#endif

    // Find the reason why we're booting; is this a WDT?
    wakeUpReason = getFirstWakeUpReason();

    //  get our own mac address. this is stored in Infopage at offset 0x10-onwards
    boardGetOwnMac(mSelfMac);

#ifdef DEBUGMAIN
    pr("MAC>%02X%02X", mSelfMac[0], mSelfMac[1]);
    pr("%02X%02X", mSelfMac[2], mSelfMac[3]);
    pr("%02X%02X", mSelfMac[4], mSelfMac[5]);
    pr("%02X%02X\n", mSelfMac[6], mSelfMac[7]);
#endif
    // do a little sleep, this prevents a partial boot during battery insertion
    doSleep(400UL);
    powerUp(INIT_EEPROM | INIT_UART);

    // load settings from infopage
    loadSettings();
    // invalidate the settings, and write them back in a later state
    invalidateSettingsEEPROM();

    // get the highest slot number, number of slots
    initializeProto();
    powerDown(INIT_EEPROM);

    // detect button or jig
    detectButtonOrJig();


    // Normal boot/startup
#ifdef DEBUGMAIN
    pr("Normal boot\n");
#endif
    // validate the mac address; this will display a warning on the screen if the mac address is invalid
    validateMacAddress();

    // Get a voltage reading on the tag, loading down the battery with the radio
    doVoltageReading();

    // show the splashscreen
    currentChannel = 11;
    showSplashScreen();
    currentChannel = 0;

// we've now displayed something on the screen; for the SSD1619, we are now aware of the lut-size
#ifdef EPD_SSD1619
    capabilities |= CAPABILITY_SUPPORTS_CUSTOM_LUTS;
    if (dispLutSize != 7) {
        capabilities |= CAPABILITY_ALT_LUT_SIZE;
    }
#endif
    tagSettings.fastBootCapabilities = capabilities;

    // now that we've collected all possible capabilities, save it to settings

    // scan for channels
    wdt30s();

    // end of the fastboot option split

    wdt10s();
#ifdef DEBUGMAIN
    pr("MAIN: No AP found...\n");
#endif
    // showAPFound();
    // showNoAP();
    // write the settings to the eeprom
    powerUp(INIT_EEPROM);
    writeSettings();
    powerDown(INIT_EEPROM);

    initPowerSaving(INTERVAL_AT_MAX_ATTEMPTS);
    doSleep(12000UL);


    // this is the loop we'll stay in forever, basically.
    while (1) {
        powerUp(INIT_UART);
        handle_time();
        wdt10s();

        uint16_t h = 0;
		uint8_t m = 0;
		uint8_t s = 0;
		uint32_t t = 0;
		__bit do_refresh = 0;

		t = current_unix_time;
		s = t % 60;
		t = (t - s) / 60;
		m = t % 60;
		t = (t - m) / 60;
		h = t % 24;

		pr("Sleeping now %lu   %02d:%02d:%02d  LastMinute: %d\n", current_unix_time, h, m, s, last_minute);
		if (last_hour != h || !was_never_synced)// Only do this every hour
		{
			request_time_via_zigbee();
			t = current_unix_time;
			s = t % 60;
			t = (t - s) / 60;
			m = t % 60;
			t = (t - m) / 60;
			h = t % 24;
			last_hour = h;
			last_minute = (m + 5) % 60;
			do_refresh = 1;
		}
		else if (last_minute == m)// refresh the time every 5 minute
		{
			last_minute = (m + 5) % 60;
			do_refresh = 1;
		}
		if (do_refresh)
		{
			do_refresh = 0;

			pr("temp: %d %dV Offset: %lu Trimmed: %lu\n", temperature, batteryVoltage, last_time_offset, one_second_trimmed);
			showCurrentTime(current_unix_time);
		}
		uint32_t seconds_till_next_minute = 61 - (current_unix_time % 60);// Try to leep until one minute has passt
		pr("Sleeping now: %lu\n", seconds_till_next_minute);
		if (!seconds_till_next_minute) // prevent endless sleeping this way
			seconds_till_next_minute = 60;
		doSleep(seconds_till_next_minute * 1000); // Attention always also increase the unix time!
		current_unix_time += seconds_till_next_minute;
    }
}
