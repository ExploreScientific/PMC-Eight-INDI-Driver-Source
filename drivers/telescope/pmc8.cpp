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
/* Experimental Mount selector switch G11 vs EXOS2 by Thomas Olson
 *
 */

#include "pmc8.h"

#include <indicom.h>
#include <connectionplugins/connectionserial.h>
#include <connectionplugins/connectiontcp.h>

#include <libnova/sidereal_time.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <math.h>
#include <string.h>

/* Simulation Parameters */
#define SLEWRATE 3          /* slew rate, degrees/s */
#define PMC8_AXIS_POSITION_WRAP 16777216
#define PMC8_AXIS_POSITION_SIGN 8388608

#define MOUNTINFO_TAB "Mount Info"

#define PMC8_DEFAULT_PORT 54372
#define PMC8_DEFAULT_IP_ADDRESS "192.168.47.1"
#define PMC8_TRACKING_AUTODETECT_INTERVAL 10
#define PMC8_VERSION_MAJOR 0
#define PMC8_VERSION_MINOR 5
#define PMC8_ASCOM_CORRECTION_THRESHOLD_COUNTS 10
#define PMC8_ASCOM_CORRECTION_SETTLE_POLLS 2
#define PMC8_PARK_POSITION_TOLERANCE_COUNTS 250
#define PMC8_HOME_POSITION_TOLERANCE_COUNTS 100

static std::unique_ptr<PMC8> scope(new PMC8());

static double motorCountsToAxisPark(int motorCounts)
{
    return motorCounts < 0 ? PMC8_AXIS_POSITION_WRAP + motorCounts : motorCounts;
}

static int axisParkToMotorCounts(double axisPosition)
{
    const int position = static_cast<int>(round(axisPosition));
    return position > PMC8_AXIS_POSITION_SIGN ? 0 - (PMC8_AXIS_POSITION_WRAP - position) : position;
}

/* Constructor */
PMC8::PMC8() : GI(this)
{
    currentRA  = ln_get_apparent_sidereal_time(ln_get_julian_from_sys());
    if (LocationNP[LOCATION_LATITUDE].getValue() < 0)
        currentDEC = -90;
    else
        currentDEC = 90;

    DBG_SCOPE = INDI::Logger::getInstance().addDebugLevel("Scope Verbose", "SCOPE");

    SetTelescopeCapability(TELESCOPE_CAN_PARK | TELESCOPE_CAN_SYNC | TELESCOPE_CAN_GOTO | TELESCOPE_CAN_ABORT |
                           TELESCOPE_HAS_TRACK_MODE | TELESCOPE_CAN_CONTROL_TRACK | TELESCOPE_HAS_TRACK_RATE |
                           TELESCOPE_HAS_LOCATION | TELESCOPE_CAN_HOME_FIND | TELESCOPE_CAN_HOME_GO,
                           9);

    setVersion(PMC8_VERSION_MAJOR, PMC8_VERSION_MINOR);
}

PMC8::~PMC8()
{
}

const char *PMC8::getDefaultName()
{
    return "PMC8";
}

