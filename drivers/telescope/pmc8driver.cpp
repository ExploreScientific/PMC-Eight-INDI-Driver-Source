/*
    INDI Explore Scientific PMC8 driver

    Copyright (C) 2017 Michael Fulbright
    Additional contributors:
        Thomas Olson, Copyright (C) 2019
        Karl Rees, Copyright (C) 2019-2023
        Martin Ruiz, Copyright (C) 2023

    Based on IEQPro driver.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "pmc8driver.h"

#include "indicom.h"
#include "indilogger.h"
#include "inditelescope.h"

#include <libnova/julian_day.h>
#include <libnova/sidereal_time.h>

#include <algorithm>
#include <math.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>

// only used for test timing of pulse guiding
#include <sys/time.h>

#define PMC8_TIMEOUT 5 /* FD timeout in seconds */

#define PMC8_SIMUL_VERSION_RESP "ESGvES06B9T9"

// MOUNT_G11
#define PMC8_G11_AXIS0_SCALE 4608000.0
#define PMC8_G11_AXIS1_SCALE 4608000.0
// MOUNT_TITAN
#define PMC8_TITAN_AXIS0_SCALE 6048000.0
#define PMC8_TITAN_AXIS1_SCALE 6048000.0
// MOUNT_EXOS2
#define PMC8_EXOS2_AXIS0_SCALE 4147200.0
#define PMC8_EXOS2_AXIS1_SCALE 4147200.0
// MOUNT_iEXOS100
#define PMC8_iEXOS100_AXIS0_SCALE 4147200.0
#define PMC8_iEXOS100_AXIS1_SCALE 4147200.0
// MOUNT_iEXOS200
#define PMC8_iEXOS200_AXIS0_SCALE 5760000.0
#define PMC8_iEXOS200_AXIS1_SCALE 5760000.0
// MOUNT_iEXOS300 - ASCOM marks these as future TBD.
#define PMC8_iEXOS300_AXIS0_SCALE 4147200.0
#define PMC8_iEXOS300_AXIS1_SCALE 4147200.0
// MOUNT_MSROEQ
#define PMC8_MSROEQ_AXIS0_SCALE 5760000.0
#define PMC8_MSROEQ_AXIS1_SCALE 5760000.0
// MOUNT_ASKO
#define PMC8_ASKO_AXIS0_SCALE 9163636.0
#define PMC8_ASKO_AXIS1_SCALE 9600000.0
// Need to initialize to some value, or certain clients (e.g., KStars Lite) freak out
double PMC8_AXIS0_SCALE = PMC8_EXOS2_AXIS0_SCALE;
double PMC8_AXIS1_SCALE = PMC8_EXOS2_AXIS1_SCALE;

#define ARCSEC_IN_CIRCLE 1296000.0

// Reference says 2621.44 counts, which then needs to be multiplied by 25 (so actually 16^4-1)
// However, on Exos2 62500 (F424) is reported when slewing
#define PMC8_MAX_PRECISE_MOTOR_RATE 62500

// any guide pulses less than this are ignored as it will not result in any actual motor motion
#define PMC8_PULSE_GUIDE_MIN_MS 20

// guide pulses longer than this require using a timer
#define PMC8_PULSE_GUIDE_MAX_NOTIMER 250

#define PMC8_MAX_RETRIES 3 /*number of times to retry reading a response */
#define PMC8_RETRY_DELAY 30000 /* how long to wait before retrying i/o */
#define PMC8_MAX_IO_ERROR_THRESHOLD 2 /* how many consecutive read timeouts before trying to reset the connection */
#define PMC8_WIFI_REFRACTION_USEC 50000 /* ASCOM WiFi branch gives the ESP module a short breathing interval */
#define PMC8_PARK_POSITION_TOLERANCE_COUNTS 250 /* ASCOM park verification tolerance */

// Explore Scientific ASCOM parity values for RA target compensation.
// Keep these synchronized with the authoritative PMC-Eight ASCOM driver wifi-fix branch.
#define PMC8_ASCOM_DEFAULT_MAX_SLEW_RATE_COUNTS 40000.0
#define PMC8_ASCOM_SHORT_MOVE_BASE_COUNTS 5000.0
#define PMC8_ASCOM_DEFAULT_LONG_MOVE_OFFSET_EAST 8.0
#define PMC8_ASCOM_DEFAULT_LONG_MOVE_OFFSET_WEST -4.0
#define PMC8_ASCOM_DEFAULT_RAMP_ONLY_OFFSET_EAST 4.0
#define PMC8_ASCOM_DEFAULT_RAMP_ONLY_OFFSET_WEST 4.0
#define PMC8_ASCOM_FINISHING_MOVE_THRESHOLD_COUNTS 2.0

// Thanks to John Wells who contributed to this issue: https://github.com/indilib/indi/issues/2132
// and PMC8 documentation: https://02d3287.netsolhost.com/pmc-eight/PMC_Eight_ProgrammersReferenceManual_Release2_2019_February_01.pdf
// and confirmation: https://espmc-eight.groups.io/g/MAIN/topic/96696552?p=Created,,,20,2,0,0
// The rate is communicated as "arcsecs per sidereal second" NOT per second so it should be 15.000
// not 15.041067
// The INDI Driver uses arcsecs per SOLAR second, so it needs to be converted.
#define PMC8_RATE_SIDEREAL 15.000
#define PMC8_RATE_LUNAR 14.451
#define PMC8_RATE_SOLAR 14.959
#define PMC8_RATE_KING 14.996

PMC8_CONNECTION_TYPE pmc8_connection         = PMC8_SERIAL_AUTO;
bool pmc8_debug                 = false;
bool pmc8_simulation            = false;
bool pmc8_isRev2Compliant       = false;
bool pmc8_reconnect_flag        = false;
bool pmc8_goto_resume           = true;
bool pmc8_ascom_slew_compensation = true;
int pmc8_io_error_ctr           = 0;
char pmc8_device[MAXINDIDEVICE] = "PMC8";
double pmc8_latitude            = 0;  // must be kept updated by pmc8.cpp when it is changed!
double pmc8_longitude           = 0;  // must be kept updated by pmc8.cpp when it is changed!
double pmc8_sidereal_rate_fraction_ra = 0.4;
double pmc8_sidereal_rate_fraction_de = 0.4;
double pmc8_expected_dec_track_rate = 0.0;
int pmc8_east_dir               = 1; // 1 is for northern hemisphere, switch to 0 for southern
double pmc8_mount_max_slew_rate_counts = PMC8_ASCOM_DEFAULT_MAX_SLEW_RATE_COUNTS;
double pmc8_mount_long_move_offset_east = PMC8_ASCOM_DEFAULT_LONG_MOVE_OFFSET_EAST;
double pmc8_mount_long_move_offset_west = PMC8_ASCOM_DEFAULT_LONG_MOVE_OFFSET_WEST;
double pmc8_mount_ramp_only_offset_east = PMC8_ASCOM_DEFAULT_RAMP_ONLY_OFFSET_EAST;
double pmc8_mount_ramp_only_offset_west = PMC8_ASCOM_DEFAULT_RAMP_ONLY_OFFSET_WEST;
bool pmc8_mount_msro_geometry = false;
bool pmc8_mount_ra_preferred_dir = true;
int pmc8_last_axis_position[2] = {0, 0};
bool pmc8_last_axis_position_valid[2] = {false, false};

static INDI::Telescope::TelescopePierSide slewDestinationSideOfPier(double ra, double dec);
static void sanitize_pmc8_ethernet_response(char *buf, int *nbytes_read, const char *expected);
PMC8Info simPMC8Info;

// state variable for driver based pulse guiding
typedef struct PulseGuideState
{
    bool pulseguideactive = false;
    bool fakepulse = false;
    int ms;
    long long pulse_start_us;
    double cur_rate;
    int cur_dir;
    double new_rate;
    int new_dir;
    bool firmwaretimed = false;
} PulseGuideState;

// need one for NS and EW pulses which may be simultaneous
PulseGuideState NS_PulseGuideState, EW_PulseGuideState;

struct
{
    double ra;
    double dec;
    int raDirection;
    int decDirection;
    double trackRate;
    double moveRate;
    double guide_rate;
} simPMC8Data;

// convert mount count to 6 character two complement hex string
void convert_motor_counts_to_hex(int val, char *hex)
{
    unsigned tmp;
    char h[16];

    if (val < 0)
    {
        tmp = abs(val);
        tmp = ~tmp;
        tmp++;
    }
    else
    {
        tmp = val;
    }

    sprintf(h, "%08X", tmp);

    strcpy(hex, h + 2);

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "convert_motor_counts_to_hex val=%d, h=%s, hex=%s", val, h, hex);
}

static bool convert_precise_rate_to_motor_scale(double rate, double axisScale, int *mrate)
{
    *mrate = round(25 * rate * (axisScale / ARCSEC_IN_CIRCLE));

    if (*mrate > PMC8_MAX_PRECISE_MOTOR_RATE)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_WARNING, "requested tracking motor rate %d exceeds maximum, using %d", *mrate,
                     PMC8_MAX_PRECISE_MOTOR_RATE);
        *mrate = PMC8_MAX_PRECISE_MOTOR_RATE;
    }
    else if (*mrate < -PMC8_MAX_PRECISE_MOTOR_RATE)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_WARNING, "requested tracking motor rate %d exceeds maximum, using %d", *mrate,
                     -PMC8_MAX_PRECISE_MOTOR_RATE);
        *mrate = -PMC8_MAX_PRECISE_MOTOR_RATE;
    }

    return true;
}

// convert rate in arcsec/sidereal_second to internal PMC8 precise motor rate for RA axis tracking ONLY
bool convert_precise_rate_to_motor(double rate, int *mrate)
{
    return convert_precise_rate_to_motor_scale(rate, PMC8_AXIS0_SCALE, mrate);
}

// convert rate in arcsec/sidereal_second to internal PMC8 precise motor rate for RA axis tracking ONLY
bool convert_precise_motor_to_rate(int mrate, double *rate)
{
    *rate = ((double)mrate) * (ARCSEC_IN_CIRCLE / PMC8_AXIS0_SCALE) / 25;

    return true;
}

// convert rate in arcsec/sidereal_second to internal PMC8 motor rate for move action (not slewing)
double get_pmc8_axis_max_move_rate(PMC8_AXIS axis)
{
    double axisScale = (axis == PMC8_AXIS_DEC) ? PMC8_AXIS1_SCALE : PMC8_AXIS0_SCALE;

    if (axisScale <= 0)
        return PMC8_MAX_MOVE_RATE;

    // ASCOM AxisRates reports max as MountMaxSpeed * 360 / MountCounts - 0.1
    // degrees/sec. INDI manual motion uses arcsec/sec, so convert here.
    double maxDegreesPerSecond = (pmc8_mount_max_slew_rate_counts * 360.0 / axisScale) - 0.1;

    return std::max(0.0, maxDegreesPerSecond * 3600.0);
}

