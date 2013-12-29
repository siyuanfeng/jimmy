#include "Plan.h"
#include "Logger.h"
#include "IK.h"
#include "ControlUtils.h"
#include "Utils.h"

#include <ros/ros.h>
#include <ros/package.h>

Plan plan;
Logger logger;
IKcmd IK_d;
IKcon IK;

const int MAX_POSES = 100;
const int MAX_GESTURES = 100;
const int MAX_POSES_PER_GESTURE = 100;
const int N_VALS_PER_POSE = 17;

static int N_POSES;

static double poses[MAX_POSES][N_VALS_PER_POSE];
static int gestureSeq[MAX_GESTURES][MAX_POSES_PER_GESTURE];
static double gestureTime[MAX_GESTURES][MAX_POSES_PER_GESTURE];
static int gestureNpose[MAX_GESTURES];

static double neckEAs[3];
static double theta_d[N_J+3];

const static double neckLims[2][3] = {
	{-1.4, -0.5, -0.5},
	{1.4, 0.5, 0.5}
};

const static double neckEAlims[2][3] = {
	{-1.4, -0.5, -0.5},
	{1.4, 0.5, 0.5}
};

const static double poseBodyLims[2][6] = {
	{-0.1, -0.1, 0.35, -0.3, -0.3, -0.3},
	{0.1, 0.1, 0.45, 0.3, 0.3, 0.3}
};

enum ConMode {
	IDLE=0,
	PRE_WALK,	//wait until we have enough steps queued up
	WALK,
	GESTURE
};

static double wallClockStart, wallClockLast, wallClockDT, wallClockT;

static bool isIdle;
static ConMode mode;

static double curTime;		//time since start of controller
static double modeTime;	//time since start of current mode
static double modeT0;		//time this mode started;
static double modeDur;		//intended duration of current mode

static double vForward, vLeft, dTheta;

bool isReady() {
	//TODO: wait until arbotix has moved robot to stand-prep
	return true;
}

int getCommand() {
	//TODO: have some way for these commands to arrive from outside
	if(curTime > 20.0)	return -1;
	if(curTime > 10.0 && curTime < 125.0)		return 1;

	return 0;
}

void getDriveCommand(double *vForward, double *vLeft, double *dTheta) {
	*vForward = 0.0;
	*vLeft = 0.02;
	*dTheta = 0.0;
	//TODO: implement retrieving these command from the user
}

void constrainDriveCommand(double *vForward, double *vLeft, double *dTheta) {
	//TODO: limit acceleration
	//TODO: limit velocities (including in combination)
}

void getNeckCommand(double *EAs) {
	double EAsd[3] = {0,0,0};
	//TODO: implement
	for(int i = 0; i < 3; i++)	EAs[i] += EAsd[i]*plan.TIME_STEP;
	for(int i = 0; i < 3; i++) {
		if(EAs[N_J+i] < neckEAlims[0][i])	EAs[N_J+i] = neckEAlims[0][i];
		if(EAs[N_J+i] > neckEAlims[1][i])	EAs[N_J+i] = neckEAlims[1][i];
	}
}





void initGesture(int gesture) {
	//TODO:  implement this function
	//set up joint trajectories using TrajEW; should be simple quintic splines between hardcoded poses
	//also set modeDur appropriately so we know when to end the gesture
}

void initWalk() {
	plan.initFeet(IK.ikrs.foot[LEFT][X], IK.ikrs.foot[LEFT][Y], getYaw(IK.ikrs.footQ[LEFT]), IK.ikrs.foot[RIGHT][X], IK.ikrs.foot[RIGHT][Y], getYaw(IK.ikrs.footQ[RIGHT]));
	for(int i = 0; i < 2; i++) {
		plan.com[i].push_back(IK.ikrs.com[i]);
		plan.comd[i].push_back(IK.ikrs.comd[i]);
	}
}

void loadPoses() {
	std::string name = ros::package::getPath("jimmy") + "/conf/poses.cf";
	std::ifstream in(name.c_str());

	int pose = 0;
	int i = 0;
	while (true) {
		in >> poses[pose][i];
		i++;
		if(i >= N_VALS_PER_POSE) {
			i = 0;
			pose++;
		}
		if (in.eof()) {
			if(i != 1) {
				printf("Ran out of data mid pose\n");
				exit(-1);
			}	
			printf("Loaded %d poses\n",pose);
			break;
		}
	}
	N_POSES = pose;

	//assemble pose limits from other limits
	double poseLimits[2][N_VALS_PER_POSE];
	for(int m = 0; m < 2; m++) {
		for(int i = 0; i < 8; i++)	poseLimits[m][i] = RobotState::jointLimits[m][i+12];
		for(int i = 0; i < 3; i++)	poseLimits[m][i+8] = neckEAlims[m][i];
		for(int i = 0; i < 6; i++)	poseLimits[m][i+11] = poseBodyLims[m][i];
	}

	for(int p = 0; p < pose; p++) {
		for(int i = 0; i < N_VALS_PER_POSE; i++) {
			if(poses[p][i] < poseLimits[0][i]) {
				printf("Limiting pose %d value %d from %g to %g\n", p, i, poses[p][i], poseLimits[0][i]);
				poses[p][i] = poseLimits[0][i];
			}
			if(poses[p][i] > poseLimits[1][i]) {
				printf("Limiting pose %d value %d from %g to %g\n", p, i, poses[p][i], poseLimits[1][i]);
				poses[p][i] = poseLimits[1][i];
			}
		}
	}
}