bool PMC8::initProperties()
{
    INDI::Telescope::initProperties();

    // Serial Cable Connection Type
    // Letting them choose standard cable can speed up connection time significantly
    IUFillSwitch(&SerialCableTypeS[0], "SERIAL_CABLE_AUTO", "Auto", ISS_ON);
    IUFillSwitch(&SerialCableTypeS[1], "SERIAL_CABLE_INVERTED", "Inverted", ISS_OFF);
    IUFillSwitch(&SerialCableTypeS[2], "SERIAL_CABLE_STANDARD", "Standard", ISS_OFF);
    IUFillSwitchVector(&SerialCableTypeSP, SerialCableTypeS, 3, getDeviceName(), "SERIAL_CABLE_TYPE", "Serial Cable",
                       CONNECTION_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    // Mount Type
    IUFillSwitch(&MountTypeS[MOUNT_G11], "MOUNT_G11", "G11", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_TITAN], "MOUNT_TITAN", "Titan", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_EXOS2], "MOUNT_EXOS2", "EXOS2", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_iEXOS100], "MOUNT_iEXOS100", "iEXOS100", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_iEXOS200], "MOUNT_iEXOS200", "iEXOS200", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_iEXOS300], "MOUNT_iEXOS300", "iEXOS300", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_MSROEQ], "MOUNT_MSROEQ", "MSROEQ", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_ASKO], "MOUNT_ASKO", "ASKO SX260S", ISS_OFF);
    IUFillSwitchVector(&MountTypeSP, MountTypeS, MOUNT_COUNT, getDeviceName(), "MOUNT_TYPE", "Mount Type", CONNECTION_TAB, IP_RW,
                       ISR_1OFMANY, 0, IPS_IDLE);


    /* Tracking Mode */
    // order is important, since driver assumes solar = 1, lunar = 2
    AddTrackMode("TRACK_SIDEREAL", "Sidereal", true);
    AddTrackMode("TRACK_SOLAR", "Solar");
    AddTrackMode("TRACK_LUNAR", "Lunar");
    //AddTrackMode("TRACK_KING", "King"); // King appears to be effectively the same as Solar, at least for EXOS-2, and a bit of pain to implement with auto-detection
    AddTrackMode("TRACK_CUSTOM", "Custom");

    // Set TrackRate limits
    /*TrackRateN[AXIS_RA].min = -PMC8_MAX_TRACK_RATE;
    TrackRateN[AXIS_RA].max = PMC8_MAX_TRACK_RATE;
    TrackRateN[AXIS_DE].min = -0.01;
    TrackRateN[AXIS_DE].max = 0.01;*/

    // what to do after goto operation
    IUFillSwitch(&PostGotoS[0], "GOTO_START_TRACKING", "Start / Resume Tracking", ISS_ON);
    IUFillSwitch(&PostGotoS[1], "GOTO_RESUME_PREVIOUS", "Previous State", ISS_OFF);
    IUFillSwitch(&PostGotoS[2], "GOTO_STOP_TRACKING", "No Tracking", ISS_OFF);
    IUFillSwitchVector(&PostGotoSP, PostGotoS, 3, getDeviceName(), "POST_GOTO_SETTINGS", "Post Goto", MOTION_TAB, IP_RW,
                       ISR_1OFMANY, 0, IPS_IDLE);

    IUFillSwitch(&SlewCompensationS[0], "ASCOM_SLEW_COMPENSATION_ON", "On", ISS_ON);
    IUFillSwitch(&SlewCompensationS[1], "ASCOM_SLEW_COMPENSATION_OFF", "Off", ISS_OFF);
    IUFillSwitchVector(&SlewCompensationSP, SlewCompensationS, 2, getDeviceName(), "ASCOM_SLEW_COMPENSATION",
                       "ASCOM Slew Compensation", MOTION_TAB, IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    // relabel move speeds
    SlewRateSP[0].setLabel("4x");
    SlewRateSP[1].setLabel("8x");
    SlewRateSP[2].setLabel("16x");
    SlewRateSP[3].setLabel("32x");
    SlewRateSP[4].setLabel("64x");
    SlewRateSP[5].setLabel("128x");
    SlewRateSP[6].setLabel("256x");
    SlewRateSP[7].setLabel("512x");
    SlewRateSP[8].setLabel("Max");

    // settings for ramping up/down when moving
    IUFillNumber(&RampN[0], "RAMP_INTERVAL", "Interval (ms)", "%g", 20, 1000, 5, 200);
    IUFillNumber(&RampN[1], "RAMP_BASESTEP", "Base Step", "%g", 1, 256, 1, 4);
    IUFillNumber(&RampN[2], "RAMP_FACTOR", "Factor", "%g", 1.0, 2.0, 0.1, 1.4);
    IUFillNumberVector(&RampNP, RampN, 3, getDeviceName(), "RAMP_SETTINGS", "Move Ramp", MOTION_TAB, IP_RW, 0, IPS_IDLE);

    /* How fast do we guide compared to sidereal rate */
    IUFillNumber(&GuideRateN[0], "GUIDE_RATE_RA", "RA (x Sidereal)", "%g", 0.1, 1.0, 0.1, 0.4);
    IUFillNumber(&GuideRateN[1], "GUIDE_RATE_DE", "DEC (x Sidereal)", "%g", 0.1, 1.0, 0.1, 0.4);
    IUFillNumberVector(&GuideRateNP, GuideRateN, 2, getDeviceName(), "GUIDE_RATE", "Guide Rate", GUIDE_TAB, IP_RW, 0, IPS_IDLE);
    IUFillNumber(&LegacyGuideRateN[0], "LEGACY_GUIDE_RATE", "x Sidereal", "%g", 0.1, 1.0, 0.1, 0.4);
    IUFillNumberVector(&LegacyGuideRateNP, LegacyGuideRateN, 1, getDeviceName(), "LEGACY_GUIDE_RATE", "Guide Rate", GUIDE_TAB,
                       IP_RW, 0, IPS_IDLE);

    GI::initProperties(GUIDE_TAB);

    TrackState = SCOPE_IDLE;

    // Park is stored as PMC-Eight motor/encoder counts. Home remains fixed at
    // motor position (0,0), while Park may be adjusted for observatory clearance.
    SetParkDataType(PARK_RA_DEC_ENCODER);

    addAuxControls();

    set_pmc8_device(getDeviceName());

    IUFillText(&FirmwareT[0], "Version", "Version", "");
    IUFillTextVector(&FirmwareTP, FirmwareT, 1, getDeviceName(), "Firmware", "Firmware", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

    setDriverInterface(getDriverInterface() | GUIDER_INTERFACE);

    return true;
}

bool PMC8::updateProperties()
{
    INDI::Telescope::updateProperties();

    if (isConnected())
    {
        getStartupData();

        defineProperty(&PostGotoSP);
        loadConfig(true, PostGotoSP.name);

        defineProperty(&SlewCompensationSP);
        loadConfig(true, SlewCompensationSP.name);
        set_pmc8_ascom_slew_compensation(IUFindOnSwitchIndex(&SlewCompensationSP) == 0);

        defineProperty(&RampNP);
        loadConfig(true, RampNP.name);

        if (firmwareInfo.IsRev2Compliant)
        {
            defineProperty(&GuideRateNP);
        }
        else
        {
            defineProperty(&LegacyGuideRateNP);
        }

        defineProperty(&FirmwareTP);

        // do not support park position
        deleteProperty(ParkPositionNP);
        deleteProperty(ParkOptionSP);
    }
    else
    {
        deleteProperty(PostGotoSP.name);
        deleteProperty(SlewCompensationSP.name);

        if (firmwareInfo.IsRev2Compliant)
        {
            deleteProperty(GuideRateNP.name);
        }
        else
        {
            deleteProperty(LegacyGuideRateNP.name);
        }

        deleteProperty(FirmwareTP.name);

        deleteProperty(RampNP.name);
    }

    GI::updateProperties();

    return true;
}

void PMC8::getStartupData()
{
    LOG_DEBUG("Getting firmware data...");
    if (get_pmc8_firmware(PortFD, &firmwareInfo))
    {
        const char *c;

        FirmwareTP.s = IPS_OK;
        c = firmwareInfo.MainBoardFirmware.c_str();
        LOGF_INFO("firmware = %s.", c);

        // not sure if there's really a point to the mount switch anymore if we know the mount from the firmware - perhaps remove as newer firmware becomes standard?
        // populate mount type switch in interface from firmware if possible
        if (firmwareInfo.MountType >= 0 && firmwareInfo.MountType < MOUNT_COUNT)
        {
            MountTypeS[firmwareInfo.MountType].s = ISS_ON;
            LOGF_INFO("Detected mount type as %s.", MountTypeS[firmwareInfo.MountType].label);
        }
        else
        {
            LOG_INFO("Cannot detect mount type--perhaps this is older firmware?");
            if (strstr(getDeviceName(), "EXOS2"))
            {
                MountTypeS[MOUNT_EXOS2].s = ISS_ON;
                LOG_INFO("Guessing mount is EXOS2 from device name.");
            }
            else if (strstr(getDeviceName(), "iEXOS100"))
            {
                MountTypeS[MOUNT_iEXOS100].s = ISS_ON;
                LOG_INFO("Guessing mount is iEXOS100 from device name.");
            }
            else
            {
                MountTypeS[MOUNT_G11].s = ISS_ON;
                LOG_INFO("Guessing mount is G11.");
            }
        }
        MountTypeSP.s = IPS_OK;
        IDSetSwitch(&MountTypeSP, nullptr);

        IUSaveText(&FirmwareT[0], c);
        IDSetText(&FirmwareTP, nullptr);
    }

    // get SRF values
    if (firmwareInfo.IsRev2Compliant)
    {
        double rate = 0.4;
        if (get_pmc8_guide_rate(PortFD, PMC8_AXIS_RA, rate))
        {
            GuideRateN[0].value = rate;
            GuideRateNP.s = IPS_OK;
            IDSetNumber(&GuideRateNP, nullptr);
        }
        if (get_pmc8_guide_rate(PortFD, PMC8_AXIS_DEC, rate))
        {
            GuideRateN[1].value = rate;
            GuideRateNP.s = IPS_OK;
            IDSetNumber(&GuideRateNP, nullptr);
        }
    }

    // PMC8 doesn't store location permanently so read from config and set
    // Convert to INDI standard longitude (0 to 360 Eastward)
    double longitude = LocationNP[LOCATION_LONGITUDE].getValue();
    double latitude = LocationNP[LOCATION_LATITUDE].getValue();
    if (latitude < 0)
        currentDEC = -90;
    else
        currentDEC = 90;


    // must also keep "low level" aware of position to convert motor counts to RA/DEC
    set_pmc8_location(latitude, longitude);

    // seems like best place to put a warning that will be seen in log window of EKOS/etc
    LOG_INFO("The PMC-Eight driver is in BETA development currently.");
    LOG_INFO("Be prepared to intervene if something unexpected occurs.");

    if (InitPark())
    {
        // If loading parking data is successful, we just set the default parking values.
        SetAxis1ParkDefault(0);
        SetAxis2ParkDefault(0);
    }
    else
    {
        // Otherwise, we set all parking data to default in case no parking data is found.
        SetAxis1Park(0);
        SetAxis2Park(0);
        SetAxis1ParkDefault(0);
        SetAxis2ParkDefault(0);
    }

#if 0
    // FIXME - Need to implement simulation functionality
    if (isSimulation())
    {
        if (isParked())
            set_sim_system_status(ST_PARKED);
        else
            set_sim_system_status(ST_STOPPED);
    }
#endif
}

bool PMC8::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    // Check guider interface
    if (GI::processNumber(dev, name, values, names, n))
        return true;

    if (!strcmp(dev, getDeviceName()))
    {
        // Guiding Rate
        if (!strcmp(name, RampNP.name))
        {
            IUUpdateNumber(&RampNP, values, names, n);
            RampNP.s = IPS_OK;
            IDSetNumber(&RampNP, nullptr);

            return true;
        }
        if (!strcmp(name, LegacyGuideRateNP.name))
        {
            IUUpdateNumber(&GuideRateNP, values, names, n);

            if (set_pmc8_guide_rate(PortFD, PMC8_AXIS_RA, LegacyGuideRateN[0].value))
                LegacyGuideRateNP.s = IPS_OK;
            else
                LegacyGuideRateNP.s = IPS_ALERT;

            IDSetNumber(&LegacyGuideRateNP, nullptr);

            return true;
        }
        if (!strcmp(name, GuideRateNP.name))
        {
            IUUpdateNumber(&GuideRateNP, values, names, n);

            if (set_pmc8_guide_rate(PortFD, PMC8_AXIS_RA, GuideRateN[0].value) &&
                    set_pmc8_guide_rate(PortFD, PMC8_AXIS_DEC, GuideRateN[1].value))
                GuideRateNP.s = IPS_OK;
            else
                GuideRateNP.s = IPS_ALERT;

            IDSetNumber(&GuideRateNP, nullptr);

            return true;
        }
    }

    return INDI::Telescope::ISNewNumber(dev, name, values, names, n);
}