static bool convert_move_rate_to_motor_axis(PMC8_AXIS axis, float rate, int *mrate)
{
    double maxMoveRate = get_pmc8_axis_max_move_rate(axis);
    double axisScale = (axis == PMC8_AXIS_DEC) ? PMC8_AXIS1_SCALE : PMC8_AXIS0_SCALE;
    float capped_move_rate = rate;

    if (maxMoveRate > 0)
    {
        if (rate > maxMoveRate)
            capped_move_rate = maxMoveRate;
        else if (rate < -maxMoveRate)
            capped_move_rate = -maxMoveRate;
    }

    *mrate = (int)(capped_move_rate * (axisScale / ARCSEC_IN_CIRCLE));

    return true;
}

// convert rate in arcsec/sidereal_second to internal PMC8 motor rate for RA move action
bool convert_move_rate_to_motor(float rate, int *mrate)
{
    return convert_move_rate_to_motor_axis(PMC8_AXIS_RA, rate, mrate);
}

// convert rate internal PMC8 motor rate to arcsec/sec for move action (not slewing)
bool convert_motor_rate_to_move_rate(int mrate, double *rate)
{
    *rate = ((double)mrate) * ARCSEC_IN_CIRCLE / PMC8_AXIS0_SCALE;

    return true;
}

void set_pmc8_mountParameters(int index)
{
    pmc8_mount_max_slew_rate_counts = PMC8_ASCOM_DEFAULT_MAX_SLEW_RATE_COUNTS;
    pmc8_mount_long_move_offset_east = PMC8_ASCOM_DEFAULT_LONG_MOVE_OFFSET_EAST;
    pmc8_mount_long_move_offset_west = PMC8_ASCOM_DEFAULT_LONG_MOVE_OFFSET_WEST;
    pmc8_mount_ramp_only_offset_east = PMC8_ASCOM_DEFAULT_RAMP_ONLY_OFFSET_EAST;
    pmc8_mount_ramp_only_offset_west = PMC8_ASCOM_DEFAULT_RAMP_ONLY_OFFSET_WEST;
    pmc8_mount_msro_geometry = false;
    pmc8_mount_ra_preferred_dir = true;

    switch(index)
    {
        case MOUNT_G11:
            PMC8_AXIS0_SCALE = PMC8_G11_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_G11_AXIS1_SCALE;
            break;
        case MOUNT_TITAN:
            PMC8_AXIS0_SCALE = PMC8_TITAN_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_TITAN_AXIS1_SCALE;
            pmc8_mount_ra_preferred_dir = false;
            break;
        case MOUNT_EXOS2:
            PMC8_AXIS0_SCALE = PMC8_EXOS2_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_EXOS2_AXIS1_SCALE;
            break;
        case MOUNT_iEXOS100:
            PMC8_AXIS0_SCALE = PMC8_iEXOS100_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_iEXOS100_AXIS1_SCALE;
            break;
        case MOUNT_iEXOS200:
            PMC8_AXIS0_SCALE = PMC8_iEXOS200_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_iEXOS200_AXIS1_SCALE;
            break;
        case MOUNT_iEXOS300:
            PMC8_AXIS0_SCALE = PMC8_iEXOS300_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_iEXOS300_AXIS1_SCALE;
            break;
        case MOUNT_MSROEQ:
            PMC8_AXIS0_SCALE = PMC8_MSROEQ_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_MSROEQ_AXIS1_SCALE;
            pmc8_mount_msro_geometry = true;
            break;
        case MOUNT_ASKO:
            PMC8_AXIS0_SCALE = PMC8_ASKO_AXIS0_SCALE;
            PMC8_AXIS1_SCALE = PMC8_ASKO_AXIS1_SCALE;
            pmc8_mount_max_slew_rate_counts = 16000.0;
            pmc8_mount_long_move_offset_east = 2.0;
            pmc8_mount_long_move_offset_west = 2.0;
            break;
        default:
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Need To Select a  Mount");
            break;
    }
}

void set_pmc8_ascom_slew_compensation(bool enable)
{
    pmc8_ascom_slew_compensation = enable;
}

void set_pmc8_debug(bool enable)
{
    pmc8_debug = enable;
}

void set_pmc8_simulation(bool enable)
{
    pmc8_simulation = enable;
    if (enable)
        simPMC8Data.guide_rate = 0.5;
}

void set_pmc8_device(const char *name)
{
    strncpy(pmc8_device, name, MAXINDIDEVICE);
}

void set_pmc8_location(double latitude, double longitude)
{
    pmc8_latitude = latitude;
    pmc8_longitude = longitude;

    pmc8_east_dir = (latitude < 0) ? 0 : 1;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "Set PMC8 'lowlevel' lat:%f long:%f", pmc8_latitude, pmc8_longitude);
}

void set_pmc8_sim_system_status(PMC8_SYSTEM_STATUS value)
{
    simPMC8Info.systemStatus = value;

    if (value == ST_PARKED)
    {
        double lst;
        double ra;

        lst = get_local_sidereal_time(pmc8_longitude);

        ra = lst + 6;
        if (ra > 24)
            ra -= 24;

        set_pmc8_sim_ra(ra);
        if (pmc8_latitude < 0)
            set_pmc8_sim_dec(-90.0);
        else
            set_pmc8_sim_dec(90.0);

    }
}

void set_pmc8_sim_track_rate(PMC8_TRACK_RATE value)
{
    simPMC8Data.trackRate = value;
}

void set_pmc8_sim_move_rate(int value)
{
    simPMC8Data.moveRate = value;
}

void set_pmc8_sim_ra(double ra)
{
    simPMC8Data.ra = ra;
}

void set_pmc8_sim_dec(double dec)
{
    simPMC8Data.dec = dec;
}

bool check_pmc8_connection(int fd, PMC8_CONNECTION_TYPE connection)
{
    pmc8_connection = connection;

    if (connection == PMC8_ETHERNET)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_SESSION, "Connecting to PMC8 via Ethernet.");
    }
    else
    {
        if (connection == PMC8_SERIAL_STANDARD) DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_SESSION,
                    "Connecting to PMC8 via standard Serial cable.  Please wait 15 seconds for mount to reset.");
        else if (connection == PMC8_SERIAL_AUTO) DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_SESSION,
                    "Connecting to PMC8 via Serial.  Autodecting cable type.  This could take up to 30 seconds.");
        else DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_SESSION, "Connecting to PMC8 via inverted Serial.");
    }

    if (connection != PMC8_SERIAL_STANDARD)
    {
        for (int i = 0; i < 2; i++)
        {
            if (i) DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_SESSION, "Retrying...");

            if (detect_pmc8(fd)) return true;
            usleep(PMC8_RETRY_DELAY);
        }
    }

    if ((connection == PMC8_SERIAL_STANDARD) || (connection == PMC8_SERIAL_AUTO))
    {
        // If they're not using a custom-configured cable, we need to clear DTR for serial to start working
        // But this resets the PMC8, so only do it after we've already checked for connection
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "Attempting to clear DTR for standard cable.");
        int serial = TIOCM_DTR;
        ioctl(fd, TIOCMBIC, &serial);

        // when we clear DTR, the PMC8 will respond with initialization screen, so may need read several times
        for (int i = 0; i < 2; i++)
        {
            if (i) DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_SESSION, "Retrying...");

            if (detect_pmc8(fd))
            {
                DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_WARNING, "Connected to PMC8 using a standard-configured FTDI cable."
                            "Your mount will reset and lose its position anytime you disconnect and reconnect."
                            "See http://indilib.org/devices/telescopes/explore-scientific-g11-pmc-eight/ ");
                return true;
            }
            usleep(PMC8_RETRY_DELAY);
        }
    }

    DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR,
                "check_pmc8_connection(): Error connecting. Check power and connection settings.");

    return false;
}

bool detect_pmc8(int fd)
{
    char initCMD[] = "ESGv!";
    int errcode    = 0;
    char errmsg[MAXRBUF];
    char response[64];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    if (pmc8_simulation)
    {
        strcpy(response, PMC8_SIMUL_VERSION_RESP);
        nbytes_read = strlen(response);
    }
    else
    {
        if ((errcode = send_pmc8_command(fd, initCMD, strlen(initCMD), &nbytes_written)) != TTY_OK)
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error connecting on write: %s", errmsg);
            return false;
        }

        if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGv")))
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "Error connecting on read: %s", errmsg);
            return false;
        }
    }

    // return true if valid firmware response
    return (!strncmp(response, "ESGvES", 6));
}

bool get_pmc8_model(int fd, FirmwareInfo *info)
{
    // Only one model for now
    info->Model.assign("PMC-Eight");

    // Set the mount type from firmware if we can (instead of relying on interface)
    // older firmware has type in firmware string
    if (!pmc8_isRev2Compliant)
    {
        INDI_UNUSED(fd);

        if (strstr(info->MainBoardFirmware.c_str(), "G11"))
        {
            info->MountType = MOUNT_G11;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "Titan"))
        {
            info->MountType = MOUNT_TITAN;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "EXOS2"))
        {
            info->MountType = MOUNT_EXOS2;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "iEXOS200"))
        {
            info->MountType = MOUNT_iEXOS200;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "iEXOS300"))
        {
            info->MountType = MOUNT_iEXOS300;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "ES1A"))
        {
            info->MountType = MOUNT_iEXOS100;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "MSRO"))
        {
            info->MountType = MOUNT_MSROEQ;
        }
        else if (strstr(info->MainBoardFirmware.c_str(), "ASKO"))
        {
            info->MountType = MOUNT_ASKO;
        }
    }
    else
    {
        //for newer firmware, need to use ESGi to get mount type
        char cmd[]  = "ESGi!";
        int errcode = 0;
        char errmsg[MAXRBUF];
        char response[64];
        int nbytes_read    = 0;
        int nbytes_written = 0;

        if (pmc8_simulation)
        {
            strcpy(response, PMC8_SIMUL_VERSION_RESP);
            nbytes_read = strlen(response);
        }
        else
        {
            if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
            {
                tty_error_msg(errcode, errmsg, MAXRBUF);
                DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "3 %s", errmsg);
                return false;
            }

            if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGi")))
            {
                DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_main_firmware(): Error reading response.");
                return false;
            }

            //ESGi response should be 31 characters
            if (nbytes_read >= 31)
            {
                //locate P9 code in response
                char num_str[3] = {0};
                strncat(num_str, response + 20, 2);
                int p9 = (int)strtol(num_str, nullptr, 16);

                // Set mount type based on the firmware P9 table. P9 is hexadecimal; codes 0xA-0xF
                // cover newer EXOS2/Titan/MSROEQ/ASKO configurations.
                if (p9 == 0) info->MountType = MOUNT_G11;
                else if (p9 == 1) info->MountType = MOUNT_iEXOS100;
                else if (p9 == 2) info->MountType = MOUNT_iEXOS200;
                else if (p9 == 3) info->MountType = MOUNT_iEXOS300;
                else if (p9 <= 7) info->MountType = MOUNT_G11;
                else if (p9 <= 12) info->MountType = MOUNT_EXOS2;
                else if (p9 == 13) info->MountType = MOUNT_TITAN;
                else if (p9 == 14) info->MountType = MOUNT_MSROEQ;
                else if (p9 == 15) info->MountType = MOUNT_ASKO;
                // unrecognized code.  Just going to guess and treat as iExos100.
                else
                {
                    info->MountType = MOUNT_iEXOS100;
                    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Unrecognized device code #%d. Treating as iEXOS100.", p9);
                }

            }
            else
            {
                DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR,
                             "Could not detect device type. Only received #%d bytes, expected at least 31.", nbytes_read);
                return false;
            }

            tcflush(fd, TCIFLUSH);
        }
    }
    // update mount parameters
    set_pmc8_mountParameters(info->MountType);
    return true;
}

