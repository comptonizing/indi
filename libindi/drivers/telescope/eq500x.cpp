#if 0
LX200-based EQ500X Equatorial Mount
Copyright (C) 2019 Eric Dejouhanet (eric.dejouhanet@gmail.com)
Low-level communication from elias.erdnuess@nimax.de

This library is free software;
you can redistribute it and / or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation;
either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY;
without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library;
if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301  USA
#endif

#include <cmath>
#include <memory>
#include <cstring>
#include <termios.h>
#include <unistd.h>
#include <cassert>

#include <libnova/sidereal_time.h>
#include <libnova/transform.h>

#include "lx200generic.h"
#include "eq500x.h"

#include "indicom.h"
#include "lx200driver.h"

typedef struct _simEQ500X
{
    char MechanicalRA[16];
    char MechanicalDEC[16];
    clock_t last_sim;
} simEQ500X_t;

simEQ500X_t simEQ500X_zero =
{
    "00:00:00",
    "+00*00'00",
    0,
};

simEQ500X_t simEQ500X = simEQ500X_zero;

#define MechanicalPoint_DEC_Format "+DD:MM:SS"
#define MechanicalPoint_RA_Format  "HH:MM:SS"

// This is the duration the serial port waits for while expecting replies
static int const EQ500X_TIMEOUT = 5;

// One degree, one arcminute, one arcsecond
static double constexpr ONEDEGREE = 1.0;
static double constexpr ARCMINUTE = ONEDEGREE/60.0;
static double constexpr ARCSECOND = ONEDEGREE/3600.0;

// This is the minimum detectable movement in RA/DEC
static double constexpr RA_GRANULARITY = std::lround((15.0*ARCSECOND)*3600.0)/3600.0;
static double constexpr DEC_GRANULARITY = std::lround((1.0*ARCSECOND)*3600.0)/3600.0;

// This is the number of loops expected to achieve convergence on each slew rate
// A full rotation at 5deg/s would take 360/5=72s to complete at RS speed, checking position twice per second
static int MAX_CONVERGENCE_LOOPS = 144;

// Hardcoded adjustment intervals
// RA/DEC deltas are adjusted at specific 'slew_rate' down to 'epsilon' degrees when smaller than 'distance' degrees
// The greater adjustment requirement drives the slew rate (one single command for both axis)
struct _adjustment {
    char const * slew_rate;
    double epsilon;
    double distance;
    int polling_interval;
}
const adjustments[] = {
{":RG#",   1*ARCSECOND,  0.7*ARCMINUTE, 100 },   // Guiding speed
{":RC#", 0.7*ARCMINUTE,   10*ARCMINUTE, 200 },   // Centering speed
{":RM#",  10*ARCMINUTE,    5*ONEDEGREE, 500 },   // Finding speed
{":RS#",   5*ONEDEGREE,   10*ONEDEGREE, 500 },   // Slew speed
{":RS#",  10*ONEDEGREE,  360*ONEDEGREE, 1000 }}; // Slew speed

/**************************************************************************************
** EQ500X Constructor
***************************************************************************************/
EQ500X::EQ500X(): LX200Generic()
{
    setVersion(1, 0);

    // Sanitize constants: epsilon of a slew rate must be smaller than distance of its smaller sibling
    for (size_t i = 0; i < sizeof(adjustments)/sizeof(adjustments[0])-1; i++)
        assert(adjustments[i+1].epsilon <= adjustments[i].distance);

    // Only pulse guiding, no tracking frequency
    setLX200Capability(LX200_HAS_PULSE_GUIDING);

    // Sync, goto, abort, location and 4 slew rates, no guiding rates and no park position
    SetTelescopeCapability(TELESCOPE_CAN_SYNC | TELESCOPE_CAN_GOTO | TELESCOPE_CAN_ABORT | TELESCOPE_HAS_LOCATION | TELESCOPE_HAS_PIER_SIDE, 4);

    LOG_DEBUG("Initializing from EQ500X device...");
}

/**************************************************************************************
**
***************************************************************************************/
const char *EQ500X::getDefautName()
{
    return (const char*)"EQ500X";
}

double EQ500X::getLST()
{
    return get_local_sidereal_time(lnobserver.lng);
}

void EQ500X::resetSimulation()
{
    simEQ500X = simEQ500X_zero;
}