void PMC8::ISGetProperties(const char *dev)
{
    INDI::Telescope::ISGetProperties(dev);
    defineProperty(&MountTypeSP);
    defineProperty(&SerialCableTypeSP);
    loadConfig(true, SerialCableTypeSP.name);

    // set default connection parameters
    // unfortunately, the only way I've found to set these is after calling ISGetProperties on base class
    serialConnection->setDefaultBaudRate(Connection::Serial::B_115200);
    tcpConnection->setDefaultHost(PMC8_DEFAULT_IP_ADDRESS);
    tcpConnection->setDefaultPort(PMC8_DEFAULT_PORT);

    // reload config here, even though it was already loaded in call to base class
    // since defaults may have overridden saved properties
    loadConfig(false, nullptr);
}

bool PMC8::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        if (strcmp(name, MountTypeSP.name) == 0)
        {
            IUUpdateSwitch(&MountTypeSP, states, names, n);
            int currentMountIndex = IUFindOnSwitchIndex(&MountTypeSP);
            LOGF_INFO("Selected mount is %s", MountTypeS[currentMountIndex].label);

            //right now, this lets the user override the parameters for the detected mount.  Perhaps we should prevent the user from doing so?
            set_pmc8_mountParameters(currentMountIndex);
            MountTypeSP.s = IPS_OK;
            IDSetSwitch(&MountTypeSP, nullptr);
            return true;
        }
        if (strcmp(name, SerialCableTypeSP.name) == 0)
        {
            IUUpdateSwitch(&SerialCableTypeSP, states, names, n);
            SerialCableTypeSP.s = IPS_OK;
            IDSetSwitch(&SerialCableTypeSP, nullptr);
            return true;
        }
        if (strcmp(name, PostGotoSP.name) == 0)
        {
            IUUpdateSwitch(&PostGotoSP, states, names, n);
            // for v2 firmware, if halt after goto is selected, tell driver to use ESPt2
            set_pmc8_goto_resume(!((IUFindOnSwitchIndex(&PostGotoSP) == 2) && firmwareInfo.IsRev2Compliant));
            PostGotoSP.s = IPS_OK;
            IDSetSwitch(&PostGotoSP, nullptr);
            return true;
        }
        if (strcmp(name, SlewCompensationSP.name) == 0)
        {
            IUUpdateSwitch(&SlewCompensationSP, states, names, n);
            set_pmc8_ascom_slew_compensation(IUFindOnSwitchIndex(&SlewCompensationSP) == 0);
            SlewCompensationSP.s = IPS_OK;
            IDSetSwitch(&SlewCompensationSP, nullptr);
            return true;
        }
    }

    return INDI::Telescope::ISNewSwitch(dev, name, states, names, n);
}