bool get_pmc8_main_firmware(int fd, FirmwareInfo *info)
{
    char cmd[]  = "ESGv!";
    char board[64];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[64];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    if (pmc8_simulation)
    {
        strcpy(response, PMC8_SIMUL_VERSION_RESP);
        nbytes_read = strlen(response);
    }
    else
    {
        if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "3 %s", errmsg);
            return false;
        }

        if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGv")))
        {
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_main_firmware(): Error reading response.");
            return false;
        }
    }

    // prior to v2, minimum size firmware string is 12 (for iExos100), 14 for others, but can be up to 20
    // post v2, can be 50+
    if (nbytes_read >= 12)
    {

        // strip ESGvES from string when getting firmware version
        strncpy(board, response + 6, nbytes_read - 7);
        info->MainBoardFirmware.assign(board, nbytes_read - 7);

        // Assuming version strings longer than 24 must be version 2.0 and up
        if (nbytes_read > 24) info->IsRev2Compliant = pmc8_isRev2Compliant = true;

        tcflush(fd, TCIFLUSH);

        return true;
    }

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR,
                 "Could not read firmware. Only received #%d bytes, expected at least 12.", nbytes_read);
    return false;
}

bool get_pmc8_firmware(int fd, FirmwareInfo *info)
{
    bool rc = false;

    rc = get_pmc8_main_firmware(fd, info);

    if (rc == false)
        return rc;

    rc = get_pmc8_model(fd, info);

    return rc;
}

// return move rate in arcsec / sec
bool get_pmc8_move_rate_axis(int fd, PMC8_AXIS axis, double &rate)
{
    char cmd[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESGr%d!", axis);

    if (pmc8_simulation)
    {
        if (axis == PMC8_AXIS_RA)
            rate = simPMC8Data.trackRate;
        else if (axis == PMC8_AXIS_DEC)
            rate = 0; // DEC tracking not supported yet
        else
            return false;

        return true;
    }

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    cmd[5] = '\0';

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, cmd)))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting Move Rate");
        return false;
    }

    if (nbytes_read != 10)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Axis get move rate cmd response incorrect");
        return false;
    }

    char num_str[16] = {0};

    strcpy(num_str, "0X");
    strncat(num_str, response + 5, 6);

    int mrate = (int)strtol(num_str, nullptr, 0);

    convert_motor_rate_to_move_rate(mrate, &rate);

    return true;
}

bool get_pmc8_direction_axis(int fd, PMC8_AXIS axis, int &dir)
{
    char cmd[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESGd%d!", axis);

    if (pmc8_simulation)
    {
        if (axis == PMC8_AXIS_RA)
            dir = simPMC8Data.raDirection;
        else if (axis == PMC8_AXIS_DEC)
            dir = simPMC8Data.decDirection;
        else
            return false;

        return true;
    }

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    cmd[5] = '\0';

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, cmd)))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting direction axis");
        return false;
    }

    if (nbytes_read != 7)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Axis get dir cmd response incorrect");
        return false;
    }

    char num_str[16] = {0};

    strncat(num_str, response + 5, 2);

    dir = (int)strtol(num_str, nullptr, 0);

    return true;
}

// if fast is true dont wait on response!  Used for psuedo-pulse guide
// NOTE that this will possibly mean the response will be read by a following command if it is called before
//      response comes from controller, since next command will flush before data is in buffer!
bool set_pmc8_direction_axis(int fd, PMC8_AXIS axis, int dir, bool fast)
{

    char cmd[32], expresp[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESSd%d%d!", axis, dir);

    if (pmc8_simulation)
    {
        if (axis == PMC8_AXIS_RA)
            simPMC8Data.raDirection = (PMC8_DIRECTION) dir;
        else if (axis == PMC8_AXIS_DEC)
            simPMC8Data.decDirection = (PMC8_DIRECTION) dir;
        else
            return false;

        return true;
    }

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if (fast)
    {
        return true;
    }

    snprintf(expresp, sizeof(expresp), "ESGd%d%d!", axis, dir);

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, expresp)))
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Axis get dir cmd response incorrect: expected=%s", expresp);
        return false;
    }

    return true;
}

static bool get_pmc8_legacy_is_scope_slewing(int fd, bool &isslew)
{
    double rarate;
    double decrate;
    bool rc;

    rc = get_pmc8_move_rate_axis(fd, PMC8_AXIS_RA, rarate);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_is_scope_slewing(): Error reading RA move rate");
        return false;
    }

    rc = get_pmc8_move_rate_axis(fd, PMC8_AXIS_DEC, decrate);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_is_scope_slewing(): Error reading DEC move rate");
        return false;
    }

    isslew = ((rarate > PMC8_MAX_TRACK_RATE) || (decrate >= PMC8_MAX_TRACK_RATE));
    if (!isslew && pmc8_ascom_slew_compensation)
    {
        int curdir = 0;
        rc = get_pmc8_direction_axis(fd, PMC8_AXIS_RA, curdir);
        if (!rc)
        {
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_is_scope_slewing(): Error reading RA direction");
            return false;
        }

        const int normalTrackingDir = pmc8_east_dir ? 1 : 0;
        const double finishingMoveThreshold = PMC8_ASCOM_FINISHING_MOVE_THRESHOLD_COUNTS * ARCSEC_IN_CIRCLE / PMC8_AXIS0_SCALE;
        if ((curdir != normalTrackingDir) && (rarate > finishingMoveThreshold))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG,
                         "ASCOM finishing-move detection active: rarate=%f arcsec/sec, curdir=%d, normaldir=%d",
                         rarate, curdir, normalTrackingDir);
            isslew = true;
        }
    }

    return true;
}

static bool parse_pmc8_state_vector_rate(const char *response, int offset, double &rate)
{
    char hexRate[8] = {0};
    strncpy(hexRate, response + offset, 5);
    rate = strtol(hexRate, nullptr, 16) / 25.0;
    return true;
}

static bool get_pmc8_state_vector_is_scope_slewing(int fd, bool &isslew)
{
    char cmd[8] = "ESV!";
    char response[64] = {0};
    int errcode = 0;
    char errmsg[MAXRBUF];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESV")))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting PMC8 state vector");
        return false;
    }

    if (nbytes_read < 30)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Invalid ESV! response length=%d response=%s", nbytes_read, response);
        return false;
    }

    // ESV! reports high-resolution motor rates and pulse-guide flags in one atomic read.
    // Character offsets match the authoritative ASCOM driver: RA rate at 11-15,
    // RA pulse flag at 16, DEC rate at 24-28, DEC pulse flag at 29 (1-based).
    double raRateCounts = 0;
    double decRateCounts = 0;
    parse_pmc8_state_vector_rate(response, 10, raRateCounts);
    parse_pmc8_state_vector_rate(response, 23, decRateCounts);

    const bool pulseGuideActive = (response[15] != '0') || (response[28] != '0');
    if (pulseGuideActive)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ESV! reports pulse guiding active; not treating this as goto slewing");
        isslew = false;
        return true;
    }

    double expectedRARate = 0;
    if (!get_pmc8_track_rate(fd, expectedRARate))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error reading expected RA tracking rate for ESV! slew check");
        return false;
    }

    const double expectedRACounts = fabs(expectedRARate) * PMC8_AXIS0_SCALE / ARCSEC_IN_CIRCLE;
    const double expectedDECCounts = fabs(pmc8_expected_dec_track_rate) * PMC8_AXIS1_SCALE / ARCSEC_IN_CIRCLE;

    const bool raSlewing = (fabs(raRateCounts - expectedRACounts) > 1.0) && (raRateCounts != 0.0);
    const bool decSlewing = (fabs(decRateCounts - expectedDECCounts) > 1.0) && (decRateCounts != 0.0);

    isslew = raSlewing || decSlewing;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG,
                 "ESV! slew check: raRate=%f expectedRA=%f decRate=%f expectedDEC=%f raSlewing=%d decSlewing=%d isslew=%d",
                 raRateCounts, expectedRACounts, decRateCounts, expectedDECCounts, raSlewing, decSlewing, isslew);

    return true;
}

bool get_pmc8_is_scope_slewing(int fd, bool &isslew)
{
    if (pmc8_simulation)
    {
        isslew = (simPMC8Info.systemStatus == ST_SLEWING);
        return true;
    }

    if (pmc8_isRev2Compliant && pmc8_ascom_slew_compensation)
    {
        if (get_pmc8_state_vector_is_scope_slewing(fd, isslew))
            return true;

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ESV! slew check failed; falling back to legacy rate check");
    }

    return get_pmc8_legacy_is_scope_slewing(fd, isslew);
}