/**************************************************************************************
**
***************************************************************************************/


bool EQ500X::initProperties()
{
    LX200Telescope::initProperties();

    // Mount tracks as soon as turned on
    TrackState = SCOPE_TRACKING;

    return true;
}

/**************************************************************************************
**
***************************************************************************************/
void EQ500X::getBasicData()
{
    /* */
}

bool EQ500X::checkConnection()
{
    if (!isSimulation())
    {
        if (PortFD <= 0)
            return false;

        LOG_DEBUG("Testing telescope connection using GR...");
        tty_set_debug(1);

        LOG_DEBUG("Clearing input...");
        tcflush(PortFD, TCIFLUSH);
    }

    for (int i = 0; i < 2; i++)
    {
        LOG_DEBUG("Getting RA/DEC...");
        if (getCurrentPosition(currentPosition) && 1 <= i)
        {
            LOG_DEBUG("Failure. Telescope is not responding to GR/GD!");
            return false;
        }
        const struct timespec timeout = {0, 50000000L};
        nanosleep(&timeout, nullptr);
    }

    currentRA = currentPosition.RAm();
    currentDEC = currentPosition.DECm();

    /* Blink the control pad */
#if 0
    const struct timespec timeout = {0, 250000000L};
    sendCmd(":RG#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RC#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RM#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RS#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RC#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RM#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RS#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RC#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RM#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RS#");
    nanosleep(&timeout, nullptr);
    sendCmd(":RG#");
#endif

    LOG_DEBUG("Connection check successful!");
    if (!isSimulation())
        tty_set_debug(0);
    return true;
}

bool EQ500X::updateLocation(double latitude, double longitude, double elevation)
{
    // Picked from ScopeSim
    INDI_UNUSED(elevation);
    lnobserver.lng = longitude;

    if (lnobserver.lng > 180)
        lnobserver.lng -= 360;
    lnobserver.lat = latitude;
    LOGF_INFO("Location updated: Longitude (%g) Latitude (%g)", lnobserver.lng, lnobserver.lat);

    // Only update LST if the mount is connected and "parked" looking at the pole
    if (isConnected() && !getCurrentPosition(currentPosition) && currentPosition.atParkingPosition())
    {
        double const LST = getLST();
        // Mechanical HA is east, 6 hours before meridian
        Sync(LST - 6, currentPosition.DECm());
        LOGF_INFO("Location updated: mount considered parked, synced to LST %gh", LST);
    }

    return true;
}