bool PMC8::ReadScopeStatus()
{
    bool rc = false;

    // try to disconnect and reconnect if reconnect flag is set

    if (get_pmc8_reconnect_flag())
    {
        int rc = Disconnect();
        if (rc) setConnected(false);
        rc = Connect();
        if (rc) setConnected(true, IPS_OK);
        return false;
    }

    if (isSimulation())
        mountSim();

    // avoid unnecessary status calls to mount while pulse guiding so we don't lock up the mount for 40+ ms right when it needs to start/stop
    if (isPulsingNS || isPulsingWE) return true;

    bool slewing = false;

    switch (TrackState)
    {
        case SCOPE_SLEWING:
            // are we done?
            // check slew state
            rc = get_pmc8_is_scope_slewing(PortFD, slewing);
            if (!rc)
            {
                LOG_ERROR("PMC8::ReadScopeStatus() - unable to check slew state");
            }
            else
            {
                if (slewing == false)
                {
                    if (HomeSP.getState() == IPS_BUSY)
                    {
                        int actualRA = 0;
                        int actualDEC = 0;
                        bool atHomePosition = false;

                        rc = get_pmc8_is_at_motor_position(PortFD, 0, 0, PMC8_HOME_POSITION_TOLERANCE_COUNTS,
                                                           actualRA, actualDEC, atHomePosition);
                        if (!rc)
                        {
                            HomeSP.reset();
                            HomeSP.setState(IPS_ALERT);
                            HomeSP.apply();
                            LOG_ERROR("Unable to verify home position after home slew stopped.");
                            break;
                        }

                        HomeSP.reset();
                        if (atHomePosition)
                        {
                            if (stop_pmc8_tracking_motion(PortFD))
                                LOG_DEBUG("Mount tracking is off.");

                            TrackState = SCOPE_IDLE;
                            EqNP.setState(IPS_IDLE);
                            HomeSP.setState(IPS_OK);
                            LOGF_INFO("Home position reached at motor position RA=%d DEC=%d.", actualRA, actualDEC);
                        }
                        else
                        {
                            HomeSP.setState(IPS_ALERT);
                            LOGF_WARN("Home motion stopped before reaching home target: RA=%d DEC=%d. Mount is not home.",
                                      actualRA, actualDEC);

                            if (SetTrackEnabled(true))
                                TrackState = SCOPE_TRACKING;
                            else
                                TrackState = SCOPE_IDLE;
                        }

                        HomeSP.apply();
                        break;
                    }

                    if (ascomCorrectionSettlePolls > 0)
                    {
                        ascomCorrectionSettlePolls--;
                        LOG_DEBUG("Waiting for ASCOM correction slew motion to settle before completing goto.");
                        break;
                    }

                    if (ascomCorrectionPending)
                    {
                        int raError = 0, decError = 0;
                        int raActual = 0, decActual = 0;
                        int raTarget = 0, decTarget = 0;
                        ascomCorrectionPending = false;

                        rc = get_pmc8_slew_target_error(PortFD, targetRA, targetDEC, raError, decError,
                                                        raActual, decActual, raTarget, decTarget);
                        if (!rc)
                        {
                            LOG_ERROR("Unable to evaluate ASCOM correction slew error.");
                            break;
                        }

                        LOGF_DEBUG("ASCOM correction check: RA actual=%d target=%d error=%d, DEC actual=%d target=%d error=%d",
                                   raActual, raTarget, raError, decActual, decTarget, decError);

                        if ((raError > PMC8_ASCOM_CORRECTION_THRESHOLD_COUNTS) ||
                                (decError > PMC8_ASCOM_CORRECTION_THRESHOLD_COUNTS))
                        {
                            const int postGotoMode = IUFindOnSwitchIndex(&PostGotoSP);
                            const bool compensateRASlew = (postGotoMode == 0) ||
                                                          ((postGotoMode == 1) && (RememberTrackState == SCOPE_TRACKING));

                            LOGF_INFO("ASCOM correction slew needed: RA error=%d counts, DEC error=%d counts.", raError, decError);
                            if (!slew_pmc8(PortFD, targetRA, targetDEC, compensateRASlew))
                            {
                                LOG_ERROR("ASCOM correction slew failed.");
                                break;
                            }

                            ascomCorrectionSettlePolls = PMC8_ASCOM_CORRECTION_SETTLE_POLLS;
                            LOG_INFO("ASCOM correction slew started.");
                            break;
                        }

                        LOGF_DEBUG("ASCOM correction slew not needed: RA error=%d counts, DEC error=%d counts.", raError, decError);
                    }

                    if ((IUFindOnSwitchIndex(&PostGotoSP) == 0) ||
                            ((IUFindOnSwitchIndex(&PostGotoSP) == 1) && (RememberTrackState == SCOPE_TRACKING)))
                    {
                        LOG_INFO("Slew complete, tracking...");
                        TrackState = SCOPE_TRACKING;
                        TrackStateSP.setState(IPS_IDLE);

                        // Don't want to restart tracking after goto with v2 firmware, since mount does automatically
                        // and we might detect that slewing has stopped before it fully settles
                        if (!firmwareInfo.IsRev2Compliant)
                        {
                            if (!SetTrackEnabled(true))
                            {
                                LOG_ERROR("slew complete - unable to enable tracking");
                                return false;
                            }
                        }
                    }
                    else
                    {
                        LOG_INFO("Slew complete.");
                        TrackState = RememberTrackState;
                    }
                }
            }

            break;

        case SCOPE_PARKING:
            // are we done?
            // check slew state
            rc = get_pmc8_is_scope_slewing(PortFD, slewing);
            if (!rc)
            {
                LOG_ERROR("PMC8::ReadScopeStatus() - unable to check slew state");
            }
            else
            {
                if (slewing == false)
                {
                    int actualRA = 0;
                    int actualDEC = 0;
                    bool atParkPosition = false;

                    rc = get_pmc8_is_at_motor_position(PortFD, parkTargetRA, parkTargetDEC, PMC8_PARK_POSITION_TOLERANCE_COUNTS,
                                                       actualRA, actualDEC, atParkPosition);
                    if (!rc)
                    {
                        LOG_ERROR("Unable to verify park position after park slew stopped.");
                        break;
                    }

                    if (atParkPosition)
                    {
                        if (stop_pmc8_tracking_motion(PortFD))
                            LOG_DEBUG("Mount tracking is off.");

                        SetParked(true);
                        saveConfig(true);
                        LOGF_INFO("Mount parked at motor position RA=%d DEC=%d.", actualRA, actualDEC);
                    }
                    else
                    {
                        LOGF_WARN("Park motion stopped before reaching park target: RA=%d DEC=%d. Mount is not parked.",
                                  actualRA, actualDEC);
                        SetParked(false);

                        if (SetTrackEnabled(true))
                            TrackState = SCOPE_TRACKING;
                        else
                            TrackState = SCOPE_IDLE;
                    }
                }
            }
            break;

        case SCOPE_IDLE:
            //periodically check to see if we've entered tracking state (e.g. at startup or from other client)
            if (!trackingPollCounter--)
            {

                trackingPollCounter = PMC8_TRACKING_AUTODETECT_INTERVAL;

                // make sure we aren't moving manually to avoid false positives
                if (moveInfoDEC.state == PMC8_MOVE_INACTIVE && moveInfoRA.state == PMC8_MOVE_INACTIVE)
                {

                    double track_rate;
                    uint8_t track_mode;

                    rc = get_pmc8_tracking_data(PortFD, track_rate, track_mode);

                    // N.B. PMC8 rates are arcseconds per sidereal second
                    // INDI uses arcseconds per solar second
                    track_rate *= SOLAR_SECOND;

                    if (rc && ((int)track_rate > 0) && ((int)track_rate <= PMC8_MAX_TRACK_RATE))
                    {
                        TrackModeSP.reset();
                        TrackModeSP[convertFromPMC8TrackMode(track_mode)].setState(ISS_ON);
                        TrackModeSP.setState(IPS_OK);
                        TrackModeSP.apply();
                        TrackState = SCOPE_TRACKING;
                        LOGF_INFO("Mount has started tracking at %f arcsec / sec", track_rate);
                        TrackRateNP.setState(IPS_IDLE);
                        TrackRateNP[AXIS_RA].setValue(track_rate);
                        TrackRateNP.apply();
                    }
                }
            }
            break;

        case SCOPE_TRACKING:
            //periodically check to see if we've stopped tracking or changed speed (e.g. from other client)
            if (!trackingPollCounter--)
            {
                trackingPollCounter = PMC8_TRACKING_AUTODETECT_INTERVAL;

                // make sure we aren't moving manually to avoid false positives
                if (moveInfoDEC.state == PMC8_MOVE_INACTIVE && moveInfoRA.state == PMC8_MOVE_INACTIVE)
                {

                    double track_rate;
                    uint8_t track_mode;

                    rc = get_pmc8_tracking_data(PortFD, track_rate, track_mode);

                    // N.B. PMC8 rates are arcseconds per sidereal second
                    // INDI uses arcseconds per solar second
                    track_rate *= SOLAR_SECOND;

                    if (rc && ((int)track_rate == 0))
                    {
                        LOG_INFO("Mount appears to have stopped tracking");
                        TrackState = SCOPE_IDLE;
                    }
                    else if (rc && ((int)track_rate <= PMC8_MAX_TRACK_RATE))
                    {
                        if (TrackModeSP[convertFromPMC8TrackMode(track_mode)].getState() != ISS_ON)
                        {
                            TrackModeSP.reset();
                            TrackModeSP[convertFromPMC8TrackMode(track_mode)].setState(ISS_ON);
                            TrackModeSP.apply();
                        }
                        if (TrackRateNP[AXIS_RA].getValue() != track_rate)
                        {
                            TrackState = SCOPE_TRACKING;
                            TrackRateNP.setState(IPS_IDLE);
                            TrackRateNP[AXIS_RA].setValue(track_rate);
                            TrackRateNP.apply();
                            LOGF_INFO("Mount now tracking at %f arcsec / sec", track_rate);
                        }
                    }
                }
            }

        default:
            break;
    }

    rc = get_pmc8_coords(PortFD, currentRA, currentDEC);

    if (rc)
        NewRaDec(currentRA, currentDEC);

    return rc;
}

bool PMC8::Goto(double r, double d)
{
    if (isPulsingNS ||
            isPulsingWE ||
            moveInfoDEC.state != PMC8_MOVE_INACTIVE ||
            moveInfoRA.state != PMC8_MOVE_INACTIVE ||
            (TrackState == SCOPE_SLEWING && !firmwareInfo.IsRev2Compliant))
    {
        LOG_ERROR("Cannot slew while moving or guiding.  Please stop moving or guiding first");
        return false;
    }
    else if (TrackState == SCOPE_SLEWING)
    {
        targetRA  = r;
        targetDEC = d;
        ascomCorrectionPending = false;
        ascomCorrectionSettlePolls = 0;
        abort_pmc8_goto(PortFD);
        //Supposedly the goto should abort in 2s, but we'll give it a little bit more time just in case
        IEAddTimer(2500, AbortGotoTimeoutHelper, this);
        LOG_INFO("Goto called while already slewing.  Stopping slew and will try goto again in 2.5 seconds");
        return true;
    }

    // start tracking if we're idle, so mount will track at correct rate post-goto
    RememberTrackState = TrackState;
    if ((TrackState != SCOPE_TRACKING) && (IUFindOnSwitchIndex(&PostGotoSP) == 0) && firmwareInfo.IsRev2Compliant)
    {
        SetTrackEnabled(true);
    }
    else if (IUFindOnSwitchIndex(&PostGotoSP) == 2)
    {
        RememberTrackState = SCOPE_IDLE;
    }

    char RAStr[64] = {0}, DecStr[64] = {0};

    targetRA  = r;
    targetDEC = d;

    fs_sexa(RAStr, targetRA, 2, 3600);
    fs_sexa(DecStr, targetDEC, 2, 3600);

    LOGF_DEBUG("Slewing to RA: %s - DEC: %s", RAStr, DecStr);

    const int postGotoMode = IUFindOnSwitchIndex(&PostGotoSP);
    const bool compensateRASlew = (postGotoMode == 0) || ((postGotoMode == 1) && (TrackState == SCOPE_TRACKING));
    if (slew_pmc8(PortFD, r, d, compensateRASlew) == false)
    {
        LOG_ERROR("Failed to slew.");
        return false;
    }

    ascomCorrectionPending = (IUFindOnSwitchIndex(&SlewCompensationSP) == 0) && compensateRASlew;
    ascomCorrectionSettlePolls = 0;
    TrackState = SCOPE_SLEWING;

    return true;
}