void loadGestures() {
	std::string name = ros::package::getPath("jimmy") + "/conf/gestures.cf";
	std::ifstream in(name.c_str());

	int gesture = 0;
	while (true) {
		in >> gestureNpose[gesture];
		if (in.eof()) 	break;
		for(int i = 0; i < gestureNpose[gesture]; i++) {
			in >> gestureSeq[gesture][i];
			if(gestureSeq[gesture][i] < 0) {
				printf("We only use positive pose ID's: %d\n",gestureSeq[gesture][i]);
				exit(-1);
			}
			if(gestureSeq[gesture][i] >= N_POSES) {
				printf("We only have %d poses stored: %d\n",N_POSES, gestureSeq[gesture][i]);
				exit(-1);
			}
			in >> gestureTime[gesture][i];
			if(gestureTime[gesture][i] <= 0) {
				printf("Require a positive time in gesture %d, pose %d: %g\n", gesture, i, gestureTime[gesture][i]);
				exit(-1);
			}
		}
		if (in.eof()) {
			printf("Ran out of data mid gesture\n");
			exit(-1);
		}
		gesture++;
	}
	printf("Loaded %d gestures\n",gesture);
}

void init() {
	printf("Start init\n");
	loadPoses();
	loadGestures();
	mode = IDLE;
	isIdle = false;
	curTime = 0.0;
	modeTime = 0.0;
	modeT0 = 0.0;
	modeDur = 0.0;
	for(int i = 0; i < 3; i++)		neckEAs[i] = 0.0;

	std::string name = ros::package::getPath("jimmy") + "/conf/plan.cf";
	plan = Plan(name.c_str());
	name = ros::package::getPath("jimmy") + "/conf/IK.cf";
	IK.readParams(name.c_str());
	logger.init(plan.TIME_STEP);
	logger.add_datapoint("realT","s",&wallClockT);
	logger.add_datapoint("realDT","s",&wallClockDT);
	logger.add_datapoint("curTime","s",&curTime);
	logger.add_datapoint("modeTime","s",&modeTime);
	logger.add_datapoint("mode","-",(int*)(&mode));
	logger.add_datapoint("CMD.vForward","m/s",&vForward);
	logger.add_datapoint("CMD.vLeft","m/s",&vLeft);
	logger.add_datapoint("CMD.dTheta","m/s",&dTheta);
	IK_d.addToLog(logger);
	IK.addToLog(logger);
	plan.addToLog(logger);
	IK_d.setToRS(IK.ikrs);
	IK_d.setVel0();
}



void stateMachine() {
	int command;
	switch(mode) {
	case IDLE:
		command = getCommand();
		//don't want to make this unreadable with a switch inside a switch
		if(command == 1) {
			modeT0 = curTime;
			mode = PRE_WALK;
			initWalk();
		}
		else if(command > 1) {	//each number corresponds to a gesture
			modeT0 = curTime;
			mode = GESTURE;
			initGesture(command);
		}
		break;
	case PRE_WALK:
		if(modeTime > plan.PRE_PLAN_TIME) {
			modeT0 = curTime;
			mode = WALK;
		}
		break;
	case WALK:
		if(getCommand() == 0) {
			plan.stopHere();
		}
		if(plan.isDone(modeTime)) {
			modeT0 = curTime;
			mode = IDLE;
		}
		break;
	case GESTURE:
		if(modeTime > modeDur) {
			modeT0 = curTime;
			mode = IDLE;
		}
		break;
	default:
		printf("Bad mode in stateMachine\n");
		exit(-1);
		break;
	}
}

double nomPose[3] = {0.0, 0.0, 0.42};

//integrate towards normal body pose
void idleCon() {
	//CoM pos
	for(int i = 0; i <3; i++) {
		IK_d.com[i] += 0.004*(nomPose[i]-(IK_d.com[i]-(IK_d.foot[LEFT][i]+IK_d.foot[RIGHT][i])/2.0));
	}
	
	//foot orientation
	for(int s = 0; s < 2; s++) {
		Eigen::Quaterniond desQ = IK_d.footQ[s];
		flattenQuat(desQ);
		IK_d.footQ[s] = mySlerp(IK_d.footQ[s], desQ, 0.004);
	}

	//torso orientation
	Eigen::Quaterniond desBodyQ = mySlerp(IK_d.footQ[LEFT], IK_d.footQ[RIGHT], 0.5);
	IK_d.rootQ = mySlerp(IK_d.rootQ, desBodyQ, 0.004);
}