bool EQ500X::ReadScopeStatus()
{
    if (!isConnected())
        return false;

    if (getCurrentPosition(currentPosition))
    {
        EqNP.s = IPS_ALERT;
        IDSetNumber(&EqNP, "Error reading RA/DEC.");
        return false;
    }

    // If we are simulating, currentRA/currentDEC drive content of currentPosition, else currentPositition drives currentRA/currentDEC
    if (!isSimulation())
    {
        currentRA = currentPosition.RAm();
        currentDEC = currentPosition.DECm();
    }

    // If we are slewing, adjust movement and timer time to achieve arcsecond goto precision
    if (TrackState == SCOPE_SLEWING)
    {
        // Compute RA/DEC deltas - keep in mind RA is in hours on the mount, with a granularity of 15 degrees
        double const ra_delta = currentPosition.RA_degrees_to(targetPosition);
        double const dec_delta = currentPosition.DEC_degrees_to(targetPosition);
        double const abs_ra_delta = std::abs(ra_delta);
        double const abs_dec_delta = std::abs(dec_delta);

        // If mount is not at target, adjust
        if (RA_GRANULARITY <= abs_ra_delta || DEC_GRANULARITY <= abs_dec_delta)
        {
            // This will hold required adjustments in RA and DEC axes
            struct _adjustment const *ra_adjust = nullptr, *dec_adjust = nullptr;

            // Choose slew rate for RA based on distance to target
            for(size_t i = 0; i < sizeof(adjustments) && nullptr == ra_adjust; i++)
                if (abs_ra_delta <= adjustments[i].distance)
                    ra_adjust = &adjustments[i];
            assert(nullptr != ra_adjust);
            LOGF_DEBUG("RA  %lf-%lf = %+lf° under %lf° would require adjustment at %s until less than %lf°", targetPosition.RAm()*15.0, currentPosition.RAm()*15.0, ra_delta, ra_adjust->distance, ra_adjust->slew_rate, std::max(ra_adjust->epsilon, 15.0/3600.0));

            // Choose slew rate for DEC based on distance to target
            for(size_t i = 0; i < sizeof(adjustments) && nullptr == dec_adjust; i++)
                if (abs_dec_delta <= adjustments[i].distance)
                    dec_adjust = &adjustments[i];
            assert(nullptr != dec_adjust);
            LOGF_DEBUG("DEC %lf-%lf = %+lf° under %lf° would require adjustment at %s until less than %lf°", targetPosition.DECm(), currentPosition.DECm(), dec_delta, dec_adjust->distance, dec_adjust->slew_rate, dec_adjust->epsilon);

            // This will hold the command string to send to the mount, with move commands
            char CmdString[32] = {0};

            // Previous alignment marker to spot when to change slew rate
            static struct _adjustment const * previous_adjustment = nullptr;

            // We adjust the axis which has the faster slew rate first, eventually both axis at the same time if they have same speed
            struct _adjustment const * const adjustment = ra_adjust < dec_adjust ? dec_adjust : ra_adjust;
            if (previous_adjustment != adjustment)
            {
                // Add the new slew rate
                strcat(CmdString, adjustment->slew_rate);

                // If adjustment goes expectedly down, reset countdown
                if (adjustment < previous_adjustment)
                    countdown = MAX_CONVERGENCE_LOOPS;

                // FIXME: wait for the mount to slow down to improve convergence?

                // Remember previous adjustment
                previous_adjustment = adjustment;
            }
            LOGF_DEBUG("Current adjustment speed is %s", adjustment->slew_rate);

            // Movement markers, adjustment is done when no movement is required and all flags are cleared
            static bool east = false, west = false, north = false, south = false;

            // If RA is being adjusted, check delta against adjustment epsilon to enable or disable movement
            // The smallest change detectable in RA is 1/3600 hours, or 15/3600 degrees
            if (ra_adjust == adjustment)
            {
                // This is the lowest limit of this adjustment
                double const ra_epsilon = std::max(adjustment->epsilon, RA_GRANULARITY);

                // Find requirement
                bool const go_east = ra_epsilon <= ra_delta;
                bool const go_west = ra_delta <= -ra_epsilon;
                assert(!(go_east && go_west));

                // Stop movement if required - just stopping or going opposite
                if (east && (!go_east || go_west)) { strcat(CmdString, ":Qe#"); east = false; }
                if (west && (!go_west || go_east)) { strcat(CmdString, ":Qw#"); west = false; }

                // Initiate movement if required
                if (go_east && !east) { strcat(CmdString, ":Me#"); east = true; }
                if (go_west && !west) { strcat(CmdString, ":Mw#"); west = true; }
            }

            // If DEC is being adjusted, check delta against adjustment epsilon to enable or disable movement
            // The smallest change detectable in DEC is 1/3600 degrees
            if (dec_adjust == adjustment)
            {
                // This is the lowest limit of this adjustment
                double const dec_epsilon = std::max(adjustment->epsilon, DEC_GRANULARITY);

                // Find requirement
                bool const go_south = dec_epsilon <= dec_delta;
                bool const go_north = dec_delta <= -dec_epsilon;
                assert(!(go_south && go_north));

                // Stop movement if required - just stopping or going opposite
                if (south && (!go_south || go_north)) { strcat(CmdString, ":Qs#"); south = false; }
                if (north && (!go_north || go_south)) { strcat(CmdString, ":Qn#"); north = false; }

                // Initiate movement if required
                if (go_south && !south) { strcat(CmdString, ":Ms#"); south = true; }
                if (go_north && !north) { strcat(CmdString, ":Mn#"); north = true; }
            }

            // Basic algorithm sanitization on movement orientation: move one way or the other, or not at all
            assert(!(east && west) && !(north && south));

            // This log shows target in Degrees/Degrees and delta in Degrees/Degrees
            LOGF_DEBUG("Centering (%lf°,%lf°) delta (%lf°,%lf°) moving %c%c%c%c at %s until less than (%lf°,%lf°)", targetPosition.RAm()*15.0, targetPosition.DECm(), ra_delta, dec_delta, west?'W':'.', east?'E':'.', north?'N':'.', south?'S':'.', adjustment->slew_rate, std::max(adjustment->epsilon, RA_GRANULARITY), adjustment->epsilon);

            // If we have a command to run, issue it
            if (CmdString[0] != '\0' && sendCmd(CmdString))
            {
                LOGF_ERROR("Error centering (%lf°,%lf°)", targetPosition.RAm()*15.0, targetPosition.DECm());
                slewError(-1);
                return false;
            }

            // If simulating, do simulate rates - in that case currentPosition is driven by currentRA/currentDEC
            if (isSimulation())
            {
                // These are the simulated rates
                double const rates[sizeof(adjustments)] =
                {
                        /*RG*/5*ARCSECOND,
                        /*RC*/5*ARCMINUTE,
                        /*RM*/20*ARCMINUTE,
                        /*RS*/5*ONEDEGREE,
                        /*RS*/5*ONEDEGREE,
                };

                // Calculate elapsed time since last status read
                struct timespec clock = {0,0};
                clock_gettime(CLOCK_MONOTONIC, &clock);
                long const now = clock.tv_sec * 1000 + static_cast <long> (round(clock.tv_nsec / 1000000.0));
                double const delta = simEQ500X.last_sim ? static_cast <double> (now - simEQ500X.last_sim)/1000.0 : 0.0;
                simEQ500X.last_sim = now;

                // Use currentRA/currentDEC to store smaller-than-one-arcsecond values
                if (west) currentRA -= rates[adjustment - adjustments]*delta/15.0;
                if (east) currentRA += rates[adjustment - adjustments]*delta/15.0;
                if (north) currentDEC -= rates[adjustment - adjustments]*delta;
                if (south) currentDEC += rates[adjustment - adjustments]*delta;

                // Update current position and rewrite simulated mechanical positions
                currentPosition.RAm(currentRA);
                currentPosition.toStringRA(simEQ500X.MechanicalRA, sizeof(simEQ500X.MechanicalRA));
                currentPosition.DECm(currentDEC);
                currentPosition.toStringDEC(simEQ500X.MechanicalDEC, sizeof(simEQ500X.MechanicalDEC));

                LOGF_DEBUG("New RA/DEC simulated as %lf°/%lf° (%+lf°,%+lf°), stored as %lfh/%lf° = %s/%s", currentRA*15.0, currentDEC, (west||east)?rates[adjustment-adjustments]*delta:0, (north||south)?rates[adjustment-adjustments]*delta:0, currentPosition.RAm(), currentPosition.DECm(), simEQ500X.MechanicalRA, simEQ500X.MechanicalDEC);
            }

            // If all movement flags are cleared, we are done adjusting
            if (!east && !west && !north && !south)
            {
                LOGF_INFO("Centering delta (%lf,%lf) intermediate adjustment complete (%d loops)", ra_delta, dec_delta, MAX_CONVERGENCE_LOOPS - countdown);
            }
            // Else, if it has been too long since we started, maybe we have a convergence problem.
            // The mount slows down when requested to stop under minimum distance, so we may miss the target.
            // The behavior is improved by changing the slew rate while converging, but is still tricky to tune.
            else if (--countdown <= 0)
            {
                LOGF_ERROR("Failed centering to (%lf,%lf) under loop limit, aborting...", targetPosition.RAm(), targetPosition.DECm());
                goto slew_failure;
            }
            // Else adjust poll timeout to adjustment speed and continue
            else POLLMS = static_cast <uint32_t> (adjustment->polling_interval);
        }
        // If we attained target position at one arcsecond precision, finish procedure and track target
        else
        {
            LOG_INFO("Slew is complete. Tracking...");
            sendCmd(":Q#:RG#");
            POLLMS = 1000;
            TrackState = SCOPE_TRACKING;
            EqNP.s = IPS_OK;
            IDSetNumber(&EqNP, "Mount is tracking");
        }
    }

    // Update RA/DEC properties
    NewRaDec(currentPosition.RAm(), currentPosition.DECm());
    return true;

slew_failure:
    // If we failed at some point, attempt to stop moving and update properties with error
    sendCmd(":Q#");
    POLLMS = 1000;
    TrackState = SCOPE_TRACKING;
    NewRaDec(currentPosition.RAm(), currentPosition.DECm());
    slewError(-1);
    return false;
}