// set move speed in terms of how many times sidereal
// Southern Hemisphere support: In the southern hemisphere, the mount is oriented
// differently relative to the celestial pole, so we need to invert the motor
// directions for N/S movements. The pmc8_east_dir variable is 1 for northern
// hemisphere and 0 for southern hemisphere.
bool set_pmc8_move_rate_axis(int fd, PMC8_DIRECTION dir, int reqrate)
{
    int rate = reqrate;
    PMC8_AXIS axis = ((dir == PMC8_N) || (dir == PMC8_S)) ? PMC8_AXIS_DEC : PMC8_AXIS_RA;
    int maxAxisRate = (int)round(get_pmc8_axis_max_move_rate(axis));

    if (maxAxisRate > 0)
    {
        if (rate > maxAxisRate)
            rate = maxAxisRate;
        else if (rate < -maxAxisRate)
            rate = -maxAxisRate;
    }

    switch (dir)
    {
        case PMC8_S:
            // Normal GEM: south is negative in the north, positive in the south.
            // MSRO EQ geometry follows the ASCOM preferred-direction rules.
            rate = pmc8_mount_msro_geometry ? (pmc8_east_dir ? rate : -rate) : (pmc8_east_dir ? -rate : rate);
            return set_pmc8_custom_dec_move_rate(fd, rate);
        case PMC8_N:
            // Normal GEM: north is positive in the north, negative in the south.
            // MSRO EQ geometry follows the ASCOM preferred-direction rules.
            rate = pmc8_mount_msro_geometry ? (pmc8_east_dir ? -rate : rate) : (pmc8_east_dir ? rate : -rate);
            return set_pmc8_custom_dec_move_rate(fd, rate);
        case PMC8_E:
            if (pmc8_mount_ra_preferred_dir)
                rate = -rate;
            [[fallthrough]];
        case PMC8_W:
            if ((dir == PMC8_W) && !pmc8_mount_ra_preferred_dir)
                rate = -rate;
            return set_pmc8_custom_ra_move_rate(fd, rate);
    }

    return false;
}

bool stop_pmc8_tracking_motion(int fd)
{
    bool rc;

    // stop tracking
    rc = set_pmc8_custom_ra_track_rate(fd, 0);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error stopping RA axis!");
        return false;
    }

    return true;
}

// get current (precise) tracking rate in arcsec/sec
bool get_pmc8_track_rate(int fd, double &rate)
{
    char cmd[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESGx!");

    if (pmc8_simulation)
    {
        rate = simPMC8Data.trackRate;
        return true;
    }

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGx")))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting Tracking Rate");
        return false;
    }

    if (nbytes_read != 9)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Get track rate cmd response incorrect");
        return false;
    }

    char num_str[16] = {0};

    strcpy(num_str, "0X");
    strncat(num_str, response + 4, 4);

    int mrate = (int)strtol(num_str, nullptr, 0);
    convert_precise_motor_to_rate(mrate, &rate);

    return true;
}

bool get_pmc8_tracking_data(int fd, double &rate, uint8_t &mode)
{
    if (!get_pmc8_track_rate(fd, rate)) return false;
    mode = get_pmc8_tracking_mode_from_rate(rate);
    return true;
}


uint8_t get_pmc8_tracking_mode_from_rate(double rate)
{
    int tmotor, refmotor;
    uint8_t mode;

    //get precise motor rate
    convert_precise_rate_to_motor(rate, &tmotor);

    //now check what sidereal would be
    convert_precise_rate_to_motor(PMC8_RATE_SIDEREAL, &refmotor);
    if (tmotor == refmotor) mode = PMC8_TRACK_SIDEREAL;
    else
    {

        //now check lunar
        convert_precise_rate_to_motor(PMC8_RATE_LUNAR, &refmotor);
        if (tmotor == refmotor) mode = PMC8_TRACK_LUNAR;
        else
        {

            //now check solar
            convert_precise_rate_to_motor(PMC8_RATE_SOLAR, &refmotor);
            if (tmotor == refmotor) mode = PMC8_TRACK_SOLAR;
            else
            {

                //now check king
                convert_precise_rate_to_motor(PMC8_RATE_KING, &refmotor);
                if (tmotor == refmotor) mode = PMC8_TRACK_KING;
                // must be custom
                else mode = PMC8_TRACK_CUSTOM;
            }
        }
    }
    return mode;
}


// set speed for move action (MoveNS/MoveWE) NOT slews!  This version DOESNT handle direction and expects a motor rate!
// if fast is true dont wait on response!  Used for psuedo-pulse guide
// NOTE that this will possibly mean the response will be read by a following command if it is called before
//      response comes from controller, since next command will flush before data is in buffer!
bool set_pmc8_axis_motor_rate(int fd, PMC8_AXIS axis, int mrate, bool fast)
{
    char cmd[24];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[24];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESSr%d%04X!", axis, mrate);

    if (pmc8_simulation)
    {
        return true;
    }

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if (fast)
    {
        return true;
    }

    snprintf(cmd, sizeof(cmd), "ESGr%d", axis);

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, cmd)))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error setting axis motor rate");
        return false;
    }

    if (nbytes_read == 10)
    {
        tcflush(fd, TCIFLUSH);
        return true;
    }

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Only received #%d bytes, expected 10.", nbytes_read);
    return false;
}

// set speed for move action (MoveNS/MoveWE) NOT slews! This version accepts arcsec/sec as rate.
// also handles direction
bool set_pmc8_axis_move_rate(int fd, PMC8_AXIS axis, float rate)
{
    bool rc;
    int motor_rate;

    // set direction
    if (rate < 0)
        rc = set_pmc8_direction_axis(fd, axis, 0, false);
    else
        rc = set_pmc8_direction_axis(fd, axis, 1, false);

    if (!rc)
        return rc;

    if (!convert_move_rate_to_motor_axis(axis, fabs(rate), &motor_rate))
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error converting rate %f", rate);
        return false;
    }

    rc = set_pmc8_axis_motor_rate(fd, axis, motor_rate, false);

    if (pmc8_simulation)
    {
        simPMC8Data.moveRate = rate;
        return true;
    }

    return rc;
}

#if 0
bool set_pmc8_track_enabled(int fd, bool enabled)
{
    char cmd[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, 32, ":ST%d#", enabled ? 1 : 0);

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "CMD (%s)", cmd);

    if (pmc8_simulation)
    {
        // FIXME - need to implement pmc8 track enabled sim
        //        simPMC8Info.systemStatus = enabled ? ST_TRACKING_PEC_ON : ST_STOPPED;
        //        strcpy(response, "1");
        //        nbytes_read = strlen(response);

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Need to implement pmc8 track enabled sim");
        return false;
    }
    else
    {
        // determine current tracking mode

        //        return SetTrackMode(enabled ? IUFindOnSwitchIndex(&TrackModeSP) : AP_TRACKING_OFF);
    }
}
#endif

bool set_pmc8_track_mode(int fd, uint8_t mode)
{
    float ratereal = 0;

    switch (mode)
    {
        case PMC8_TRACK_SIDEREAL:
            ratereal = PMC8_RATE_SIDEREAL;
            break;
        case PMC8_TRACK_LUNAR:
            ratereal = PMC8_RATE_LUNAR;
            break;
        case PMC8_TRACK_SOLAR:
            ratereal = PMC8_RATE_SOLAR;
            break;
        case PMC8_TRACK_KING:
            ratereal = PMC8_RATE_KING;
            break;
        default:
            return false;
            break;
    }

    if (!set_pmc8_direction_axis(fd, PMC8_AXIS_RA, pmc8_east_dir, false)) return false;
    return set_pmc8_custom_ra_track_rate(fd, ratereal);
}

// start tracking at a precision track rate
bool set_pmc8_ra_tracking(int fd, double rate)
{
    //set right direction
    int direction = pmc8_east_dir;
    if (rate < 0) direction = !direction;
    if (!set_pmc8_direction_axis(fd, PMC8_AXIS_RA, direction, false)) return false;

    //then set rate
    return (set_pmc8_custom_ra_track_rate(fd, fabs(rate)));
}

// just set the precision track rate - for when we've already set tracking direction
bool set_pmc8_custom_ra_track_rate(int fd, double rate)
{
    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "set_pmc8_custom_ra_track_rate() called rate=%f ", rate);

    char cmd[24];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[24];
    int nbytes_read    = 0;
    int nbytes_written = 0;
    int rateval;

    if (!convert_precise_rate_to_motor(rate, &rateval))
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error converting rate %f", rate);
        return false;
    }

    snprintf(cmd, sizeof(cmd), "ESTr%04X!", rateval);

    if (pmc8_simulation)
    {
        simPMC8Data.trackRate = rate;
        return true;
    }
    else
    {
        if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
            return false;
        }

        if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGx")))
        {
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error setting custom RA track rate");
            return false;
        }
    }

    if (nbytes_read != 9)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Only received #%d bytes, expected 9.", nbytes_read);
        return false;
    }

    tcflush(fd, TCIFLUSH);

    // set direction to 1
    // return set_pmc8_direction_axis(fd, PMC8_AXIS_RA, 1, false);
    return true;
}

bool set_pmc8_custom_dec_track_rate(int fd, double rate, INDI::Telescope::TelescopePierSide pierSide)
{
    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "set_pmc8_custom_dec_track_rate() called rate=%f pierSide=%d", rate,
                 pierSide);

    if (fabs(rate) < 0.1)
        rate = 0.0;

    pmc8_expected_dec_track_rate = fabs(rate);

    if (pmc8_simulation)
        return true;

    if (!pmc8_isRev2Compliant)
    {
        if (rate == 0.0)
            return true;

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR,
                    "set_pmc8_custom_dec_track_rate(): DEC tracking requires Rev2-compliant firmware");
        return false;
    }

    int direction = 0;
    if (pmc8_mount_msro_geometry)
    {
        // ASCOM parity for fork/EQ geometry: pier side is not meaningful for DEC
        // rate direction. Positive rate moves toward the north pole, reversed in
        // the southern hemisphere.
        direction = (rate > 0.0) ? (pmc8_east_dir ? 0 : 1) : (pmc8_east_dir ? 1 : 0);
    }
    else if (pierSide == INDI::Telescope::PIER_EAST)
        direction = (rate > 0.0) ? 1 : 0;
    else if (pierSide == INDI::Telescope::PIER_WEST)
        direction = (rate > 0.0) ? 0 : 1;
    else if (rate != 0.0)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "set_pmc8_custom_dec_track_rate(): pier side unknown");
        return false;
    }

    if (rate != 0.0 && !set_pmc8_direction_axis(fd, PMC8_AXIS_DEC, direction, false))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "set_pmc8_custom_dec_track_rate(): error setting DEC direction");
        return false;
    }

    int rateval = 0;
    if (!convert_precise_rate_to_motor_scale(fabs(rate), PMC8_AXIS1_SCALE, &rateval))
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error converting DEC tracking rate %f", rate);
        return false;
    }

    char cmd[24];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[24];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESTe1%04X!", rateval);

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGx")))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error setting custom DEC track rate");
        return false;
    }

    if (nbytes_read != 9)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Only received #%d bytes, expected 9.", nbytes_read);
        return false;
    }

    tcflush(fd, TCIFLUSH);

    return true;
}