bool PMC8::Sync(double ra, double dec)
{

    targetRA  = ra;
    targetDEC = dec;
    char RAStr[64] = {0}, DecStr[64] = {0};

    fs_sexa(RAStr, targetRA, 2, 3600);
    fs_sexa(DecStr, targetDEC, 2, 3600);

    LOGF_DEBUG("Syncing to RA: %s - DEC: %s", RAStr, DecStr);

    if (sync_pmc8(PortFD, ra, dec) == false)
    {
        LOG_ERROR("Failed to sync.");
    }

    EqNP.setState(IPS_OK);

    currentRA  = ra;
    currentDEC = dec;

    NewRaDec(currentRA, currentDEC);

    return true;
}

void PMC8::AbortGotoTimeoutHelper(void *p)
{
    //static_cast<PMC8*>(p)->TrackState = static_cast<PMC8*>(p)->RememberTrackState;
    static_cast<PMC8*>(p)->Goto(static_cast<PMC8*>(p)->targetRA, static_cast<PMC8*>(p)->targetDEC);
}

bool PMC8::Abort()
{
    //GUIDE Abort guide operations.
    if (GuideNSNP.getState() == IPS_BUSY || GuideWENP.getState() == IPS_BUSY)
    {
        GuideNSNP.setState(IPS_IDLE);
        GuideWENP.setState(IPS_IDLE);
        GuideNSNP[0].setValue(0);
        GuideNSNP[1].setValue(0);
        GuideWENP[0].setValue(0);
        GuideWENP[1].setValue(0);

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideNSTID = 0;
        }

        LOG_INFO("Guide aborted.");
        GuideNSNP.apply();
        GuideWENP.apply();
        return true;
    }


    //GOTO Abort slew operations.
    if (TrackState == SCOPE_SLEWING)
    {
        ascomCorrectionPending = false;
        ascomCorrectionSettlePolls = 0;
        abort_pmc8_goto(PortFD);
        //It will take about 2s to abort; we'll rely on ReadScopeStatus to detect when that occurs
        LOG_INFO("Goto aborted.");
        return true;
    }

    //MOVE Abort move operations.
    if ((moveInfoDEC.state == PMC8_MOVE_ACTIVE) || (moveInfoRA.state == PMC8_MOVE_ACTIVE))
    {
        if (moveInfoDEC.state == PMC8_MOVE_ACTIVE)
        {
            MoveNS((INDI_DIR_NS)moveInfoDEC.moveDir, MOTION_STOP);
        }
        if (moveInfoRA.state == PMC8_MOVE_ACTIVE)
        {
            MoveWE((INDI_DIR_WE)moveInfoRA.moveDir, MOTION_STOP);
        }
        LOG_INFO("Move aborted.");
        return true;
    }

    LOG_INFO("Abort called--stopping all motion.");
    if (abort_pmc8(PortFD))
    {
        TrackState = SCOPE_IDLE;
        return true;
    }
    else return false;
}

bool PMC8::Park()
{
    //if we're already parking, no need to do anything
    if (TrackState == SCOPE_PARKING)
    {
        return true;
    }

    parkTargetRA = axisParkToMotorCounts(GetAxis1Park());
    parkTargetDEC = axisParkToMotorCounts(GetAxis2Park());

    if (park_pmc8(PortFD, parkTargetRA, parkTargetDEC))
    {
        TrackState = SCOPE_PARKING;
        LOGF_INFO("Telescope parking in progress to motor position RA=%d DEC=%d.", parkTargetRA, parkTargetDEC);
        return true;
    }
    else
    {
        return false;
    }
}

bool PMC8::UnPark()
{
    if (unpark_pmc8(PortFD))
    {
        SetParked(false);
        TrackState = SCOPE_IDLE;
        return true;
    }
    else
    {
        return false;
    }
}

IPState PMC8::ExecuteHomeAction(TelescopeHomeAction action)
{
    switch (action)
    {
        case HOME_FIND:
        case HOME_GO:
        {
            if (TrackState == SCOPE_SLEWING || TrackState == SCOPE_PARKING)
            {
                LOG_WARN("Cannot home while the mount is already moving.");
                return IPS_ALERT;
            }

            int actualRA = 0;
            int actualDEC = 0;
            bool atHomePosition = false;

            if (get_pmc8_is_at_motor_position(PortFD, 0, 0, PMC8_HOME_POSITION_TOLERANCE_COUNTS,
                                              actualRA, actualDEC, atHomePosition) && atHomePosition)
            {
                if (stop_pmc8_tracking_motion(PortFD))
                    LOG_DEBUG("Mount tracking is off.");

                TrackState = SCOPE_IDLE;
                LOGF_INFO("Mount is already home at motor position RA=%d DEC=%d.", actualRA, actualDEC);
                return IPS_OK;
            }

            ascomCorrectionPending = false;
            ascomCorrectionSettlePolls = 0;

            if (!SetTrackEnabled(false))
            {
                LOG_ERROR("Unable to stop tracking before homing.");
                return IPS_ALERT;
            }

            if (!home_pmc8(PortFD))
            {
                LOG_ERROR("Unable to start home slew.");
                return IPS_ALERT;
            }

            TrackState = SCOPE_SLEWING;
            LOG_INFO("Slewing to PMC-Eight home motor position (0, 0).");
            return IPS_BUSY;
        }

        case HOME_SET:
            LOG_WARN("Set Home is not exposed for PMC-Eight because it would re-zero the motor counters.");
            return IPS_ALERT;

        default:
            return IPS_ALERT;
    }
}

bool PMC8::Handshake()
{
    if (isSimulation())
    {
        set_pmc8_sim_system_status(ST_STOPPED);
        set_pmc8_sim_track_rate(PMC8_TRACK_SIDEREAL);
        set_pmc8_sim_move_rate(64 * 15);
        //        set_pmc8_sim_hemisphere(HEMI_NORTH);
    }

    PMC8_CONNECTION_TYPE conn = PMC8_SERIAL_AUTO;
    if (getActiveConnection() == serialConnection)
    {
        if (IUFindOnSwitchIndex(&SerialCableTypeSP) == 1) conn = PMC8_SERIAL_INVERTED;
        if (IUFindOnSwitchIndex(&SerialCableTypeSP) == 2) conn = PMC8_SERIAL_STANDARD;
    }
    else
    {
        conn = PMC8_ETHERNET;
    }

    return check_pmc8_connection(PortFD, conn);
}

bool PMC8::updateTime(ln_date *utc, double utc_offset)
{
    // mark unused
    INDI_UNUSED(utc);
    INDI_UNUSED(utc_offset);

    LOG_ERROR("PMC8::updateTime() not implemented!");
    return false;

}

bool PMC8::updateLocation(double latitude, double longitude, double elevation)
{
    INDI_UNUSED(elevation);

    if (longitude > 180)
        longitude -= 360;

    // Southern Hemisphere support
    // As of 2024, southern hemisphere is now supported with proper coordinate
    // transformations and motor direction handling via pmc8_east_dir variable.
    // The low-level driver (pmc8driver.cpp) handles the coordinate math.
    if (latitude < 0)
    {
        LOG_INFO("Southern Hemisphere detected - using inverted coordinate system.");
    }

    // must also keep "low level" aware of position to convert motor counts to RA/DEC
    set_pmc8_location(latitude, longitude);

    char l[32] = {0}, L[32] = {0};
    fs_sexa(l, latitude, 3, 3600);
    fs_sexa(L, longitude, 4, 3600);

    LOGF_INFO("Site location updated to Lat %.32s - Long %.32s", l, L);

    return true;
}