void walkCon() {
	for(int i = 0; i < 3; i++)	neckEAs[i] *= 0.995;
	plan.fillIK_d(IK_d, modeTime);
}

void gestureCon() {
	//TODO: implement
	//read trajectories set up in initGesture()
	//set up IK_d
}


void controlLoop() {

	//handle mode switching
	stateMachine();

	modeTime = curTime-modeT0;

	//whipe out record values
	vForward = vLeft = dTheta = 0.0;
	plan.clearForRecord();
	
	//do actual control
	switch(mode) {
	case IDLE:
		getNeckCommand(neckEAs);
		idleCon();
		break;
	case PRE_WALK:
		idleCon();
		getDriveCommand(&vForward, &vLeft, &dTheta);
		constrainDriveCommand(&vForward, &vLeft, &dTheta);
		plan.driveFutureRobot(vForward, vLeft, dTheta);
		break;
	case WALK:
		getDriveCommand(&vForward, &vLeft, &dTheta);
		constrainDriveCommand(&vForward, &vLeft, &dTheta);
		plan.driveFutureRobot(vForward, vLeft, dTheta);
		walkCon();
		break;
	case GESTURE:
		gestureCon();
		break;
	default:
		printf("Bad mode in controlLoop\n");
		exit(-1);
		break;
	}
	

	double thetad_d[N_J+3];
	IK.IK(IK_d, theta_d, thetad_d);

	//conversion from RPY to angles
	theta_d[N_J] = neckEAs[2];
	theta_d[N_J+1] = neckEAs[1]+neckEAs[0];
	theta_d[N_J+2] = neckEAs[1]-neckEAs[0];
	//theta_d[N_J] = sin(2*M_PI*modeTime); //neckEAs[2];
	//theta_d[N_J+1] = sin(2*M_PI*modeTime); //neckEAs[1]+neckEAs[0];
	//theta_d[N_J+2] = sin(2*M_PI*modeTime); //neckEAs[1]-neckEAs[0];

	//limit angles
	for(int i = 0; i < 3; i++) {
		thetad_d[N_J+i] = 0.0;
		if(theta_d[N_J+i] < neckLims[0][i])	theta_d[N_J+i] = neckLims[0][i];
		if(theta_d[N_J+i] > neckLims[1][i])	theta_d[N_J+i] = neckLims[1][i];
	}

	//TODO: pass IK commands to motors
	//TODO: pass neck commands to motors
}

/*
// no arbotix stuff
int main( int argc, char **argv ) {
	wallClockStart = get_time();
	init();
	printf("Waiting for Arbotix\n");
	while(!isReady()) {
		//do nothing
	}
	printf("Starting\n");
	while(true) {
		double wallNow = get_time();
		wallClockT = wallNow-wallClockStart;
		wallClockDT = wallNow-wallClockLast;
		wallClockLast = wallNow;
		curTime += plan.TIME_STEP;		//maybe do this off a real clock if we're not getting true real time accurately

		controlLoop();

		logger.saveData();
		//TODO:  wait until plan.TIME_STEP

		if(getCommand() == -1) {
			logger.writeToMRDPLOT();
			exit(-1);
		}
	}

	return 374832748;
}
*/


int main( int argc, char **argv ) 
{
  ControlUtils utils;

	wallClockStart = get_time();
	init();
  //////////////////////////////////////////////
	printf("Stand Preping\n");
	   
  double joints_d[TOTAL_JOINTS] = {0};
  for (int i = 0; i < TOTAL_JOINTS; i++)
    joints_d[i] = standPrepPose[i];

  utils.sendStandPrep(joints_d);
  utils.waitForReady();
  //////////////////////////////////////////////

	printf("Starting\n");

  wallClockLast = get_time();
  double timeQuota = plan.TIME_STEP;
	while(true) {
		double wallNow = get_time();
		wallClockT = wallNow-wallClockStart;
		wallClockDT = wallNow-wallClockLast;
		wallClockLast = wallNow;
		curTime += plan.TIME_STEP;		//maybe do this off a real clock if we're not getting true real time accurately

		controlLoop();

		logger.saveData();

		if(getCommand() == -1) {
			logger.writeToMRDPLOT();
			exit(-1);
		}
    
    //////////////////////////////////////////////
    // wait
    double wall1 = get_time();
    double dt = wall1 - wallNow;
    
    // step up quota for the next time step
    if (dt > timeQuota) {
      timeQuota -= (dt - plan.TIME_STEP);
      printf("takes too long %g\n", dt);
      utils.sendJoints_d(theta_d);
    }
    else {
      timeQuota = plan.TIME_STEP;
      int sleep_t = (int)((plan.TIME_STEP - dt)*1e6);
      
      utils.sendJoints_d(theta_d);
      usleep(sleep_t);
    }
    //////////////////////////////////////////////
	}

	return 374832748;
}