bool set_pmc8_custom_ra_move_rate(int fd, double rate)
{
    bool rc;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "set_pmc8_custom_ra move_rate() called rate=%f ", rate);

    // safe guard for now - only all use to STOP slewing or MOVE commands with this
    if (fabs(rate) > get_pmc8_axis_max_move_rate(PMC8_AXIS_RA))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "set_pmc8_custom_ra_move rate only supports low rates currently");

        return false;
    }

    rc = set_pmc8_axis_move_rate(fd, PMC8_AXIS_RA, rate);

    return rc;
}

bool set_pmc8_custom_dec_move_rate(int fd, double rate)
{
    bool rc;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "set_pmc8_custom_dec_move_rate() called rate=%f ", rate);

    // safe guard for now - only all use to STOP slewing with this
    if (fabs(rate) > get_pmc8_axis_max_move_rate(PMC8_AXIS_DEC))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "set_pmc8_custom_dec_move_rate only supports low rates currently");
        return false;
    }

    rc = set_pmc8_axis_move_rate(fd, PMC8_AXIS_DEC, rate);

    return rc;
}

// rate is fraction of sidereal
bool set_pmc8_guide_rate(int fd, PMC8_AXIS axis, double rate)
{
    if (pmc8_simulation)
    {
        simPMC8Data.guide_rate = rate;
        return true;
    }

    // set driver values
    if (axis == PMC8_AXIS_RA)
    {
        pmc8_sidereal_rate_fraction_ra = rate;
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "set_pmc8_guide_rate: ra guide rate set to %f", rate);
    }
    if ((axis == PMC8_AXIS_DEC) || !pmc8_isRev2Compliant)
    {
        pmc8_sidereal_rate_fraction_de = rate;
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "set_pmc8_guide_rate: dec guide rate set to %f", rate);
    }

    if (pmc8_isRev2Compliant)
    {
        // now write to mount to sync ST4 rates
        char cmd[32], expresp[32];
        int errcode = 0;
        char errmsg[MAXRBUF];
        char response[32];
        int nbytes_read    = 0;
        int nbytes_written = 0;

        snprintf(cmd, sizeof(cmd), "ESSf%d%02X!", axis, int(rate * 100));

        if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
            return false;
        }

        snprintf(expresp, sizeof(expresp), "ESGf%d%02X!", axis, int(rate * 100));

        if ((errcode = get_pmc8_response(fd, response, &nbytes_read, expresp)))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "SRF set cmd response incorrect: expected=%s", expresp);
            return false;
        }
    }

    return true;
}

// get SRF value for axis
bool get_pmc8_guide_rate(int fd, PMC8_AXIS axis, double &rate)
{
    if (pmc8_simulation)
    {
        rate =  simPMC8Data.guide_rate;
        return true;
    }

    // read from mount
    char cmd[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESGf%d!", axis);

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    cmd[5] = '\0';

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, cmd)))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting SRF rate");
        return false;
    }

    if (nbytes_read != 8)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "SRF Get rate cmd response incorrect");
        return false;
    }

    char num_str[16] = {0};

    strcpy(num_str, "0X");
    strncat(num_str, response + 5, 2);
    int tint = strtol(num_str, nullptr, 0);

    rate = ((double)tint) / 100;

    // set driver values
    if (axis == PMC8_AXIS_RA)
    {
        pmc8_sidereal_rate_fraction_ra = rate;
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "get_pmc8_guide_rate: ra guide rate set to %f", rate);
    }
    else
    {
        pmc8_sidereal_rate_fraction_de = rate;
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "get_pmc8_guide_rate: dec guide rate set to %f", rate);
    }

    return true;
}

bool get_pmc8_guide_state(PMC8_DIRECTION gdir, PulseGuideState **pstate)
{

    switch (gdir)
    {
        case PMC8_N:
        case PMC8_S:
            *pstate = &NS_PulseGuideState;
            break;

        case PMC8_W:
        case PMC8_E:
            *pstate = &EW_PulseGuideState;
            break;

        default:
            return false;
            break;
    }
    return true;
}

static bool get_pmc8_firmware_guide_active(int fd, bool &active)
{
    char cmd[8] = "ESGq!";
    char response[32] = {0};
    int errcode = 0;
    char errmsg[MAXRBUF];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGq")))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting firmware pulse-guide state");
        return false;
    }

    if (nbytes_read < 7)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Firmware pulse-guide state response incorrect: %s", response);
        return false;
    }

    active = (response[4] != '0') || (response[5] != '0');
    return true;
}

static bool start_pmc8_firmware_timed_guide(int fd, PMC8_DIRECTION gdir, int ms, long &timetaken_us,
                                            INDI::Telescope::TelescopePierSide pierSide, PulseGuideState *pstate)
{
    char cmd[24];
    char response[24] = {0};
    int errcode = 0;
    char errmsg[MAXRBUF];
    int nbytes_read    = 0;
    int nbytes_written = 0;
    int axis = 0;
    int direction = 0;
    int guideMs = ms > 0xFFFF ? 0xFFFF : ms;
    struct timeval tp;
    long long pulse_start_us;
    long long pulse_sofar_us;

    if (guideMs != ms)
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_WARNING, "Firmware pulse guide duration capped from %d ms to %d ms", ms, guideMs);

    switch (gdir)
    {
        case PMC8_E:
            axis = 0;
            direction = 1;
            break;
        case PMC8_W:
            axis = 0;
            direction = 0;
            break;
        case PMC8_N:
            if (pierSide == INDI::Telescope::PIER_UNKNOWN)
                return false;
            axis = 1;
            direction = (pierSide == INDI::Telescope::PIER_EAST) ? 1 : 0;
            break;
        case PMC8_S:
            if (pierSide == INDI::Telescope::PIER_UNKNOWN)
                return false;
            axis = 1;
            direction = (pierSide == INDI::Telescope::PIER_EAST) ? 0 : 1;
            break;
        default:
            return false;
    }

    snprintf(cmd, sizeof(cmd), "ESSq%d%d%04X!", axis, direction, guideMs);

    gettimeofday(&tp, nullptr);
    pulse_start_us = tp.tv_sec * 1000000 + tp.tv_usec;

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, "ESGq")))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error sending firmware-timed pulse guide");
        return false;
    }

    if (nbytes_read < 7)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Firmware-timed pulse guide response incorrect: %s", response);
        return false;
    }

    bool guideConfirmed = false;
    for (int i = 0; i < 10; i++)
    {
        bool guideActive = false;
        usleep(50000);
        if (get_pmc8_firmware_guide_active(fd, guideActive) && guideActive)
        {
            guideConfirmed = true;
            break;
        }
    }

    gettimeofday(&tp, nullptr);
    pulse_sofar_us = (tp.tv_sec * 1000000 + tp.tv_usec) - pulse_start_us;

    pstate->pulseguideactive = true;
    pstate->fakepulse = false;
    pstate->firmwaretimed = true;
    pstate->ms = guideMs;
    pstate->pulse_start_us = pulse_start_us;
    pstate->cur_rate = 0;
    pstate->cur_dir = -1;
    pstate->new_rate = 0;
    pstate->new_dir = direction;

    timetaken_us = pulse_sofar_us;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG,
                 "Firmware-timed pulse guide sent: dir=%d axis=%d guide_dir=%d ms=%d confirmed=%d timetaken_us=%d",
                 gdir, axis, direction, guideMs, guideConfirmed, timetaken_us);

    return true;
}

// if return value is true then timetaken will return how much pulse time has already occurred
bool start_pmc8_guide(int fd, PMC8_DIRECTION gdir, int ms, long &timetaken_us, double ratehint,
                      INDI::Telescope::TelescopePierSide pierSide)
{
    bool rc;
    double cur_rate = 0;
    int cur_dir = -1;

    // used to test timing
    struct timeval tp;
    long long pulse_start_us;
    long long pulse_sofar_us;

    PulseGuideState *pstate;

    if (!get_pmc8_guide_state(gdir, &pstate))
    {
        return false;
    }

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_start_guide(): pulse dir=%d dur=%d ms", gdir, ms);

    if (pstate->pulseguideactive)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "pmc8_start_guide(): already executing a pulse guide!");
        return false;
    }

    // ignore short pulses - they do nothing
    if (ms < PMC8_PULSE_GUIDE_MIN_MS)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_start_guide(): ignore short pulse ms=%d ms", ms);
        timetaken_us = ms * 1000;
        pstate->pulseguideactive = true;
        pstate->fakepulse = true;
        pstate->firmwaretimed = false;
        return true;
    }

    if (pmc8_isRev2Compliant)
    {
        if (start_pmc8_firmware_timed_guide(fd, gdir, ms, timetaken_us, pierSide, pstate))
            return true;

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG,
                    "Firmware-timed pulse guide failed; falling back to legacy timer/rate pulse guide");
    }

    // get precise tracking rate if in RA
    if ((gdir == PMC8_E) || (gdir == PMC8_W))
    {

        // use rate provided by interface if valid rather than querying for it
        if (ratehint <= 0)
        {
            rc = get_pmc8_track_rate(fd, cur_rate);
            if (!rc)
            {
                DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "pmc8_start_guide(): error reading current RA rate!");
                return rc;
            }
        }
        else
            cur_rate = ratehint;
    }
    // we could get slew rate if in DEC, but driver doesn't currently support DEC tracking
    // and we shouldn't get here if we're slewing, so for now we assume interface is always correct and avoid delay from unnecessary calls to mount
    else
    {
        cur_rate = ratehint;
    }

    // if slewing abort
    // shouldn't get here if slewing, but doesn't hurt to check
    if (cur_rate > PMC8_MAX_TRACK_RATE)
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR,
                     "pmc8_start_guide(): Cannot send guide correction while slewing! rate=%d dir=%d",
                     cur_rate, gdir);
        return rc;
    }

    double new_rate = cur_rate;
    int new_dir = 0;

    // RA guiding routine just changes the precision tracking call
    if ((gdir == PMC8_E) || (gdir == PMC8_W))
    {
        double guide_rate = pmc8_sidereal_rate_fraction_ra * PMC8_RATE_SIDEREAL;

        if (gdir == PMC8_E) new_rate -= guide_rate;
        else new_rate += guide_rate;

        if (new_rate < 0)
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG,
                         "pmc8_start_guide(): with current tracking rate of %f, requested guide rate of %f would flip RA motor in opposite direction, so pausing motor instead.",
                         cur_rate, new_rate);
            new_rate = 0;
        }

        // measure time when we start pulse
        gettimeofday(&tp, nullptr);
        pulse_start_us = tp.tv_sec * 1000000 + tp.tv_usec;

        if (!set_pmc8_custom_ra_track_rate(fd, new_rate))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "pmc8_start_guide(): error settings new_rate to %f", new_rate);
            return false;
        }

    }
    // DEC guiding routine needs to set a DEC move rate and possibly a new direction
    // Southern Hemisphere support: In the southern hemisphere, the celestial pole
    // is in the opposite direction, so guide pulses need to be inverted.
    // pmc8_east_dir is 1 for northern hemisphere, 0 for southern hemisphere.
    else if ((gdir == PMC8_N) || (gdir == PMC8_S))
    {
        double guide_rate = pmc8_sidereal_rate_fraction_de * PMC8_RATE_SIDEREAL;

        // Determine effective guide direction considering hemisphere
        // In southern hemisphere, N/S guide commands need to be inverted
        bool effectiveSouth = (gdir == PMC8_S);
        if (!pmc8_east_dir) effectiveSouth = !effectiveSouth;  // flip for southern hemisphere

        if (effectiveSouth) new_rate -= guide_rate;
        else new_rate += guide_rate;

        if (new_rate < 0) new_dir = 1;

        int mrate;
        if (!convert_move_rate_to_motor_axis(PMC8_AXIS_DEC, fabs(new_rate), &mrate))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error converting rate %f", new_rate);
            return false;
        }

        // we should flip direction first so that we decrease the distance we could be going in the wrong direction
        // this is of course obvious with dec assumed to be 0, but just a reminder in case we ever support dec tracking

        // ideally, we would set direction only if needed
        // but based on our current assumptions, that could cost us an extra call to find out the current direction
        // so for now we'll always end up setting the direction
        if (cur_dir != new_dir)
            if (!set_pmc8_direction_axis(fd, PMC8_AXIS_DEC, new_dir, false))
                DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_start_guide(): error setting new_dec_dir");

        // measure time when we start pulse
        gettimeofday(&tp, nullptr);
        pulse_start_us = tp.tv_sec * 1000000 + tp.tv_usec;

        if (!set_pmc8_axis_motor_rate(fd, PMC8_AXIS_DEC, mrate, false))
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_start_guide(): error setting new_dec_rate");

    }
    // if for some reason the gdir is non-sensical ...
    else
    {
        return false;
    }

    // store state
    pstate->pulseguideactive = true;
    pstate->fakepulse = false;
    pstate->firmwaretimed = false;
    pstate->ms = ms;
    pstate->pulse_start_us = pulse_start_us;
    pstate->cur_rate  = cur_rate;
    pstate->cur_dir   = cur_dir;
    pstate->new_rate  = new_rate;
    pstate->new_dir   = new_dir;

    // see how long we've waited
    gettimeofday(&tp, nullptr);
    pulse_sofar_us = (tp.tv_sec * 1000000 + tp.tv_usec) - pulse_start_us;

    timetaken_us = pulse_sofar_us;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_start_guide(): timetaken_us=%d us", timetaken_us);

    return true;
}