void PMC8::debugTriggered(bool enable)
{
    set_pmc8_debug(enable);
}

void PMC8::simulationTriggered(bool enable)
{
    set_pmc8_simulation(enable);
}

int PMC8::getSlewRate(PMC8_AXIS axis)
{
    int mode = SlewRateSP.findOnSwitchIndex();
    int maxAxisRate = static_cast<int>(std::lround(get_pmc8_axis_max_move_rate(axis)));

    if (mode >= 8)
        return maxAxisRate;

    int requestedRate = static_cast<int>(std::lround(4.0 * std::pow(2.0, mode) * 15.0));

    if (maxAxisRate <= 0)
        return requestedRate;

    return std::min(requestedRate, maxAxisRate);
}


bool PMC8::ramp_movement(PMC8_DIRECTION dir)
{

    PMC8MoveInfo *moveInfo = ((dir == PMC8_N) | (dir == PMC8_S)) ? &moveInfoDEC : &moveInfoRA;

    if (moveInfo->state != PMC8_MOVE_RAMPING)
    {
        return false; //shouldn't be here
        LOG_ERROR("Ramp function called while not in ramp state");
    }

    int newrate = moveInfo->rampLastStep;

    if (moveInfo->rampDir == PMC8_RAMP_UP)
    {
        newrate += RampN[1].value * pow(RampN[2].value, moveInfo->rampIteration++) * 15;
    }
    else
    {
        newrate -= RampN[1].value * pow(RampN[2].value, --moveInfo->rampIteration) * 15;
    }

    int adjrate = newrate;

    //check to see if we're done
    if (newrate >= moveInfo->targetRate)
    {
        adjrate = moveInfo->targetRate;
        moveInfo->state = PMC8_MOVE_ACTIVE;
    }
    else if (newrate <= 0)
    {
        adjrate = 0;
        moveInfo->state = PMC8_MOVE_INACTIVE;
        //restore tracking if we're at 0
        if ((dir == PMC8_E) || (dir == PMC8_W))
        {
            if (TrackState == SCOPE_TRACKING)
            {
                if (!SetTrackEnabled(true))
                {
                    LOG_ERROR("slew complete - unable to enable tracking");
                    return false;
                }
            }

            return true;
        }
    }

    //adjust for current tracking rate
    if (dir == PMC8_E) adjrate += round(TrackRateNP[AXIS_RA].getValue());
    else if (dir == PMC8_W) adjrate -= round(TrackRateNP[AXIS_RA].getValue());

    // Solar second to Sideral second conversion
    adjrate /= SOLAR_SECOND;

    LOGF_EXTRA3("Ramping: mount dir %d, ramping dir %d, iteration %d, step to %d", dir, moveInfo->rampDir,
                moveInfo->rampIteration, adjrate);

    if (!set_pmc8_move_rate_axis(PortFD, dir, adjrate))
    {
        LOGF_ERROR("Error ramping move rate: mount dir %d, ramping dir %d, iteration %d, step to %d", dir, moveInfo->rampDir,
                   moveInfo->rampIteration, adjrate);
        moveInfo->state = PMC8_MOVE_INACTIVE;
        return false;
    }

    moveInfo->rampLastStep = newrate;

    return true;
}

//MOVE The timer helper functions.
void PMC8::rampTimeoutHelperN(void *p)
{
    PMC8* pmc8 = static_cast<PMC8*>(p);
    if (pmc8->ramp_movement(PMC8_N) && (pmc8->moveInfoDEC.state == PMC8_MOVE_RAMPING))
        pmc8->moveInfoDEC.timer = IEAddTimer(pmc8->RampN[0].value, rampTimeoutHelperN, p);
}
void PMC8::rampTimeoutHelperS(void *p)
{
    PMC8* pmc8 = static_cast<PMC8*>(p);
    if (pmc8->ramp_movement(PMC8_S) && (pmc8->moveInfoDEC.state == PMC8_MOVE_RAMPING))
        pmc8->moveInfoDEC.timer = IEAddTimer(pmc8->RampN[0].value, rampTimeoutHelperS, p);
}
void PMC8::rampTimeoutHelperW(void *p)
{
    PMC8* pmc8 = static_cast<PMC8*>(p);
    if (pmc8->ramp_movement(PMC8_W) && (pmc8->moveInfoRA.state == PMC8_MOVE_RAMPING))
        pmc8->moveInfoRA.timer = IEAddTimer(pmc8->RampN[0].value, rampTimeoutHelperW, p);
}
void PMC8::rampTimeoutHelperE(void *p)
{
    PMC8* pmc8 = static_cast<PMC8*>(p);
    if (pmc8->ramp_movement(PMC8_E) && (pmc8->moveInfoRA.state == PMC8_MOVE_RAMPING))
        pmc8->moveInfoRA.timer = IEAddTimer(pmc8->RampN[0].value, rampTimeoutHelperE, p);
}


bool PMC8::MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command)
{
    if (TrackState == SCOPE_PARKED)
    {
        LOG_ERROR("Please unpark the mount before issuing any motion commands.");
        return false;
    }
    if (TrackState == SCOPE_SLEWING)
    {
        LOG_ERROR("Mount is slewing.  Wait to issue move command until goto completes.");
        return false;
    }
    if ((moveInfoDEC.state == PMC8_MOVE_ACTIVE) && (moveInfoDEC.moveDir != dir))
    {
        LOG_ERROR("Mount received command to move in opposite direction before stopping.  This shouldn't happen.");
        return false;
    }

    // read desired move rate
    int currentIndex = SlewRateSP.findOnSwitchIndex();
    LOGF_DEBUG("MoveNS at slew index %d", currentIndex);

    switch (command)
    {
        case MOTION_START:
            moveInfoDEC.rampDir = PMC8_RAMP_UP;
            moveInfoDEC.targetRate = getSlewRate(PMC8_AXIS_DEC);
            // if we're still ramping down, we can bypass resetting the state and adding a timer
            // but we do need to make sure it's the same direction first (if not, kill our previous timer)
            if (moveInfoDEC.state == PMC8_MOVE_RAMPING)
            {
                if (moveInfoDEC.moveDir == dir) return true;
                IERmTimer(moveInfoDEC.timer);
                LOG_WARN("Started moving other direction before ramp down completed.  This *may* cause mechanical problems with mount.  It is adviseable to wait for axis movement to settle before switching directions.");
            }
            moveInfoDEC.moveDir = dir;
            moveInfoDEC.state = PMC8_MOVE_RAMPING;
            moveInfoDEC.rampIteration = 0;
            moveInfoDEC.rampLastStep = 0;

            LOGF_INFO("Moving toward %s.", (dir == DIRECTION_NORTH) ? "North" : "South");

            break;

        case MOTION_STOP:
            // if we've already started moving other direction, no need to stop
            if (moveInfoDEC.moveDir != dir)
            {
                LOGF_DEBUG("Stop command issued for direction %d, but we're not moving that way", dir);
                return false;
            }

            moveInfoDEC.rampDir = PMC8_RAMP_DOWN;
            // if we're still ramping up, we can bypass adding a timer
            if (moveInfoDEC.state == PMC8_MOVE_RAMPING) return true;
            moveInfoDEC.state = PMC8_MOVE_RAMPING;

            LOGF_INFO("%s motion stopping.", (dir == DIRECTION_NORTH) ? "North" : "South");

            break;
    }

    if (dir == DIRECTION_NORTH)
        rampTimeoutHelperN(this);
    else
        rampTimeoutHelperS(this);

    return true;
}


