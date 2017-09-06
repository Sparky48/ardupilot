/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <AP_HAL/AP_HAL.h>
#include "AP_RangeFinder_uLanding.h"
#include <AP_SerialManager/AP_SerialManager.h>
#include <ctype.h>

#define ULANDING_HDR 254   // Header Byte from uLanding (0xFE)
#define ULANDING_HDR_V0 72 // Header Byte for beta V0 of uLanding (0x48)

extern const AP_HAL::HAL& hal;

/*
   The constructor also initialises the rangefinder. Note that this
   constructor is not called until detect() returns true, so we
   already know that we should setup the rangefinder
*/
AP_RangeFinder_uLanding::AP_RangeFinder_uLanding(RangeFinder &_ranger, uint8_t instance,
                                                             RangeFinder::RangeFinder_State &_state,
                                                             AP_SerialManager &serial_manager) :
    AP_RangeFinder_Backend(_ranger, instance, _state, MAV_DISTANCE_SENSOR_RADAR)
{
    uart = serial_manager.find_serial(AP_SerialManager::SerialProtocol_Aerotenna_uLanding, 0);
    if (uart != nullptr) {
        uart->begin(serial_manager.find_baudrate(AP_SerialManager::SerialProtocol_Aerotenna_uLanding, 0));
    }
}

/*
   detect if a uLanding rangefinder is connected. We'll detect by
   trying to take a reading on Serial. If we get a result the sensor is
   there.
*/
bool AP_RangeFinder_uLanding::detect(RangeFinder &_ranger, uint8_t instance, AP_SerialManager &serial_manager)
{
    return serial_manager.find_serial(AP_SerialManager::SerialProtocol_Aerotenna_uLanding, 0) != nullptr;
}

/*
   detect uLanding Firmware Version
*/
bool AP_RangeFinder_uLanding::detect_version(void)
{
    if (_version_known) {
        // return true if we've already detected the uLanding version
        return true;
    } else if (uart == nullptr) {
        return false;
    }

    bool hdr_found = false;
    uint8_t byte1 = 0;
    uint8_t count = 0;

    // read any available data from uLanding
    int16_t nbytes = uart->available();

    while (nbytes-- > 0) {
        uint8_t c = uart->read();
        
        if (((c == ULANDING_HDR_V0) || (c == ULANDING_HDR)) && !hdr_found) {
            byte1 = c;
            hdr_found = true;
            count++;
        } else if (hdr_found) {
            if (byte1 == ULANDING_HDR_V0) {
                if (++count < 4) {
                    /* need to collect 4 bytes to check for recurring
                     * header byte in the old 3-byte data format
                     */
                    continue;
                } else {
                    if (c == byte1) {
                        // if header byte is recurring, set uLanding Version
                        _version = 0;
                        _header = ULANDING_HDR_V0;
                        _version_known = true;
                        return true;
                    } else {
                        /* if V0 header byte didn't occur again on 4th byte,
                         * start the search again for a header byte
                         */
                        count = 0;
                        byte1 = 0;
                        hdr_found = false;
                    }
                }
            } else {
                if ((c & 0x80) || (c == ULANDING_HDR_V0)) {
                    /* Though unlikely, it is possible we could find ULANDING_HDR
                     * in a data byte from the old 3-byte format. In this case,
                     * either the next byte is another data byte (which by default
                     * is of the form 0x1xxxxxxx), or the next byte is the old 
                     * header byte (ULANDING_HDR_V0). In this case, start the search
                     * again for a header byte.
                     */
                    count = 0;
                    byte1 = 0;
                    hdr_found = false;
                } else {
                    /* if this second byte passes the above if statement, this byte
                     * is the version number
                     */
                    _version = c;
                    _header = ULANDING_HDR;
                    _version_known = true;
                    return true;
                }
            }
        }
    }

    /* return false if we've gone through all available data
     * and haven't detected a uLanding firmware version
     */
    return false;
}


// read - return last value measured by sensor
bool AP_RangeFinder_uLanding::get_reading(uint16_t &reading_cm)
{
    if (uart == nullptr) {
        return false;
    }

    
    if (!detect_version()) {
        // return false if uLanding version check failed
        return false;
    }

    // read any available lines from the uLanding
    float sum = 0;
    uint16_t count = 0;
    bool hdr_found = false;

    int16_t nbytes = uart->available();

    while (nbytes-- > 0) {
        uint8_t c = uart->read();
        
        if ((c == _header) && !hdr_found) {
            // located header byte
            linebuf_len = 0;
            hdr_found   = true;
        }
        // decode index information
        if (hdr_found) {
            linebuf[linebuf_len++] = c;

            /* don't process linebuf until we've collected six bytes of data
             * (or 3 bytes for Version 0 firmware)
             */
            if ((linebuf_len < (sizeof(linebuf)/sizeof(linebuf[0]))) ||
                (_version == 0 && linebuf_len < 3)) {
                continue;
            } else {
                if (_version == 0) {
                    // parse data from Firmware Version #0
                    sum += (linebuf[2]&0x7F)*128 + (linebuf[1]&0x7F);
                    count++;
                } else {
                    // evaluate checksum
                    if (((linebuf[1] + linebuf[2] + linebuf[3] + linebuf[4]) & 0xFF) == linebuf[5]) {
                        // if checksum passed, parse data from Firmware Version #1
                        sum += linebuf[3]*256 + linebuf[2];
                        count++;
                    }
                }

                hdr_found = false;
                linebuf_len = 0;
            }
        }
    }

    if (count == 0) {
        return false;
    }

    reading_cm = sum / count;

    if (_version == 0) {
        reading_cm *= 2.5f;
    }

    return true;
}

/*
   update the state of the sensor
*/
void AP_RangeFinder_uLanding::update(void)
{
    if (get_reading(state.distance_cm)) {
        // update range_valid state based on distance measured
        last_reading_ms = AP_HAL::millis();
        update_status();
    } else if (AP_HAL::millis() - last_reading_ms > 200) {
        set_status(RangeFinder::RangeFinder_NoData);
    }
}