bool stop_pmc8_guide(int fd, PMC8_DIRECTION gdir)
{
    struct timeval tp;
    long long pulse_end_us;

    PulseGuideState *pstate;

    if (!get_pmc8_guide_state(gdir, &pstate))
    {
        return false;
    }

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_stop_guide(): pulse dir=%d dur=%d ms", gdir, pstate->ms);

    if (!pstate->pulseguideactive)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "pmc8_stop_guide(): pulse guide not active!!");
        return false;
    }

    // flush any responses to commands we ignored above!
    tcflush(fd, TCIFLUSH);

    if (pstate->firmwaretimed)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_stop_guide(): firmware-timed pulse completed");
        pstate->pulseguideactive = false;
        pstate->firmwaretimed = false;
        return true;
    }

    // "fake pulse" - it was so short we would have overshot its length AND the motors wouldn't have moved anyways
    if (pstate->fakepulse)
    {

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_stop_guide(): fake pulse done");
        pstate->pulseguideactive = false;
        return true;
    }

    gettimeofday(&tp, nullptr);
    pulse_end_us = tp.tv_sec * 1000000 + tp.tv_usec;

    if ((gdir == PMC8_E) || (gdir == PMC8_W))
    {
        if (!set_pmc8_custom_ra_track_rate(fd, pstate->cur_rate))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "pmc8_stop_guide(): error restoring tracking_rate to %f",
                         pstate->cur_rate);
            return false;
        }
    }
    else if ((gdir == PMC8_N) || (gdir == PMC8_S))
    {
        int mrate;

        if (!convert_move_rate_to_motor_axis(PMC8_AXIS_DEC, fabs(pstate->cur_rate), &mrate))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error converting rate %f", pstate->cur_rate);
            return false;
        }

        // under assumption of no dec tracking, all we need to do is stop motion
        // but if dec tracking is ever supported, need to fix direction, and it may be better to do that first if cur_rate > new_rate
        if (!set_pmc8_axis_motor_rate(fd, PMC8_AXIS_DEC, mrate, false))
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_stop_guide(): error returning to old move rate");
        // only change direction if needed
        if ((pstate->cur_rate != 0) && (pstate->cur_dir != pstate->new_dir))
            if (!set_pmc8_direction_axis(fd, PMC8_AXIS_DEC, pstate->cur_dir, false))
                DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_stop_guide(): error returning to old direction");
    }
    else
    {
        return false;
    }

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "pmc8_stop_guide(): requested = %d ms, actual = %f ms",
                 pstate->ms, (pulse_end_us - pstate->pulse_start_us) / 1000.0);

    // flush any responses to commands we ignored above!
    tcflush(fd, TCIFLUSH);

    // mark pulse done
    pstate->pulseguideactive = false;

    return true;
}

// convert from axis position returned by controller to motor counts used in conversion to RA/DEC
int convert_axispos_to_motor(int axispos)
{
    int r;

    if (axispos > 8388608)
        r = 0 - (16777216 - axispos);
    else
        r = axispos;

    return r;
}

bool convert_ra_to_motor(double ra, INDI::Telescope::TelescopePierSide sop, int *mcounts)
{
    double motor_angle;
    double hour_angle;
    double lst;

    INDI_UNUSED(sop);

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "convert_ra_to_motor - ra=%f sop=%d", ra, sop);

    lst = get_local_sidereal_time(pmc8_longitude);

    hour_angle = lst - ra;

    // limit values to +/- 12 hours
    if (hour_angle > 12)
        hour_angle = hour_angle - 24;
    else if (hour_angle <= -12)
        hour_angle = hour_angle + 24;

    // ASCOM parity: determine the RA motor offset from hour-angle sign, not from
    // the caller's pier side. MSRO use HA directly because home is HA 0.
    if (hour_angle <= 0)
    {
        if (pmc8_mount_msro_geometry)
            motor_angle = hour_angle;
        else
            motor_angle = hour_angle + 6;
    }
    else
    {
        if (pmc8_mount_msro_geometry)
            motor_angle = hour_angle;
        else
            motor_angle = hour_angle - 6;
    }

    if (!pmc8_east_dir)
        motor_angle = -motor_angle;

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "convert_ra_to_motor - lst = %f hour_angle=%f", lst, hour_angle);

    *mcounts = motor_angle * PMC8_AXIS0_SCALE / 24;

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "convert_ra_to_motor - motor_angle=%f *mcounts=%d", motor_angle, *mcounts);


    return true;
}

bool convert_motor_to_radec(int racounts, int deccounts, double &ra_value, double &dec_value)
{
    double motor_angle;
    double hour_angle;

    double lst;

    lst = get_local_sidereal_time(pmc8_longitude);

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "lst = %f", lst);

    motor_angle = (24.0 * racounts) / PMC8_AXIS0_SCALE;

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "racounts = %d  motor_angle = %f", racounts, motor_angle);

    // Northern Hemisphere
    if (pmc8_east_dir)
    {
        if (deccounts < 0)
            hour_angle = pmc8_mount_msro_geometry ? motor_angle : motor_angle + 6;
        else
            hour_angle = pmc8_mount_msro_geometry ? motor_angle : motor_angle - 6;
    }
    // Southern Hemisphere
    else
    {
        if (deccounts < 0)
            hour_angle = pmc8_mount_msro_geometry ? -motor_angle : -(motor_angle + 6);
        else
            hour_angle = pmc8_mount_msro_geometry ? -motor_angle : -(motor_angle - 6);
    }

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "hour_angle = %f", hour_angle);

    ra_value = lst - hour_angle;

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ra_value  = %f", ra_value);

    if (ra_value >= 24.0)
        ra_value = ra_value - 24.0;
    else if (ra_value < 0.0)
        ra_value = ra_value + 24.0;

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ra_value (final) = %f", ra_value);

    motor_angle = (360.0 * deccounts) / PMC8_AXIS1_SCALE;

    // Northern Hemisphere
    if (pmc8_east_dir)
    {
        if (motor_angle >= 0)
            dec_value = pmc8_mount_msro_geometry ? -motor_angle : 90 - motor_angle;
        else
            dec_value = pmc8_mount_msro_geometry ? -motor_angle : 90 + motor_angle;
    }
    // Southern Hemisphere
    else
    {
        if (motor_angle >= 0)
            dec_value = pmc8_mount_msro_geometry ? -motor_angle : -90 + motor_angle;
        else
            dec_value = pmc8_mount_msro_geometry ? motor_angle : -90 - motor_angle;
    }

    return true;
}

bool convert_dec_to_motor(double dec, INDI::Telescope::TelescopePierSide sop, int *mcounts)
{
    double motor_angle;

    // Northern Hemisphere
    if (pmc8_east_dir)
    {
        if (sop == INDI::Telescope::PIER_EAST)
            motor_angle = pmc8_mount_msro_geometry ? -dec : (dec - 90.0);
        else if (sop == INDI::Telescope::PIER_WEST)
            motor_angle = pmc8_mount_msro_geometry ? -dec : -(dec - 90.0);
        else
            return false;
    }
    // Southern Hemisphere
    else
    {
        if (sop == INDI::Telescope::PIER_EAST)
            motor_angle = pmc8_mount_msro_geometry ? dec : -(dec + 90.0);
        else if (sop == INDI::Telescope::PIER_WEST)
            motor_angle = pmc8_mount_msro_geometry ? dec : (dec + 90.0);
        else
            return false;
    }

    *mcounts = (motor_angle / 360.0) * PMC8_AXIS1_SCALE;

    //     DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "convert_dec_to_motor dec = %f, sop = %d", dec, sop);
    //     DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "convert_dec_to_motor motor_angle = %f, motor_counts= %d", motor_angle, *mcounts);

    return true;
}