bool PMC8::MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command)
{
    if (TrackState == SCOPE_PARKED)
    {
        LOG_ERROR("Please unpark the mount before issuing any motion commands.");
        return false;
    }
    if (TrackState == SCOPE_SLEWING)
    {
        LOG_ERROR("Mount is already slewing.  Wait to issue move command until done slewing.");
        return false;
    }
    if ((moveInfoRA.state == PMC8_MOVE_ACTIVE) && (moveInfoRA.moveDir != dir))
    {
        LOG_ERROR("Mount received command to move in opposite direction before stopping.  This shouldn't happen.");
        return false;
    }

    // read desired move rate
    int currentIndex = SlewRateSP.findOnSwitchIndex();
    LOGF_DEBUG("MoveWE at slew index %d", currentIndex);

    switch (command)
    {
        case MOTION_START:
            moveInfoRA.rampDir = PMC8_RAMP_UP;
            moveInfoRA.targetRate = getSlewRate(PMC8_AXIS_RA);
            // if we're still ramping down, we can bypass resetting the state and adding a timer
            // but we do need to make sure it's the same direction first (if not, kill our previous timer)
            if (moveInfoRA.state == PMC8_MOVE_RAMPING)
            {
                if (moveInfoRA.moveDir == dir) return true;
                IERmTimer(moveInfoRA.timer);
                LOG_WARN("Started moving other direction before ramp down completed.  This *may* cause mechanical problems with mount.  It is adviseable to wait for axis movement to settle before switching directions.");
            }
            moveInfoRA.moveDir = dir;
            moveInfoRA.state = PMC8_MOVE_RAMPING;
            moveInfoRA.rampIteration = 0;
            moveInfoRA.rampLastStep = 0;

            LOGF_INFO("Moving toward %s.", (dir == DIRECTION_WEST) ? "West" : "East");

            break;

        case MOTION_STOP:
            // if we've already started moving other direction, no need to stop
            if (moveInfoRA.moveDir != dir)
            {
                LOGF_DEBUG("Stop command issued for direction %d, but we're not moving that way", dir);
                return false;
            }

            moveInfoRA.rampDir = PMC8_RAMP_DOWN;
            // if we're still ramping up, we can bypass adding a timer
            if (moveInfoRA.state == PMC8_MOVE_RAMPING) return true;
            moveInfoRA.state = PMC8_MOVE_RAMPING;

            LOGF_INFO("%s motion stopping.", (dir == DIRECTION_WEST) ? "West" : "East");

            break;
    }

    if (dir == DIRECTION_EAST)
        rampTimeoutHelperE(this);
    else
        rampTimeoutHelperW(this);

    return true;
}