bool EQ500X::Goto(double ra, double dec)
{
    targetPosition.RAm(targetRA = ra);
    targetPosition.DECm(targetDEC = dec);

    // Check whether a meridian flip is required
    double const LST = getLST();
    double const HA = std::fmod(LST - ra + 12.0, 12.0);
    // Deduce orientation of mount in HA quadrants
    if ((-12 < HA && HA <= -6) || (0 <= HA && HA < 6))
        INDI::Telescope::setPierSide(PIER_WEST);
    else
        INDI::Telescope::setPierSide(PIER_EAST);
    LOGF_INFO("Goto target HA is %lf, LST is %lf, quadrant is %s", HA, LST, INDI::Telescope::getPierSide() == PIER_EAST ? "east" : INDI::Telescope::getPierSide() == PIER_WEST ? "west" : "unknown");
    targetPosition.setPierSide(INDI::Telescope::getPierSide());

    // Format RA/DEC for logs
    char RAStr[16]={0}, DecStr[16]={0};
    fs_sexa(RAStr, targetRA, 2, 3600);
    fs_sexa(DecStr, targetDEC, 2, 3600);

    // If moving, let's stop it first.
    if (EqNP.s == IPS_BUSY)
    {
        if (!isSimulation() && abortSlew(PortFD) < 0)
        {
            AbortSP.s = IPS_ALERT;
            IDSetSwitch(&AbortSP, "Abort slew failed.");
            return false;
        }

        AbortSP.s = IPS_OK;
        EqNP.s    = IPS_IDLE;
        IDSetSwitch(&AbortSP, "Slew aborted.");
        IDSetNumber(&EqNP, nullptr);

        if (MovementNSSP.s == IPS_BUSY || MovementWESP.s == IPS_BUSY)
        {
            MovementNSSP.s = MovementWESP.s = IPS_IDLE;
            EqNP.s                          = IPS_IDLE;
            IUResetSwitch(&MovementNSSP);
            IUResetSwitch(&MovementWESP);
            IDSetSwitch(&MovementNSSP, nullptr);
            IDSetSwitch(&MovementWESP, nullptr);
        }

        // sleep for 100 mseconds
        const struct timespec timeout = {0, 100000000L};
        nanosleep(&timeout, nullptr);
    }

    if (setTargetPosition(targetPosition))
    {
        EqNP.s = IPS_ALERT;
        IDSetNumber(&EqNP, "Error setting RA/DEC.");
        return false;
    }

    if (!isSimulation())
    {
        /* The goto feature is quite imprecise because it will always use full speed.
         * By the time the mount stops, the position is off by 0-5 degrees, depending on the speed attained during the move.
         * Additionally, a firmware limitation prevents the goto feature from slewing to close coordinates, and will cause uneeded axis rotation.
         * Therefore, don't use the goto feature for a goto, and let ReadScope adjust the position by itself.
         */
    }

    // Limit the number of loops
    countdown = MAX_CONVERGENCE_LOOPS;

    // Reset original adjustment
    // Reset movement markers

    TrackState = SCOPE_SLEWING;
    EqNP.s     = IPS_BUSY;

    LOGF_INFO("Slewing to JNow RA: %s - DEC: %s", RAStr, DecStr);

    return true;
}