bool set_pmc8_target_position_axis(int fd, PMC8_AXIS axis, int point)
{

    char cmd[32];
    char expresp[32];
    char hexpt[16];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    convert_motor_counts_to_hex(point, hexpt);

    // for v2+ firmware, use axis 2 if we don't want to track after the slew
    int naxis = axis;
    if (pmc8_isRev2Compliant && !axis && !pmc8_goto_resume) naxis = 2;
    snprintf(cmd, sizeof(cmd), "ESPt%d%s!", naxis, hexpt);

    if (!pmc8_simulation)
    {

        if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
            return false;
        }

        snprintf(expresp, sizeof(expresp), "ESGt%d%s!", naxis, hexpt);

        if ((errcode = get_pmc8_response(fd, response, &nbytes_read, expresp)))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Axis Set Point cmd response incorrect: %s - expected %s", response,
                         expresp);
            return false;
        }
    }

    return true;
}

bool set_pmc8_target_position(int fd, int rapoint, int decpoint)
{
    bool rc;

    rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_RA, rapoint);

    if (!rc)
        return rc;

    rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_DEC, decpoint);

    return rc;
}


bool set_pmc8_position_axis(int fd, PMC8_AXIS axis, int point)
{

    char cmd[32];
    char expresp[32];
    char hexpt[16];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    if (pmc8_simulation)
    {
        // FIXME - need to implement simulation code for setting point position
        return true;
    }

    convert_motor_counts_to_hex(point, hexpt);
    snprintf(cmd, sizeof(cmd), "ESSp%d%s!", axis, hexpt);

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    snprintf(expresp, sizeof(expresp), "ESGp%d%s!", axis, hexpt);

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, expresp)))
    {
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Axis Set Point cmd response incorrect: %s - expected %s", response,
                     expresp);
        return false;
    }

    return true;
}


bool set_pmc8_position(int fd, int rapoint, int decpoint)
{
    bool rc;

    rc = set_pmc8_position_axis(fd, PMC8_AXIS_RA, rapoint);

    if (!rc)
        return rc;

    rc = set_pmc8_position_axis(fd, PMC8_AXIS_DEC, decpoint);

    return rc;
}


bool get_pmc8_position_axis(int fd, PMC8_AXIS axis, int &point)
{

    char cmd[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    if (pmc8_simulation)
    {
        // FIXME - need to implement simulation code for setting point position
        return true;
    }

    snprintf(cmd, sizeof(cmd), "ESGp%d!", axis);

    if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
    {
        tty_error_msg(errcode, errmsg, MAXRBUF);
        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
        return false;
    }

    cmd[5] = '\0';

    if ((errcode = get_pmc8_response(fd, response, &nbytes_read, cmd)))
    {
        if (pmc8_connection == PMC8_ETHERNET && pmc8_last_axis_position_valid[axis])
        {
            point = pmc8_last_axis_position[axis];
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_WARNING,
                         "Using previous axis %d position after WiFi read failure: %d", axis, point);
            return true;
        }

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error getting position axis");
        return false;
    }

    if (nbytes_read != 12)
    {
        if (pmc8_connection == PMC8_ETHERNET && pmc8_last_axis_position_valid[axis])
        {
            point = pmc8_last_axis_position[axis];
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_WARNING,
                         "Using previous axis %d position after invalid WiFi response length: %d", axis, point);
            return true;
        }

        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Axis Get Point cmd response incorrect");
        return false;
    }

    char num_str[16] = {0};

    strcpy(num_str, "0X");
    strncat(num_str, response + 5, 6);

    point = (int)strtol(num_str, nullptr, 0);
    pmc8_last_axis_position[axis] = point;
    pmc8_last_axis_position_valid[axis] = true;

    return true;
}


bool get_pmc8_position(int fd, int &rapoint, int &decpoint)
{
    bool rc;
    int axis_ra_pos, axis_dec_pos;

    rc = get_pmc8_position_axis(fd, PMC8_AXIS_RA, axis_ra_pos);

    if (!rc)
        return rc;

    rc = get_pmc8_position_axis(fd, PMC8_AXIS_DEC, axis_dec_pos);

    if (!rc)
        return rc;

    // convert from axis position to motor counts
    rapoint = convert_axispos_to_motor(axis_ra_pos);
    decpoint = convert_axispos_to_motor(axis_dec_pos);

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ra  axis pos = 0x%x  motor_counts=%d",  axis_ra_pos,  rapoint);
    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "dec axis pos = 0x%x  motor_counts=%d", axis_dec_pos, decpoint);

    return rc;
}


bool park_pmc8(int fd, int rapoint, int decpoint)
{

    bool rc;
    const bool saved_goto_resume = pmc8_goto_resume;

    // ASCOM parks with ESPt2 on RA for Rev2 firmware so the controller stops at the
    // target, waits for RA to move clear, then starts DEC toward the pole.
    pmc8_goto_resume = false;
    rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_RA, rapoint);
    pmc8_goto_resume = saved_goto_resume;

    if (!rc)
        return rc;

    usleep(3000000);

    rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_DEC, decpoint);

    // FIXME - Need to add code to handle simulation and also setting any scope state values

    return rc;
}


bool unpark_pmc8(int fd)
{
    INDI_UNUSED(fd);

    // nothing really to do for PMC8 there is no unpark command

    if (pmc8_simulation)
    {
        set_pmc8_sim_system_status(ST_STOPPED);
        return true;
    }


    // FIXME - probably need to set a state variable to show we're unparked
    DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "PMC8 unparked");

    return true;
}

bool home_pmc8(int fd)
{
    bool rc;
    const bool saved_goto_resume = pmc8_goto_resume;

    // Match ASCOM FindHome: command RA to motor zero using ESPt2 so Rev2 firmware
    // stops at target, then command DEC to motor zero.
    pmc8_goto_resume = false;
    rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_RA, 0);
    pmc8_goto_resume = saved_goto_resume;

    if (!rc)
        return rc;

    rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_DEC, 0);

    return rc;
}

bool get_pmc8_is_at_motor_position(int fd, int target_ra, int target_dec, int tolerance_counts,
                                    int &actual_ra, int &actual_dec, bool &is_at_position)
{
    if (!get_pmc8_position(fd, actual_ra, actual_dec))
        return false;

    const int ra_error = abs(actual_ra - target_ra);
    const int dec_error = abs(actual_dec - target_dec);
    is_at_position = (ra_error < tolerance_counts) && (dec_error < tolerance_counts);

    return true;
}

bool abort_pmc8(int fd)
{
    bool rc;

    if (pmc8_simulation)
    {
        // FIXME - need to do something to represent mount has stopped slewing
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "PMC8 slew stopped in simulation - need to add more code?");
        return true;
    }

    // stop move/slew rates
    rc = set_pmc8_custom_ra_move_rate(fd, 0);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error stopping RA axis!");
        return false;
    }

    rc = set_pmc8_custom_dec_move_rate(fd, 0);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error stopping DEC axis!");
        return false;
    }

    return true;
}

bool abort_pmc8_goto(int fd)
{
    char cmd[32];
    char expresp[32];
    int errcode = 0;
    char errmsg[MAXRBUF];
    char response[32];
    int nbytes_read    = 0;
    int nbytes_written = 0;

    snprintf(cmd, sizeof(cmd), "ESPt3000000!");

    if (!pmc8_simulation)
    {

        if ((errcode = send_pmc8_command(fd, cmd, strlen(cmd), &nbytes_written)) != TTY_OK)
        {
            tty_error_msg(errcode, errmsg, MAXRBUF);
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "%s", errmsg);
            return false;
        }

        snprintf(expresp, sizeof(expresp), "ESGt3!");

        if ((errcode = get_pmc8_response(fd, response, &nbytes_read, expresp)))
        {
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Abort Goto cmd response incorrect: %s - expected %s", response,
                         expresp);
            return false;
        }
    }

    return true;
}

// "slew" on PMC8 is instantaneous once you set the target ra/dec
// no concept of setting target and then starting a slew operation as two steps
bool slew_pmc8(int fd, double ra, double dec, bool compensate_ra)
{
    bool rc;
    int racounts, deccounts;
    bool slewRA = true;
    INDI::Telescope::TelescopePierSide sop;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "slew_pmc8: ra=%f  dec=%f", ra, dec);

    sop = slewDestinationSideOfPier(ra, dec);

    rc = convert_ra_to_motor(ra, sop, &racounts);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "slew_pmc8: error converting RA to motor counts");
        return false;
    }

    rc = convert_dec_to_motor(dec, sop, &deccounts);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "slew_pmc8: error converting DEC to motor counts");
        return false;
    }

    if (pmc8_ascom_slew_compensation && compensate_ra)
    {
        int curRAcounts = 0;
        int curDECcounts = 0;

        rc = get_pmc8_position(fd, curRAcounts, curDECcounts);
        if (!rc)
        {
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "slew_pmc8: error reading current motor counts for ASCOM RA compensation");
            return false;
        }

        const int moveDistance = abs(racounts - curRAcounts);
        const double moveOffsetSign = pmc8_latitude < 0.0 ? -1.0 : 1.0;
        int raOffset = 0;

        if (moveDistance < round(PMC8_ASCOM_SHORT_MOVE_BASE_COUNTS - 2.0 * PMC8_AXIS0_SCALE / 86400.0))
        {
            raOffset = round(moveOffsetSign * 2.0 * PMC8_AXIS0_SCALE / 86400.0);
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ASCOM RA slew compensation using short-move offset");
        }
        else if (moveDistance > (2.0 * pmc8_mount_max_slew_rate_counts))
        {
            const bool slewWest = curRAcounts < racounts;
            const double longMoveOffset = slewWest ? pmc8_mount_long_move_offset_west : pmc8_mount_long_move_offset_east;
            raOffset = round(((double)moveDistance / pmc8_mount_max_slew_rate_counts + moveOffsetSign * longMoveOffset) *
                             (PMC8_AXIS0_SCALE / 86400.0));
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ASCOM RA slew compensation using long-move offset");
        }
        else
        {
            const bool slewWest = curRAcounts < racounts;
            const double rampOnlyOffset = slewWest ? pmc8_mount_ramp_only_offset_west : pmc8_mount_ramp_only_offset_east;
            raOffset = round(moveOffsetSign * rampOnlyOffset * PMC8_AXIS0_SCALE / 86400.0);
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ASCOM RA slew compensation using ramp-only offset");
        }

        DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG,
                     "ASCOM RA slew compensation: current=%d target_before=%d distance=%d offset=%d target_after=%d latitude=%f",
                     curRAcounts, racounts, moveDistance, raOffset, racounts + raOffset, pmc8_latitude);
        // Match ASCOM behavior: if RA is already at the requested count, only send DEC.
        // Sending an RA target in this case can create an unnecessary finishing move.
        if (moveDistance > 1)
            racounts += raOffset;
        else
        {
            slewRA = false;
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ASCOM RA slew compensation skipping RA target for DEC-only slew");
        }
    }

    if (slewRA)
        rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_RA, racounts);
    else
        rc = true;

    if (rc)
        rc = set_pmc8_target_position_axis(fd, PMC8_AXIS_DEC, deccounts);

    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error slewing PMC8");
        return false;
    }

    if (pmc8_simulation)
    {
        set_pmc8_sim_system_status(ST_SLEWING);
    }

    return true;
}

