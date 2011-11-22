#include <glib.h>
#include <mavconn.h>
#include <ros/ros.h>

#include <asctec_hl_comm/WaypointActionGoal.h>
#include <geometry_msgs/PoseStamped.h>
#include <tf/transform_datatypes.h>

std::string lcmurl = "udpm://"; ///< host name for UDP server
bool verbose = false;

int sysid = 42;
int compid = 199;

/**
 * Grabs all mavlink_local_position_setpoint_set_t messages from MAVLINK and
 * publishes the corresponding asctec_hl_comm::WaypointActionGoal messages to
 * the ROS topic "fcu/waypoint/goal".
 * Grabs all geometry_msgs/PoseStamped messages from ROS and publishes the
 * corresponding messages to MAVLINK.
 */

ros::NodeHandle* nh = 0;
lcm_t *lcm = 0;

const double WGS84_A = 6378137.0;
const double WGS84_ECCSQ = 0.00669437999013;

double homeLatitude = 0.0;
double homeLongitude = 0.0;
double homeAltitude = 0.0;

char
utmLetterDesignator(double latitude)
{
    // This routine determines the correct UTM letter designator for the given latitude
    // returns 'Z' if latitude is outside the UTM limits of 84N to 80S
    // Written by Chuck Gantz- chuck.gantz@globalstar.com
    char letterDesignator;

    if ((84.0 >= latitude) && (latitude >= 72.0)) letterDesignator = 'X';
    else if ((72.0 > latitude) && (latitude >= 64.0)) letterDesignator = 'W';
    else if ((64.0 > latitude) && (latitude >= 56.0)) letterDesignator = 'V';
    else if ((56.0 > latitude) && (latitude >= 48.0)) letterDesignator = 'U';
    else if ((48.0 > latitude) && (latitude >= 40.0)) letterDesignator = 'T';
    else if ((40.0 > latitude) && (latitude >= 32.0)) letterDesignator = 'S';
    else if ((32.0 > latitude) && (latitude >= 24.0)) letterDesignator = 'R';
    else if ((24.0 > latitude) && (latitude >= 16.0)) letterDesignator = 'Q';
    else if ((16.0 > latitude) && (latitude >= 8.0)) letterDesignator = 'P';
    else if (( 8.0 > latitude) && (latitude >= 0.0)) letterDesignator = 'N';
    else if (( 0.0 > latitude) && (latitude >= -8.0)) letterDesignator = 'M';
    else if ((-8.0 > latitude) && (latitude >= -16.0)) letterDesignator = 'L';
    else if ((-16.0 > latitude) && (latitude >= -24.0)) letterDesignator = 'K';
    else if ((-24.0 > latitude) && (latitude >= -32.0)) letterDesignator = 'J';
    else if ((-32.0 > latitude) && (latitude >= -40.0)) letterDesignator = 'H';
    else if ((-40.0 > latitude) && (latitude >= -48.0)) letterDesignator = 'G';
    else if ((-48.0 > latitude) && (latitude >= -56.0)) letterDesignator = 'F';
    else if ((-56.0 > latitude) && (latitude >= -64.0)) letterDesignator = 'E';
    else if ((-64.0 > latitude) && (latitude >= -72.0)) letterDesignator = 'D';
    else if ((-72.0 > latitude) && (latitude >= -80.0)) letterDesignator = 'C';
    else letterDesignator = 'Z'; //This is here as an error flag to show that the Latitude is outside the UTM limits

    return letterDesignator;
}