bool EQ500X::Sync(double ra, double dec)
{
    if(!setTargetPosition(MechanicalPoint(ra, dec)))
    {
        if (!isSimulation())
        {
            char b[64/*RB_MAX_LEN*/] = {0};
            tcflush(PortFD, TCIFLUSH);

            if (getCommandString(PortFD, b, ":CM#") < 0)
                goto sync_error;
            if (!strncmp("No name", b, sizeof(b)))
                goto sync_error;
        }

        currentRA = currentPosition.RAm(targetPosition.RAm(ra));
        currentDEC = currentPosition.DECm(targetPosition.DECm(dec));

        if (isSimulation())
        {
            currentPosition.toStringRA(simEQ500X.MechanicalRA, sizeof(simEQ500X.MechanicalRA));
            currentPosition.toStringDEC(simEQ500X.MechanicalDEC, sizeof(simEQ500X.MechanicalDEC));
        }

        NewRaDec(currentRA, currentDEC);

        LOGF_INFO("Mount synced to target RA '%lf' DEC '%lf'", targetPosition.RAm(), targetPosition.DECm());
        return false;
    }

sync_error:
    EqNP.s = IPS_ALERT;
    IDSetNumber(&EqNP, "Synchronization failed.");
    LOGF_ERROR("Mount sync to target RA '%lf' DEC '%lf' failed", targetPosition.RAm(), targetPosition.DECm());
    return true;
}