IPState PMC8::GuideNorth(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;

    //only guide if tracking
    if (TrackState == SCOPE_TRACKING)
    {

        // If already moving, then stop movement
        if (MovementNSSP.getState() == IPS_BUSY)
        {
            int dir = MovementNSSP.findOnSwitchIndex();
            MoveNS(dir == 0 ? DIRECTION_NORTH : DIRECTION_SOUTH, MOTION_STOP);
        }

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        isPulsingNS = true;
        start_pmc8_guide(PortFD, PMC8_N, (int)ms, timetaken_us, 0, destSideOfPier(currentRA, currentDEC));

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else
    {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideNSTID      = IEAddTimer(timeremain_ms, guideTimeoutHelperN, this);
    return ret;
}

IPState PMC8::GuideSouth(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;

    //only guide if tracking
    if (TrackState == SCOPE_TRACKING)
    {

        // If already moving, then stop movement
        if (MovementNSSP.getState() == IPS_BUSY)
        {
            int dir = MovementNSSP.findOnSwitchIndex();
            MoveNS(dir == 0 ? DIRECTION_NORTH : DIRECTION_SOUTH, MOTION_STOP);
        }

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        isPulsingNS = true;
        start_pmc8_guide(PortFD, PMC8_S, (int)ms, timetaken_us, 0, destSideOfPier(currentRA, currentDEC));

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else
    {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideNSTID      = IEAddTimer(timeremain_ms, guideTimeoutHelperS, this);
    return ret;
}

IPState PMC8::GuideEast(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;

    //only guide if tracking
    if (TrackState == SCOPE_TRACKING)
    {

        // If already moving (no pulse command), then stop movement
        if (MovementWESP.getState() == IPS_BUSY)
        {
            int dir = MovementWESP.findOnSwitchIndex();
            MoveWE(dir == 0 ? DIRECTION_WEST : DIRECTION_EAST, MOTION_STOP);
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideWETID = 0;
        }

        isPulsingWE = true;

        start_pmc8_guide(PortFD, PMC8_E, (int)ms, timetaken_us, TrackRateNP[AXIS_RA].getValue() / SOLAR_SECOND,
                         destSideOfPier(currentRA, currentDEC));

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else
    {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideWETID      = IEAddTimer(timeremain_ms, guideTimeoutHelperE, this);
    return ret;
}

IPState PMC8::GuideWest(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;

    //only guide if tracking
    if (TrackState == SCOPE_TRACKING)
    {

        // If already moving (no pulse command), then stop movement
        if (MovementWESP.getState() == IPS_BUSY)
        {
            int dir = MovementWESP.findOnSwitchIndex();
            MoveWE(dir == 0 ? DIRECTION_WEST : DIRECTION_EAST, MOTION_STOP);
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideWETID = 0;
        }

        isPulsingWE = true;
        start_pmc8_guide(PortFD, PMC8_W, (int)ms, timetaken_us, TrackRateNP[AXIS_RA].getValue() / SOLAR_SECOND,
                         destSideOfPier(currentRA, currentDEC));

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else
    {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideWETID      = IEAddTimer(timeremain_ms, guideTimeoutHelperW, this);
    return ret;
}

void PMC8::guideTimeout(PMC8_DIRECTION calldir)
{
    // end previous pulse command
    stop_pmc8_guide(PortFD, calldir);

    if (calldir == PMC8_N || calldir == PMC8_S)
    {
        isPulsingNS = false;
        GuideNSNP[0].setValue(0);
        GuideNSNP[1].setValue(0);
        GuideNSNP.setState(IPS_IDLE);
        GuideNSTID            = 0;
        GuideNSNP.apply();
    }
    if (calldir == PMC8_W || calldir == PMC8_E)
    {
        isPulsingWE = false;
        GuideWENP[0].setValue(0);
        GuideWENP[1].setValue(0);
        GuideWENP.setState(IPS_IDLE);
        GuideWETID            = 0;
        GuideWENP.apply();
    }

    LOG_DEBUG("GUIDE CMD COMPLETED");
}

//GUIDE The timer helper functions.
void PMC8::guideTimeoutHelperN(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_N);
}
void PMC8::guideTimeoutHelperS(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_S);
}
void PMC8::guideTimeoutHelperW(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_W);
}
void PMC8::guideTimeoutHelperE(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_E);
}

bool PMC8::SetSlewRate(int index)
{

    INDI_UNUSED(index);

    // slew rate is rate for MoveEW/MOVENE commands - not for GOTOs!!!

    // just return true - we will check SlewRateSP when we do actually moves
    return true;
}

bool PMC8::saveConfigItems(FILE *fp)
{
    INDI::Telescope::saveConfigItems(fp);

    IUSaveConfigSwitch(fp, &SerialCableTypeSP);
    IUSaveConfigSwitch(fp, &MountTypeSP);
    IUSaveConfigNumber(fp, &RampNP);
    IUSaveConfigNumber(fp, &LegacyGuideRateNP);
    IUSaveConfigSwitch(fp, &PostGotoSP);
    IUSaveConfigSwitch(fp, &SlewCompensationSP);

    return true;
}

void PMC8::mountSim()
{
    static struct timeval ltv;
    struct timeval tv;
    double dt, da, dx;
    int nlocked;

    /* update elapsed time since last poll, don't presume exactly POLLMS */
    gettimeofday(&tv, nullptr);

    if (ltv.tv_sec == 0 && ltv.tv_usec == 0)
        ltv = tv;

    dt  = tv.tv_sec - ltv.tv_sec + (tv.tv_usec - ltv.tv_usec) / 1e6;
    ltv = tv;
    da  = SLEWRATE * dt;

    /* Process per current state. We check the state of EQUATORIAL_COORDS and act accordingly */
    switch (TrackState)
    {
        case SCOPE_IDLE:
            currentRA += (TrackRateNP[AXIS_RA].getValue() / 3600.0 * dt) / 15.0;
            currentRA = range24(currentRA);
            break;

        case SCOPE_TRACKING:
            if (TrackModeSP[1].getState() == ISS_ON)
            {
                currentRA  += ( ((TRACKRATE_SIDEREAL / 3600.0) - (TrackRateNP[AXIS_RA].getValue() / 3600.0)) * dt) / 15.0;
                currentDEC += ( (TrackRateNP[AXIS_DE].getValue() / 3600.0) * dt);
            }
            break;

        case SCOPE_SLEWING:
        case SCOPE_PARKING:
            /* slewing - nail it when both within one pulse @ SLEWRATE */
            nlocked = 0;

            dx = targetRA - currentRA;

            // Take shortest path
            if (fabs(dx) > 12)
                dx *= -1;

            if (fabs(dx) <= da)
            {
                currentRA = targetRA;
                nlocked++;
            }
            else if (dx > 0)
                currentRA += da / 15.;
            else
                currentRA -= da / 15.;

            if (currentRA < 0)
                currentRA += 24;
            else if (currentRA > 24)
                currentRA -= 24;

            dx = targetDEC - currentDEC;
            if (fabs(dx) <= da)
            {
                currentDEC = targetDEC;
                nlocked++;
            }
            else if (dx > 0)
                currentDEC += da;
            else
                currentDEC -= da;

            if (nlocked == 2)
            {
                if (TrackState == SCOPE_SLEWING)
                    set_pmc8_sim_system_status(ST_TRACKING);
                else
                    set_pmc8_sim_system_status(ST_PARKED);
            }

            break;

        case SCOPE_PARKED:
            // setting system status to parked will automatically
            // set the simulated RA/DEC to park position so reread
            set_pmc8_sim_system_status(ST_PARKED);
            get_pmc8_coords(PortFD, currentRA, currentDEC);

            break;

        default:
            break;
    }

    set_pmc8_sim_ra(currentRA);
    set_pmc8_sim_dec(currentDEC);
}

bool PMC8::SetParkPosition(double Axis1Value, double Axis2Value)
{
    if (Axis1Value < 0 || Axis1Value >= PMC8_AXIS_POSITION_WRAP ||
            Axis2Value < 0 || Axis2Value >= PMC8_AXIS_POSITION_WRAP)
    {
        LOG_WARN("Park encoder positions must be between 0 and 16777215.");
        return false;
    }

    return true;
}

bool PMC8::SetCurrentPark()
{
    int rapoint = 0;
    int decpoint = 0;

    if (!get_pmc8_position(PortFD, rapoint, decpoint))
    {
        LOG_ERROR("Unable to read current motor position for Set Current Park.");
        return false;
    }

    SetAxis1Park(motorCountsToAxisPark(rapoint));
    SetAxis2Park(motorCountsToAxisPark(decpoint));
    LOGF_INFO("Current park position set to motor position RA=%d DEC=%d.", rapoint, decpoint);

    return true;
}

bool PMC8::SetDefaultPark()
{
    // Default Park matches fixed Home at PMC-Eight motor position (0,0), but
    // users may set Park independently for roll-off-roof or clearance needs.
    SetAxis1Park(0);
    SetAxis2Park(0);

    return true;
}

uint8_t PMC8::convertToPMC8TrackMode(uint8_t mode)
{
    switch (mode)
    {
        case TRACK_SIDEREAL:
            return PMC8_TRACK_SIDEREAL;
            break;
        case TRACK_LUNAR:
            return PMC8_TRACK_LUNAR;
            break;
        case TRACK_SOLAR:
            return PMC8_TRACK_SOLAR;
            break;
        case TRACK_CUSTOM:
            return PMC8_TRACK_CUSTOM;
            break;
        default:
            return PMC8_TRACK_UNDEFINED;
    }
}

uint8_t PMC8::convertFromPMC8TrackMode(uint8_t mode)
{
    switch (mode)
    {
        case PMC8_TRACK_SIDEREAL:
            return TRACK_SIDEREAL;
            break;
        case PMC8_TRACK_LUNAR:
            return TRACK_LUNAR;
            break;
        case PMC8_TRACK_SOLAR:
            return TRACK_SOLAR;
            break;
        default:
            return TRACK_CUSTOM;
    }
}

bool PMC8::SetTrackMode(uint8_t mode)
{
    uint8_t pmc8_mode;

    LOGF_DEBUG("PMC8::SetTrackMode called mode=%d", mode);

    pmc8_mode = convertToPMC8TrackMode(mode);

    if (pmc8_mode == PMC8_TRACK_UNDEFINED)
    {
        LOGF_ERROR("PMC8::SetTrackMode mode=%d not supported!", mode);
        return false;
    }

    if (pmc8_mode == PMC8_TRACK_CUSTOM)
    {
        if (set_pmc8_ra_tracking(PortFD, TrackRateNP[AXIS_RA].getValue() / SOLAR_SECOND) &&
                set_pmc8_custom_dec_track_rate(PortFD, TrackRateNP[AXIS_DE].getValue() / SOLAR_SECOND,
                                               destSideOfPier(currentRA, currentDEC)))
        {
            return true;
        }
    }
    else
    {
        if (set_pmc8_track_mode(PortFD, pmc8_mode) &&
                set_pmc8_custom_dec_track_rate(PortFD, 0.0, destSideOfPier(currentRA, currentDEC)))
            return true;
    }

    return false;
}

bool PMC8::SetTrackRate(double raRate, double deRate)
{
    double pmc8RARate;
    double pmc8DERate;

    LOGF_INFO("Custom tracking rate set: raRate=%f  deRate=%f", raRate, deRate);

    pmc8RARate = raRate / SOLAR_SECOND;
    pmc8DERate = deRate / SOLAR_SECOND;

    if (set_pmc8_ra_tracking(PortFD, pmc8RARate) &&
            set_pmc8_custom_dec_track_rate(PortFD, pmc8DERate, destSideOfPier(currentRA, currentDEC)))
        return true;

    LOG_ERROR("PMC8::SetTrackRate failed");
    return false;
}

bool PMC8::SetTrackEnabled(bool enabled)
{

    LOGF_DEBUG("PMC8::SetTrackEnabled called enabled=%d", enabled);

    // need to determine current tracking mode and start tracking
    if (enabled)
    {
        if (!SetTrackMode(TrackModeSP.findOnSwitchIndex()))
        {
            LOG_ERROR("PMC8::SetTrackEnabled - unable to enable tracking");
            return false;
        }
    }
    else
    {
        bool rc;

        rc = set_pmc8_custom_ra_track_rate(PortFD, 0);
        if (!rc)
        {
            LOG_ERROR("PMC8::SetTrackEnabled - unable to set RA track rate to 0");
            return false;
        }

        rc = set_pmc8_custom_dec_track_rate(PortFD, 0, destSideOfPier(currentRA, currentDEC));
        if (!rc)
        {
            LOG_ERROR("PMC8::SetTrackEnabled - unable to set DEC track rate to 0");
            return false;
        }
    }

    return true;
}