void
lltoutm(double latitude, double longitude,
		double& utmNorthing, double& utmEasting,
		std::string& utmZone)
{
    // converts lat/long to UTM coords.  Equations from USGS Bulletin 1532
    // East Longitudes are positive, West longitudes are negative.
    // North latitudes are positive, South latitudes are negative
    // Lat and Long are in decimal degrees
    // Written by Chuck Gantz- chuck.gantz@globalstar.com

    double k0 = 0.9996;

    double LongOrigin;
    double eccPrimeSquared;
    double N, T, C, A, M;

    double LatRad = latitude * M_PI / 180.0;
    double LongRad = longitude * M_PI / 180.0;
    double LongOriginRad;

    int ZoneNumber = static_cast<int>((longitude + 180.0) / 6.0) + 1;

    if (latitude >= 56.0 && latitude < 64.0 &&
            longitude >= 3.0 && longitude < 12.0) {
        ZoneNumber = 32;
    }

    // Special zones for Svalbard
    if (latitude >= 72.0 && latitude < 84.0) {
        if (     longitude >= 0.0  && longitude <  9.0) ZoneNumber = 31;
        else if (longitude >= 9.0  && longitude < 21.0) ZoneNumber = 33;
        else if (longitude >= 21.0 && longitude < 33.0) ZoneNumber = 35;
        else if (longitude >= 33.0 && longitude < 42.0) ZoneNumber = 37;
    }
    LongOrigin = static_cast<double>((ZoneNumber - 1) * 6 - 180 + 3);  //+3 puts origin in middle of zone
    LongOriginRad = LongOrigin * M_PI / 180.0;

    // compute the UTM Zone from the latitude and longitude
    std::ostringstream oss;
    oss << ZoneNumber << utmLetterDesignator(latitude);
    utmZone = oss.str();

    eccPrimeSquared = WGS84_ECCSQ / (1.0 - WGS84_ECCSQ);

    N = WGS84_A / sqrt(1.0f - WGS84_ECCSQ * sin(LatRad) * sin(LatRad));
    T = tan(LatRad) * tan(LatRad);
    C = eccPrimeSquared * cos(LatRad) * cos(LatRad);
    A = cos(LatRad) * (LongRad - LongOriginRad);

    M = WGS84_A * ((1.0 - WGS84_ECCSQ / 4.0
                    - 3.0 * WGS84_ECCSQ * WGS84_ECCSQ / 64.0
                    - 5.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 256.0)
                   * LatRad
                   - (3.0 * WGS84_ECCSQ / 8.0
                      + 3.0 * WGS84_ECCSQ * WGS84_ECCSQ / 32.0
                      + 45.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 1024.0)
                   * sin(2.0 * LatRad)
                   + (15.0 * WGS84_ECCSQ * WGS84_ECCSQ / 256.0
                      + 45.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 1024.0)
                   * sin(4.0 * LatRad)
                   - (35.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 3072.0)
                   * sin(6.0 * LatRad));

    utmEasting = k0 * N * (A + (1.0 - T + C) * A * A * A / 6.0
                           + (5.0 - 18.0 * T + T * T + 72.0 * C
                              - 58.0 * eccPrimeSquared)
                           * A * A * A * A * A / 120.0)
                 + 500000.0;

    utmNorthing = k0 * (M + N * tan(LatRad) *
                        (A * A / 2.0 +
                         (5.0 - T + 9.0 * C + 4.0 * C * C) * A * A * A * A / 24.0
                         + (61.0 - 58.0 * T + T * T + 600.0 * C
                            - 330.0 * eccPrimeSquared)
                         * A * A * A * A * A * A / 720.0));
    if (latitude < 0.0) {
        utmNorthing += 10000000.0; //10000000 meter offset for southern hemisphere
    }
}