void EQ500X::setPierSide(TelescopePierSide side)
{
    INDI_UNUSED(side);
    PierSideSP.s = IPS_ALERT;
    IDSetSwitch(&PierSideSP, "Not supported");
}

/**************************************************************************************
**
***************************************************************************************/

int EQ500X::sendCmd(char const *data)
{
    LOGF_DEBUG("CMD <%s>", data);
    if (!isSimulation())
    {
        int nbytes_write = 0;
        return tty_write_string(PortFD, data, &nbytes_write);
    }
    return 0;
}

int EQ500X::getReply(char *data, size_t const len)
{
    if (!isSimulation())
    {
        int nbytes_read = 0;
        int error_type = tty_read(PortFD, data, len, EQ500X_TIMEOUT, &nbytes_read);

        LOGF_DEBUG("RES <%.*s> (%d)", nbytes_read, data, error_type);
        return error_type;
    }
    return 0;
}

bool EQ500X::gotoTargetPosition()
{
    if (!isSimulation())
    {
        if (!sendCmd(":MS#"))
        {
            char buf[64/*RB_MAX_LEN*/] = {0};
            if (!getReply(buf, 1))
                return buf[0] != '0'; // 0 is valid for :MS
        }
        else return true;
    }
    return false;
}

bool EQ500X::getCurrentPosition(MechanicalPoint &p)
{
    char b[64/*RB_MAX_LEN*/] = {0};
    MechanicalPoint result = p;

    if (isSimulation())
        memcpy(b, simEQ500X.MechanicalRA, std::min(sizeof(b), sizeof(simEQ500X.MechanicalRA)));
    else if (getCommandString(PortFD, b, ":GR#") < 0)
        goto radec_error;

    if (result.parseStringRA(b, 64))
        goto radec_error;

    LOGF_DEBUG("RA reads '%s' as %lf.", b, result.RAm());

    if (isSimulation())
        memcpy(b, simEQ500X.MechanicalDEC, std::min(sizeof(b), sizeof(simEQ500X.MechanicalDEC)));
    else if (getCommandString(PortFD, b, ":GD#") < 0)
        goto radec_error;

    if (result.parseStringDEC(b, 64))
        goto radec_error;

    LOGF_DEBUG("DEC reads '%s' as %lf.", b, result.DECm());

    p = result;
    return false;

radec_error:
    return true;
}

bool EQ500X::setTargetPosition(MechanicalPoint const &p)
{
    if (!isSimulation())
    {
        // Size string buffers appropriately
        char CmdString[] = ":Sr" MechanicalPoint_RA_Format "#:Sd" MechanicalPoint_DEC_Format "#";
        char bufRA[sizeof(MechanicalPoint_RA_Format)], bufDEC[sizeof(MechanicalPoint_DEC_Format)];

        // Write RA/DEC in placeholders
        snprintf(CmdString, sizeof(CmdString), ":Sr%s#:Sd%s#", p.toStringRA(bufRA, sizeof(bufRA)), p.toStringDEC(bufDEC, sizeof(bufDEC)));
        LOGF_DEBUG("Target RA '%f' DEC '%f' converted to '%s'", static_cast <float> (p.RAm()), static_cast <float> (p.DECm()), CmdString);

        char buf[64/*RB_MAX_LEN*/] = {0};

        if (!sendCmd(CmdString))
            if (!getReply(buf, 2))
                if (buf[0] == '1' && buf[1] == '1')
                    return false;
                else LOGF_ERROR("Failed '%s', mount replied %c%c", CmdString, buf[0], buf[1]);
            else LOGF_ERROR("Failed getting 2-byte reply to '%s'", CmdString);
        else LOGF_ERROR("Failed '%s'", CmdString);

        return true;
    }

    return false;
}

/**************************************************************************************
**
***************************************************************************************/

EQ500X::MechanicalPoint::MechanicalPoint(double ra, double dec)
{
    RAm(ra);
    DECm(dec);
}

bool EQ500X::MechanicalPoint::atParkingPosition() const
{
    // Consider 0/+90 is pole - no way to check if synced already
    return this->operator ==(MechanicalPoint(0,90));
}