bool get_pmc8_slew_target_error(int fd, double ra, double dec, int &raError, int &decError,
                                int &raActual, int &decActual, int &raTarget, int &decTarget)
{
    INDI::Telescope::TelescopePierSide sop = slewDestinationSideOfPier(ra, dec);

    if (!convert_ra_to_motor(ra, sop, &raTarget))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_slew_target_error: error converting RA to motor counts");
        return false;
    }

    if (!convert_dec_to_motor(dec, sop, &decTarget))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_slew_target_error: error converting DEC to motor counts");
        return false;
    }

    if (!get_pmc8_position(fd, raActual, decActual))
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "get_pmc8_slew_target_error: error reading current motor counts");
        return false;
    }

    raError  = abs(raTarget - raActual);
    decError = abs(decTarget - decActual);

    return true;
}

INDI::Telescope::TelescopePierSide destSideOfPier(double ra, double dec)
{
    double hour_angle;
    double lst;

    INDI_UNUSED(dec);

    lst = get_local_sidereal_time(pmc8_longitude);

    hour_angle = lst - ra;

    // limit values to +/- 12 hours
    if (hour_angle > 12)
        hour_angle = hour_angle - 24;
    else if (hour_angle <= -12)
        hour_angle = hour_angle + 24;

    // ASCOM convention: destination pier side is based on hour angle and is the same
    // in both hemispheres. Slew-time DEC motor conversion uses a private southern
    // hemisphere flip to preserve the PMC-Eight motor-count geometry.
    if (hour_angle < 0.0)
        return INDI::Telescope::PIER_WEST;
    else
        return INDI::Telescope::PIER_EAST;
}

static INDI::Telescope::TelescopePierSide slewDestinationSideOfPier(double ra, double dec)
{
    INDI::Telescope::TelescopePierSide sop = destSideOfPier(ra, dec);

    if (!pmc8_east_dir)
        return (sop == INDI::Telescope::PIER_WEST) ? INDI::Telescope::PIER_EAST : INDI::Telescope::PIER_WEST;

    return sop;
}

bool sync_pmc8(int fd, double ra, double dec)
{
    bool rc;
    int racounts, deccounts;
    INDI::Telescope::TelescopePierSide sop;

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "sync_pmc8: ra=%f  dec=%f", ra, dec);

    sop = slewDestinationSideOfPier(ra, dec);

    rc = convert_ra_to_motor(ra, sop, &racounts);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "sync_pmc8: error converting RA to motor counts");
        return false;
    }

    rc = convert_dec_to_motor(dec, sop, &deccounts);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "sync_pmc8: error converting DEC to motor counts");
        return false;
    }

    if (pmc8_simulation)
    {
        // FIXME - need to implement pmc8 sync sim
        //        strcpy(response, "1");
        //        nbytes_read = strlen(response);
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Need to implement PMC8 sync simulation");
        return false;
    }
    else
    {
        rc = set_pmc8_position(fd, racounts, deccounts);
    }

    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error setting pmc8 position");
        return false;
    }

    return true;
}

bool set_pmc8_radec(int fd, double ra, double dec)
{
    bool rc;
    int racounts, deccounts;
    INDI::Telescope::TelescopePierSide sop;


    sop = slewDestinationSideOfPier(ra, dec);

    rc = convert_ra_to_motor(ra, sop, &racounts);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "set_pmc8_radec: error converting RA to motor counts");
        return false;
    }

    rc = convert_dec_to_motor(dec, sop, &deccounts);
    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "set_pmc8_radec: error converting DEC to motor counts");
        return false;
    }

    if (pmc8_simulation)
    {
        // FIXME - need to implement pmc8 sync sim
        //        strcpy(response, "1");
        //        nbytes_read = strlen(response);
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Need to implement PMC8 sync simulation");
        return false;
    }
    else
    {

        rc = set_pmc8_target_position(fd, racounts, deccounts);
    }

    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Error setting target position");
        return false;
    }

    return true;
}

bool get_pmc8_coords(int fd, double &ra, double &dec)
{
    int racounts, deccounts;
    bool rc;

    if (pmc8_simulation)
    {
        // sortof silly but convert simulated RA/DEC to counts so we can then convert
        // back to RA/DEC to test that conversion code
        INDI::Telescope::TelescopePierSide sop;

        sop = slewDestinationSideOfPier(simPMC8Data.ra, simPMC8Data.dec);

        rc = convert_ra_to_motor(simPMC8Data.ra, sop, &racounts);

        if (!rc)
            return rc;

        rc = convert_dec_to_motor(simPMC8Data.dec, sop, &deccounts);

        if (!rc)
            return rc;
    }
    else
    {
        rc = get_pmc8_position(fd, racounts, deccounts);
    }

    if (!rc)
    {
        DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "Error getting PMC8 motor position");
        return false;
    }

    // convert motor counts to ra/dec
    convert_motor_to_radec(racounts, deccounts, ra, dec);

    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "ra  motor_counts=%d  RA  = %f", racounts, ra);
    //    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "dec motor_counts=%d  DEC = %f", deccounts, dec);

    return rc;
}

static void sanitize_pmc8_ethernet_response(char *buf, int *nbytes_read, const char *expected)
{
    if (buf == nullptr || nbytes_read == nullptr || *nbytes_read <= 0)
        return;

    buf[*nbytes_read] = '\0';

    char *start = buf;

    // ASCOM's WiFi read skips control characters before the first printable byte.
    while (*start != '\0' && static_cast<unsigned char>(*start) < 0x20)
        start++;

    // RN-131 style WiFi modules can send a greeting into the command stream.
    if (strncmp(start, "*HELLO*", 7) == 0)
        start += 7;

    while (*start != '\0' && static_cast<unsigned char>(*start) < 0x20)
        start++;

    // Reconnects can leave an AT echo ahead of the PMC-Eight response.
    if (strncmp(start, "AT", 2) == 0)
        start += 2;

    while (*start != '\0' && static_cast<unsigned char>(*start) < 0x20)
        start++;

    // If a valid response is present after noise, keep the response and discard the noise.
    if (expected != nullptr)
    {
        char *expected_start = strstr(start, expected);
        if (expected_start != nullptr)
            start = expected_start;
    }

    if (start != buf)
    {
        memmove(buf, start, strlen(start) + 1);
        *nbytes_read = strlen(buf);
    }
}

// wrap read commands to PMC8
bool get_pmc8_response(int fd, char* buf, int *nbytes_read, const char* expected = NULL )
{
    int err_code = 1;
    int cnt = 0;

    //repeat a few times, after that, let's assume we're not getting a response
    while ((err_code) && (cnt++ < PMC8_MAX_RETRIES))
    {
        *nbytes_read = 0;

        //Read until exclamation point to get response
        if ((err_code = tty_read_section(fd, buf, '!', PMC8_TIMEOUT, nbytes_read)))
        {

            char errmsg[MAXRBUF];
            tty_error_msg(err_code, errmsg, MAXRBUF);

            // if we see connection timed out, exit out of here and try to reconnect
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "Read error: %s", errmsg);
            if (strstr(errmsg, "Connection timed out") || strstr(errmsg, "Bad"))
            {
                if (pmc8_connection == PMC8_ETHERNET)
                {
                    usleep(PMC8_WIFI_REFRACTION_USEC);
                    continue;
                }
                else
                {
                    set_pmc8_reconnect_flag();
                    return err_code;
                }
            }
        }
        if (*nbytes_read > 0)
        {
            buf[*nbytes_read] = '\0';
            DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "RES %d bytes (%s)", *nbytes_read, buf);

            //PMC8 connection is not entirely reliable when using Ethernet instead of Serial connection.
            //So, try to compensate for common problems
            if (pmc8_connection == PMC8_ETHERNET)
            {
                sanitize_pmc8_ethernet_response(buf, nbytes_read, expected);

                //Another problem is random extraneous ESGp! reponses during slew, so when we see those, drop them and try again
                if (strncmp(buf, "ESGp!", 5) == 0)
                {
                    err_code = 1;
                    DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "Invalid response ESGp!");
                }
            }
            //If a particular response was expected, make sure we got it
            if (expected)
            {
                if (strncmp(buf, expected, strlen(expected)) != 0)
                {
                    err_code = 1;
                    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_EXTRA_1, "No Match for %s", expected);
                }
                else
                {
                    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_EXTRA_1, "Matches %s", expected);
                    // On rare occasions, there may have been a read error even though it's the response we want, so set err_code explicitly
                    err_code = 0;
                    if (pmc8_connection == PMC8_ETHERNET)
                        usleep(PMC8_WIFI_REFRACTION_USEC);
                }
            }
        }
        else
        {
            DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "No Response");
            err_code = 1;
        }
    }
    // if this is our nth consecutive read error, try to reconnect
    if (err_code)
    {
        if (++pmc8_io_error_ctr > PMC8_MAX_IO_ERROR_THRESHOLD) set_pmc8_reconnect_flag();
    }
    else
    {
        pmc8_io_error_ctr = 0;
    }
    return err_code;
}

//wrap write commands to pmc8
bool send_pmc8_command(int fd, const char *buf, int nbytes, int *nbytes_written)
{

    DEBUGFDEVICE(pmc8_device, INDI::Logger::DBG_DEBUG, "CMD (%s)", buf);

    tcflush(fd, TCIFLUSH);

    int err_code = 1;
    //try to reconnect if we see broken pipe error
    if ((err_code = tty_write(fd, buf, nbytes, nbytes_written)))
    {
        char errmsg[MAXRBUF];
        tty_error_msg(err_code, errmsg, MAXRBUF);
        if (strstr(errmsg, "Broken pipe") || strstr(errmsg, "Bad"))
        {
            set_pmc8_reconnect_flag();
            return err_code;
        }
    }
    return err_code;
}

void set_pmc8_reconnect_flag()
{
    DEBUGDEVICE(pmc8_device, INDI::Logger::DBG_ERROR, "Bad connection. Trying to reconnect.");
    pmc8_reconnect_flag = true;
}

bool get_pmc8_reconnect_flag()
{
    if (pmc8_reconnect_flag)
    {
        pmc8_reconnect_flag = false;
        return true;
    }
    return false;
}

void set_pmc8_goto_resume(bool resume)
{
    pmc8_goto_resume = resume;
}

// Helper to expose pmc8_east_dir to the high-level driver (pmc8.cpp)
// Returns 1 for Northern Hemisphere, 0 for Southern Hemisphere
int get_pmc8_east_dir()
{
    return pmc8_east_dir;
}