void
utmtoll(double utmNorthing, double utmEasting, const std::string& utmZone,
        double& latitude, double& longitude)
{
    // converts UTM coords to lat/long.  Equations from USGS Bulletin 1532
    // East Longitudes are positive, West longitudes are negative.
    // North latitudes are positive, South latitudes are negative
    // Lat and Long are in decimal degrees.
    // Written by Chuck Gantz- chuck.gantz@globalstar.com

    double k0 = 0.9996;
    double eccPrimeSquared;
    double e1 = (1.0 - sqrt(1.0 - WGS84_ECCSQ)) / (1.0 + sqrt(1.0 - WGS84_ECCSQ));
    double N1, T1, C1, R1, D, M;
    double LongOrigin;
    double mu, phi1, phi1Rad;
    double x, y;
    int ZoneNumber;
    char ZoneLetter;
    bool NorthernHemisphere;

    x = utmEasting - 500000.0; //remove 500,000 meter offset for longitude
    y = utmNorthing;

    std::istringstream iss(utmZone);
    iss >> ZoneNumber >> ZoneLetter;
    if ((ZoneLetter - 'N') >= 0) {
        NorthernHemisphere = true;//point is in northern hemisphere
    } else {
        NorthernHemisphere = false;//point is in southern hemisphere
        y -= 10000000.0;//remove 10,000,000 meter offset used for southern hemisphere
    }

    LongOrigin = (ZoneNumber - 1.0) * 6.0 - 180.0 + 3.0;  //+3 puts origin in middle of zone

    eccPrimeSquared = WGS84_ECCSQ / (1.0 - WGS84_ECCSQ);

    M = y / k0;
    mu = M / (WGS84_A * (1.0 - WGS84_ECCSQ / 4.0
                         - 3.0 * WGS84_ECCSQ * WGS84_ECCSQ / 64.0
                         - 5.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 256.0));

    phi1Rad = mu + (3.0 * e1 / 2.0 - 27.0 * e1 * e1 * e1 / 32.0) * sin(2.0 * mu)
              + (21.0 * e1 * e1 / 16.0 - 55.0 * e1 * e1 * e1 * e1 / 32.0)
              * sin(4.0 * mu)
              + (151.0 * e1 * e1 * e1 / 96.0) * sin(6.0 * mu);
    phi1 = phi1Rad / M_PI * 180.0;

    N1 = WGS84_A / sqrt(1.0 - WGS84_ECCSQ * sin(phi1Rad) * sin(phi1Rad));
    T1 = tan(phi1Rad) * tan(phi1Rad);
    C1 = eccPrimeSquared * cos(phi1Rad) * cos(phi1Rad);
    R1 = WGS84_A * (1.0 - WGS84_ECCSQ) /
         pow(1.0 - WGS84_ECCSQ * sin(phi1Rad) * sin(phi1Rad), 1.5);
    D = x / (N1 * k0);

    latitude = phi1Rad - (N1 * tan(phi1Rad) / R1)
               * (D * D / 2.0 - (5.0 + 3.0 * T1 + 10.0 * C1 - 4.0 * C1 * C1
                                 - 9.0 * eccPrimeSquared) * D * D * D * D / 24.0
                  + (61.0 + 90.0 * T1 + 298.0 * C1 + 45.0 * T1 * T1
                     - 252.0 * eccPrimeSquared - 3.0 * C1 * C1)
                  * D * D * D * D * D * D / 720.0);
    latitude *= 180.0 / M_PI;

    longitude = (D - (1.0 + 2.0 * T1 + C1) * D * D * D / 6.0
                 + (5.0 - 2.0 * C1 + 28.0 * T1 - 3.0 * C1 * C1
                    + 8.0 * eccPrimeSquared + 24.0 * T1 * T1)
                 * D * D * D * D * D / 120.0) / cos(phi1Rad);
    longitude = LongOrigin + longitude / M_PI * 180.0;
}

void
poseStampedCallback(const geometry_msgs::PoseStamped& poseStampedMsg)
{
	// set timestamp (get NSec from ROS and convert to us)
	uint64_t timestamp = poseStampedMsg.header.stamp.toNSec() / 1000;

	// send MAVLINK attitude and local position messages
	mavlink_message_t msg;

	//convert quaternion to euler angles
	const btQuaternion quat(poseStampedMsg.pose.orientation.x,
							poseStampedMsg.pose.orientation.y,
							poseStampedMsg.pose.orientation.z,
							poseStampedMsg.pose.orientation.w);
	const btMatrix3x3 mat(quat);
	double roll, pitch, yaw;
	mat.getEulerYPR(yaw, pitch, roll);

	mavlink_msg_attitude_pack(sysid, compid, &msg, timestamp, roll, pitch, yaw, 0.0f, 0.0f, 0.0f);
	//mavlink_message_t_publish(lcm, "MAVLINK", &msg);
	sendMAVLinkMessage(lcm, &msg);

	float x = poseStampedMsg.pose.position.x;
	float y = poseStampedMsg.pose.position.y;
	float z = poseStampedMsg.pose.position.z;

	mavlink_msg_local_position_ned_pack(sysid, compid, &msg, timestamp, x, y, z, 0.0f, 0.0f, 0.0f);
	//mavlink_message_t_publish(lcm, "MAVLINK", &msg);
	sendMAVLinkMessage(lcm, &msg);

	if (verbose)
	{
		ROS_INFO("Sent Mavlink local position and attitude messages.");
	}
}

void
paramCheckCallback(const ros::TimerEvent&)
{
	bool homeShift = false;

	double latitude;
	if (nh->getParamCached("/gps_ref_latitude", latitude) &&
		latitude != homeLatitude)
	{
		homeLatitude = latitude;
		homeShift = true;
	}

	double longitude;
	if (nh->getParamCached("/gps_ref_longitude", longitude) &&
		longitude != homeLongitude)
	{
		homeLongitude = longitude;
		homeShift = true;
	}

	double altitude;
	if (nh->getParamCached("/gps_ref_altitude", altitude) &&
		altitude != homeAltitude)
	{
		homeAltitude = altitude;
		homeShift = true;
	}

	if (homeShift)
	{
		mavlink_message_t msg;
		mavlink_msg_gps_global_origin_pack(sysid, compid, &msg,
				homeLatitude, homeLongitude, homeAltitude);
		//mavlink_message_t_publish(lcm, "MAVLINK", &msg);
		sendMAVLinkMessage(lcm, &msg);

		if (verbose)
		{
			ROS_INFO("Sent Mavlink GPS local origin set message.");
		}
	}
}