double EQ500X::MechanicalPoint::RAm() const
{
    return static_cast <double> (_RAm)/3600.0;
}

double EQ500X::MechanicalPoint::DECm() const
{
    return static_cast <double> (_DECm)/3600.0;
}

double EQ500X::MechanicalPoint::RAm(double const value)
{
    _RAm = std::lround(std::fmod(value+24.0,24.0)*3600.0);
    return RAm();
}

double EQ500X::MechanicalPoint::DECm(double const value)
{
    // Should be inside [-180,+180], even [-90,+90], but mount supports a larger (not useful) interval
    _DECm = std::lround(std::fmod(value+256.0,256.0)*3600.0);
    return DECm();
}

char const * EQ500X::MechanicalPoint::toStringRA(char *buf, size_t buf_length) const
{
    // See /test/test_eq500xdriver.cpp for description of RA conversion

    int const hours = ((_pierSide == PIER_WEST ? 12 : 0) + 24 + static_cast <int> (_RAm/3600)) % 24;
    int const minutes = static_cast <int> (_RAm/60) % 60;
    int const seconds = static_cast <int> (_RAm) % 60;

    int const written = snprintf(buf, buf_length, "%02d:%02d:%02d", hours, minutes, seconds);

    return (0 < written && written < (int) buf_length) ? buf : (char const*)0;
}

bool EQ500X::MechanicalPoint::parseStringRA(char const *buf, size_t buf_length)
{
    if (buf_length < sizeof(MechanicalPoint_RA_Format-1))
        return true;

    // Mount replies to "#GR:" with "HH:MM:SS".
    // HH, MM and SS are respectively hours, minutes and seconds in [00:00:00,23:59:59].
    // FIXME: Sanitize.

    int hours = 0, minutes = 0, seconds = 0;
    if (3 == sscanf(buf, "%02d:%02d:%02d", &hours, &minutes, &seconds))
    {
        _RAm = (    (_pierSide == PIER_WEST ? -12*3600 : +0) + 24*3600 +
                    (hours % 24) * 3600 +
                    minutes * 60 +
                    seconds ) % (24*3600);
        return false;
    }

    return true;
}

char const * EQ500X::MechanicalPoint::toStringDEC(char *buf, size_t buf_length) const
{
    // See /test/test_eq500xdriver.cpp for description of DEC conversion

    long const value = _pierSide == PIER_EAST ? (90*3600 - _DECm) : (_DECm - 90*3600);
    int const degrees = static_cast <int> (value/3600) % 256;
    int const minutes = static_cast <int> (std::abs(value)/60) % 60;
    int const seconds = static_cast <int> (std::abs(value)) % 60;

    if (degrees < -255 || +255 < degrees)
        return (char const*)0;

    char high_digit = '0';
    if (-100 < degrees && degrees < 100)
    {
        high_digit = '0' + (std::abs(degrees)/10);
    }
    else switch (std::abs(degrees)/10)
    {
    case 10: high_digit = ':'; break;
    case 11: high_digit = ';'; break;
    case 12: high_digit = '<'; break;
    case 13: high_digit = '='; break;
    case 14: high_digit = '>'; break;
    case 15: high_digit = '?'; break;
    case 16: high_digit = '@'; break;
    case 17: high_digit = 'A'; break;
    case 18: high_digit = 'B'; break;
    case 19: high_digit = 'C'; break;
    case 20: high_digit = 'D'; break;
    case 21: high_digit = 'E'; break;
    case 22: high_digit = 'F'; break;
    case 23: high_digit = 'G'; break;
    case 24: high_digit = 'H'; break;
    case 25: high_digit = 'I'; break;
    default: return (char const*)0; break;
    }

    char const low_digit = '0' + (std::abs(degrees)%10);

    int const written = snprintf(buf, buf_length, "%c%c%c:%02d:%02d", (0<=degrees)?'+':'-', high_digit, low_digit, minutes, seconds);

    return (0 < written && written < (int) buf_length) ? buf : (char const*)0;
}