void
mavlinkHandler(const lcm_recv_buf_t* rbuf, const char* channel,
			   const mavconn_mavlink_msg_container_t* container, void* user)
{
	const mavlink_message_t* msg = getMAVLinkMsgPtr(container);
	switch (msg->msgid)
	{
		// get setpoint from MAVLINK
		case MAVLINK_MSG_ID_SET_LOCAL_POSITION_SETPOINT:
		{
			mavlink_set_local_position_setpoint_t setpoint;
			mavlink_msg_set_local_position_setpoint_decode(msg, &setpoint);

			// publish goal to ROS
			asctec_hl_comm::WaypointActionGoal goal;
			goal.goal_id.stamp = ros::Time::now();
			goal.goal.goal_pos.x = setpoint.x;
			goal.goal.goal_pos.y = setpoint.y;
			goal.goal.goal_pos.z = setpoint.z;
			goal.goal.goal_yaw = setpoint.yaw;
			goal.goal.max_speed.x = 2.0f;
			goal.goal.max_speed.y = 2.0f;
			goal.goal.max_speed.z = 2.0f;
			goal.goal.accuracy_position = 0.25f;
			goal.goal.accuracy_orientation = 0.1f;
			goal.goal.timeout = 60.0f;
			
			ros::Publisher* waypointPub = reinterpret_cast<ros::Publisher*>(user);
			waypointPub->publish(goal);
			
			if (verbose)
			{
				ROS_INFO("Sent ROS WaypointActionGoal message [%.2f %.2f %.2f %.2f].",
						 setpoint.x, setpoint.y, setpoint.z, setpoint.yaw);
			}

			break;
		}
		default: {}
	};
}

// Handling Program options
static GOptionEntry entries[] =
{
		{ "sysid", 'a', 0, G_OPTION_ARG_INT, &sysid, "ID of this system, 1-255", "42" },
		{ "compid", 'c', 0, G_OPTION_ARG_INT, &compid, "ID of this component, 1-255", "199" },
		{ "lcmurl", 'l', 0, G_OPTION_ARG_STRING, &lcmurl, "LCM URL to connect to", "udpm://" },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Verbose output", NULL },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 } };

int main(int argc, char **argv)
{
	ros::init(argc, argv, "mavconn_asctec_bridge");

	GError* error = NULL;
	GOptionContext* context = g_option_context_new("- translate messages between Asctec (ROS) and MAVCONN (LCM)");
	g_option_context_add_main_entries(context, entries, NULL);
	//g_option_context_add_group (context, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		g_print("Option parsing failed: %s\n", error->message);
		exit(1);
	}

	nh = new ros::NodeHandle;
	ros::Subscriber poseStampedSub = nh->subscribe("hex/fcu/current_pose", 10, poseStampedCallback);
	ros::Publisher waypointPub = nh->advertise<asctec_hl_comm::WaypointActionGoal>("hex/fcu/waypoint/goal", 10);
	
	// check for changed parameters on parameter server
	ros::Timer paramCheckTimer = nh->createTimer(ros::Duration(2.0), paramCheckCallback);

	/**
	 * Connect to LCM Channel and register for MAVLink messages ->
	 */
	lcm = lcm_create(lcmurl.c_str());
	if (!lcm)
	{
		exit(EXIT_FAILURE);
	}

	mavconn_mavlink_msg_container_t_subscription_t* mavlinkSub =
		mavconn_mavlink_msg_container_t_subscribe(lcm, "MAVLINK", &mavlinkHandler, &waypointPub);
	
	// start thread(s) to listen for ROS messages
	ros::AsyncSpinner spinner(1);
	spinner.start();

	// listen for LCM messages
	int lcm_fd = lcm_get_fileno(lcm);

	// wait a limited amount of time for an incoming LCM message
	struct timeval timeout = {
		1,	// seconds
		0	// microseconds
	};

	while (ros::ok())
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(lcm_fd, &fds);

		int status = select(lcm_fd + 1, &fds, 0, 0, &timeout);

		if (status != 0 && FD_ISSET(lcm_fd, &fds) && !ros::isShuttingDown())
		{
			// LCM has events ready to be processed.
			lcm_handle(lcm);
		}
	}

	delete nh;

	mavconn_mavlink_msg_container_t_unsubscribe(lcm, mavlinkSub);
	lcm_destroy(lcm);

	return 0;
}