bool EQ500X::MechanicalPoint::parseStringDEC(char const *buf, size_t buf_length)
{
    if (buf_length < sizeof(MechanicalPoint_DEC_Format)-1)
        return true;

    char b[sizeof(MechanicalPoint_DEC_Format)] = {0};
    strncpy(b, buf, sizeof(b));

    // Mount replies to "#GD:" with "sDD:MM:SS".
    // s is in {+,-} and provides a sign.
    // DD are degrees, unit D spans '0' to '9' in [0,9] but high D spans '0' to 'I' in [0,25].
    // MM are minutes, SS are seconds in [00:00,59:59].
    // The whole reply is in [-255:59:59,+255:59:59].

    struct _DecValues {
        char degrees[4];
        char minutes[3];
        char seconds[3];
    } * const DecValues = (struct _DecValues*) b;

    if (DecValues->degrees[1] < '0' || 'I' < DecValues->degrees[1])
        return true;

    int const sgn = DecValues->degrees[0] == '-' ? -1 : +1;

    // Replace sign with hundredth, or space if lesser than 100
    switch (DecValues->degrees[1])
    {
    case ':': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '0'; break;
    case ';': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '1'; break;
    case '<': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '2'; break;
    case '=': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '3'; break;
    case '>': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '4'; break;
    case '?': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '5'; break;
    case '@': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '6'; break;
    case 'A': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '7'; break;
    case 'B': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '8'; break;
    case 'C': DecValues->degrees[0] = '1'; DecValues->degrees[1] = '9'; break;
    case 'D': DecValues->degrees[0] = '2'; DecValues->degrees[1] = '0'; break;
    case 'E': DecValues->degrees[0] = '2'; DecValues->degrees[1] = '1'; break;
    case 'F': DecValues->degrees[0] = '2'; DecValues->degrees[1] = '2'; break;
    case 'G': DecValues->degrees[0] = '2'; DecValues->degrees[1] = '3'; break;
    case 'H': DecValues->degrees[0] = '2'; DecValues->degrees[1] = '4'; break;
    case 'I': DecValues->degrees[0] = '2'; DecValues->degrees[1] = '5'; break;
    default: DecValues->degrees[0] = '0'; break;
    }
    DecValues->degrees[3] = DecValues->minutes[2] = DecValues->seconds[2] = '\0';

    // FIXME: could be sscanf("%d%d%d", ...) but needs intermediate variables, however that would count matches
    int const orientation = _pierSide == PIER_EAST ? -1 : +1;
    _DECm = 90*3600 + orientation * sgn * (
                    atoi(DecValues->degrees)*3600 +
                    atoi(DecValues->minutes)*60 +
                    atoi(DecValues->seconds) );

    //LOGF_INFO("DEC converts as %f.", value);

    return false;
}

double EQ500X::MechanicalPoint::RA_degrees_to(EQ500X::MechanicalPoint const &b) const
{
    // RA is circular, DEC is not
    // We have hours and not degrees because that's what the mount is handling in terms of precision
    // We need to be cautious, as if we were to use real degrees, the RA movement would need to be 15 times more precise
    long delta = b._RAm - _RAm;
    if (delta > +12*3600) delta -= 24*3600;
    if (delta < -12*3600) delta += 24*3600;
    return static_cast <double> (delta * 15) / 3600.0;
}

double EQ500X::MechanicalPoint::DEC_degrees_to(EQ500X::MechanicalPoint const &b) const
{
    // RA is circular, DEC is not
    return static_cast <double> (b._DECm - _DECm) / 3600.0;
}

double EQ500X::MechanicalPoint::operator -(EQ500X::MechanicalPoint const &b) const
{
    double const ra_distance = RA_degrees_to(b);
    double const dec_distance = DEC_degrees_to(b);
    // FIXME: Incorrect distance calculation, but enough for our purpose
    return std::sqrt(ra_distance*ra_distance + dec_distance*dec_distance);
}

bool EQ500X::MechanicalPoint::operator !=(EQ500X::MechanicalPoint const &b) const
{
    return (_pierSide != b._pierSide) || (RA_GRANULARITY <= std::abs(RA_degrees_to(b))) || (DEC_GRANULARITY <= std::abs(DEC_degrees_to(b)));
}

bool EQ500X::MechanicalPoint::operator ==(EQ500X::MechanicalPoint const &b) const
{
    return !this->operator !=(b);
}

enum INDI::Telescope::TelescopePierSide EQ500X::MechanicalPoint::setPierSide(enum INDI::Telescope::TelescopePierSide pierSide)
{
    return _pierSide = pierSide;
}
