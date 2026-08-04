// -*-c++-*-

/***************************************************************************
 client.cpp  -  A basic client that connects to
 the server
 -------------------
 begin                : 27-DEC-2001
 copyright            : (C) 2001 by The RoboCup Soccer Server
 Maintenance Group.
 email                : sserver-admin@lists.sourceforge.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU LGPL as published by the Free Software  *
 *   Foundation; either version 3 of the License, or (at your option) any  *
 *   later version.                                                        *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H  //条件编译  C++预处理器
#include "config.h"
#endif

#include "compress.h"

#include <rcssbase/net/socketstreambuf.hpp> //rcssbase可以实现网络连接、参数读取、序列化等一些通用的功能，是仿真比赛server 的底层库
#include <rcssbase/net/udpsocket.hpp> //hpp,其实质就是将.cpp的实现代码混入.h头文件当中，定义与实现都包含在同一文件
#include <rcssbase/gzip/gzstream.hpp>

#ifdef HAVE_SSTREAM // 在C++有两种字符串流，一种在sstream中定义，另一种在strstream中定义
#include <sstream> //处理字符串
#else//C++引入了ostringstream、istringstream、stringstream这三个类，要使用他们创建对象就必须包含sstream.h头文件
#include <strstream> //字符串流操作 与c兼容
#endif
#include <iostream> //输入输出流
#include <cerrno> // errno 是记录系统的最后一次错误代码  在errno.h中定义  errno不同的值表示不同的含义
#include <csignal> // 定义了程序执行时如何处理不同的信号
#include <cstdio> //cstdio是将stdio.h的内容用C++头文件的形式表示出来。stdio.h是C标准函数库中的头文件，提供基本的文字的输入输出流操作
#include <cstdlib> //cstdlib是C++里面的一个常用函数库， 等价于C中的<stdlib.h>,stdlib.h可以提供一些函数与符号常量，
#include <cstring> //<cstring>是C标准库头文件<string.h>的C++标准库版本  包含了strcmp、strchr、strstr等操作
#include <string>
#include <string.h> // C版本头文件 对应基于char*的字符串处理函数
#include <cmath>

#ifdef __CYGWIN__
// cygwin 不算 win32
#elif defined(_WIN32) || defined(__WIN32__) || defined (WIN32)
#  define RCSS_WIN
#  include <winsock2.h> // 连接系统和用户使用的软件之间用于交流的一个接口，这个功能就是修复软件与系统正确的通讯的作用
#endif

#ifndef RCSS_WIN
#  include <unistd.h> // 是 C++ 程序设计语言中提供对操作系统 API 的访问功能的头文件的名称
#  include <sys/select.h> // select是一个计算机函数，位于头文件#include <sys/select.h> 。该函数用于监视文件描述符的变化情况——读写或是异常。
#  include <sys/time.h> // 定义了四个变量类型、两个宏和各种操作日期和时间的函数
#  include <sys/types.h> // 基本系统数据类型 定义了很多类型
#endif

int iPlayerId = 0;
int iSide = 0;//1:left;2:right
int lastSeeCycle = -1;
// ===== 守门员状态机 =====
enum GoalieState {
    GS_HOLDING,      // 守位（默认）
    GS_ALIGNING,     // 横向调整
    GS_CHASING,      // 迎球（威胁球）
    GS_CATCHING,     // 正在扑球
    GS_CLEARING,     // 接球后解围
    GS_RETURNING     // 回门
};

GoalieState goalieState = GS_HOLDING;
int goalieCatchAttemptCycle = -100;
double goalieCatchAttemptDistance = 1000.0;
bool goalieCatchSuccess = false;
int goalieCatchCycle = -100;
int goalieAdvanceSteps = 0;
int goalieLastBallCycle = -100;
double goalieLastBallDirection = 0.0;
double goalieLastBallDistance = 1000.0;
int goalieLastConfirmedAreaCycle = -100;
double goalieLastBallToOwnGoal = 1000.0;

enum DefenderMode {
	DM_SHAPE,
	DM_CHALLENGE,
	DM_RECOVER
};

DefenderMode defenderMode = DM_SHAPE;
int defenderModeSinceCycle = -100;
// 默认为空：run() 按 iSide 选用 team1/team2，保证左右为两支不同队伍。
// 若传入 -team，则使用自定义队名覆盖上述默认。
std::string teamName;
bool teamNameFromArg = false;

double absDouble(double value) {
	return value < 0.0 ? -value : value;
}

double normalizeAngle(double angle) {
	while (angle > 180.0) angle -= 360.0;
	while (angle < -180.0) angle += 360.0;
	return angle;
}

struct SeenPlayer {
	int unum;
	double distance;
	double direction;
};

struct VisualState {
	int cycle;
	bool hasBall;
	double ballDistance;
	double ballDirection;
	double ballNeckDirection;
	bool hasBallMotion;
	double ballDistanceChange;
	double ballDirectionChange;
	bool hasOwnGoal;
	bool hasAttackGoal;
	double ownGoalDistance;
	double ownGoalDirection;
	double attackGoalDistance;
	double attackGoalDirection;
	SeenPlayer teammates[4];
	int teammateCount;
	SeenPlayer opponents[5];
	int opponentCount;
};

struct BallTrackState {
	bool hasSample;
	bool hasMotion;
	int lastCycle;
	double lastDistance;
	double lastDirection;
	double closingRate;
	double directionRate;
	int closingStreak;
};

BallTrackState gBallTrack = {
	false, false, -100, 0.0, 0.0, 0.0, 0.0, 0
};

enum PlayMode {
	PM_UNKNOWN = 0,
	PM_BEFORE_KICK_OFF,
	PM_PLAY_ON,
	PM_GOAL_L,
	PM_GOAL_R,
	PM_KICK_OFF_L,
	PM_KICK_OFF_R,
	PM_GOAL_KICK_L,
	PM_GOAL_KICK_R,
	PM_CORNER_KICK_L,
	PM_CORNER_KICK_R,
	PM_KICK_IN_L,
	PM_KICK_IN_R,
	PM_FREE_KICK_L,
	PM_FREE_KICK_R,
	PM_OTHER
};

struct MatchState {
	PlayMode playMode;
	int sideFromServer; // 0 unknown, 1 left, 2 right
	int unumFromServer;
};

MatchState gMatchState = { PM_UNKNOWN, 0, 0 };

struct BodyState {
	bool hasStamina;
	bool hasSpeed;
	double stamina;
	double effort;
	double speed;
	double headAngle;
	int cycle;
};

BodyState gBodyState = { false, false, 8000.0, 1.0, 0.0, 0.0, -1 };
int lastHeardChaseUnum = 0;
int lastHeardChaseCycle = -100;
int lastPassTargetUnum = 0;
int lastPassCycle = -100;
double lastPassSourceDirection = 0.0;
int lastChaseSayCycle = -100;
int lastOwnTouchCycle = -100;
int ownTouchFollowUntilCycle = -100;
int lastSeenBallCycle = -100;
double lastSeenBallDirection = 0.0;
int lastOwnKickOffCycle = -100;
int kickoffTouchCycle = -100;
int kickoffSecondTouchLockUntilCycle = -100;
int lastTackleCycle = -100;
int lastDefenderKickCycle = -100;
int lastSetPieceKickCycle = -100;
double lastSeenBallDistance = 1000.0;
const char *gLastChaseReason = "none";
const char *gLastAttackAction = "none";
int lastForwardMode = 0; // 1 chase, 2 support
int lastForwardModeCycle = -100;
bool hasAttackDirectionEstimate = false;
double attackDirectionEstimate = 0.0;
int attackDirectionLandmarkCycle = -100;

double absDouble(double value);
double normalizeAngle(double angle);

bool getAttackDirection(const VisualState &state, double &direction) {
	if (state.hasAttackGoal) {
		direction = state.attackGoalDirection;
		return true;
	}
	if (state.hasOwnGoal) {
		direction = normalizeAngle(state.ownGoalDirection + 180.0);
		return true;
	}
	if (hasAttackDirectionEstimate
			&& state.cycle - attackDirectionLandmarkCycle >= 0
			&& state.cycle - attackDirectionLandmarkCycle <= 300) {
		direction = attackDirectionEstimate;
		return true;
	}
	return false;
}

void clearTransientAttackState() {
	lastHeardChaseUnum = 0;
	lastHeardChaseCycle = -100;
	lastPassTargetUnum = 0;
	lastPassCycle = -100;
	lastPassSourceDirection = 0.0;
	lastChaseSayCycle = -100;
	lastOwnTouchCycle = -100;
	ownTouchFollowUntilCycle = -100;
	lastSeenBallCycle = -100;
	lastSeenBallDirection = 0.0;
	lastOwnKickOffCycle = -100;
	kickoffTouchCycle = -100;
	kickoffSecondTouchLockUntilCycle = -100;
	lastTackleCycle = -100;
	lastDefenderKickCycle = -100;
	lastSetPieceKickCycle = -100;
	lastSeenBallDistance = 1000.0;
	goalieLastBallDistance = 1000.0;
	goalieLastConfirmedAreaCycle = -100;
	goalieLastBallToOwnGoal = 1000.0;
	lastForwardMode = 0;
	lastForwardModeCycle = -100;
	defenderMode = DM_SHAPE;
	defenderModeSinceCycle = -100;
	gBallTrack.hasSample = false;
	gBallTrack.hasMotion = false;
	gBallTrack.lastCycle = -100;
	gBallTrack.closingRate = 0.0;
	gBallTrack.directionRate = 0.0;
	gBallTrack.closingStreak = 0;
	gLastChaseReason = "restart";
	gLastAttackAction = "restart_wait";
}

double clampDouble(double value, double minimum, double maximum) {
	if (value < minimum) return minimum;
	if (value > maximum) return maximum;
	return value;
}

void markOwnTouch(int cycle, int followCycles) {
	lastOwnTouchCycle = cycle;
	ownTouchFollowUntilCycle = cycle + followCycles;
}

bool kickoffSecondTouchLocked(int cycle) {
	return iPlayerId == 4
			&& kickoffTouchCycle >= 0
			&& cycle >= kickoffTouchCycle
			&& cycle <= kickoffSecondTouchLockUntilCycle;
}

void releaseKickoffSecondTouchLock() {
	kickoffSecondTouchLockUntilCycle = -100;
}

void updateBallTrack(const VisualState &state) {
	if (!state.hasBall) {
		return;
	}

	if (state.hasBallMotion) {
		const double rawClosing = clampDouble(
				-state.ballDistanceChange, -3.0, 3.0);
		const double rawDirection = clampDouble(
				state.ballDirectionChange, -12.0, 12.0);
		if (gBallTrack.hasMotion
				&& state.cycle - gBallTrack.lastCycle > 0
				&& state.cycle - gBallTrack.lastCycle <= 3) {
			gBallTrack.closingRate = gBallTrack.closingRate * 0.4
					+ rawClosing * 0.6;
			gBallTrack.directionRate = gBallTrack.directionRate * 0.4
					+ rawDirection * 0.6;
		}
		else {
			gBallTrack.closingRate = rawClosing;
			gBallTrack.directionRate = rawDirection;
		}
		gBallTrack.hasMotion = true;
		if (rawClosing > 0.3) {
			if (gBallTrack.closingStreak < 10) {
				++gBallTrack.closingStreak;
			}
		}
		else {
			gBallTrack.closingStreak = 0;
		}
	}
	else {
		gBallTrack.hasMotion = false;
		gBallTrack.closingRate = 0.0;
		gBallTrack.directionRate = 0.0;
		gBallTrack.closingStreak = 0;
	}

	gBallTrack.hasSample = true;
	gBallTrack.lastCycle = state.cycle;
	gBallTrack.lastDistance = state.ballDistance;
	gBallTrack.lastDirection = state.ballDirection;
}

double predictedBallDirection(const VisualState &state, int maxLeadCycles) {
	if (!state.hasBall || !gBallTrack.hasMotion
			|| state.cycle != gBallTrack.lastCycle) {
		return state.ballDirection;
	}

	// The server-provided angular change is still observer-relative. Use it
	// only as a small lead correction; large values usually include our turn.
	if (absDouble(gBallTrack.directionRate) > 6.0) {
		return state.ballDirection;
	}

	int leadCycles = maxLeadCycles;
	if (state.ballDistance < 5.0) leadCycles = 1;
	else if (state.ballDistance < 15.0 && leadCycles > 2) leadCycles = 2;
	const double lead = clampDouble(gBallTrack.directionRate * leadCycles,
			-8.0, 8.0);
	return normalizeAngle(state.ballDirection + lead);
}

int kickPowerForDistance(double distance) {
	int power = static_cast<int>(distance * 2.35 + 0.5);
	if (power < 12) power = 12;
	if (power > 70) power = 70;
	return power;
}

bool isRestartHoldMode() {
	return gMatchState.playMode == PM_BEFORE_KICK_OFF
			|| gMatchState.playMode == PM_GOAL_L
			|| gMatchState.playMode == PM_GOAL_R;
}

bool isStructuredRestartMode(PlayMode mode) {
	return mode == PM_GOAL_KICK_L || mode == PM_GOAL_KICK_R
			|| mode == PM_CORNER_KICK_L || mode == PM_CORNER_KICK_R
			|| mode == PM_KICK_IN_L || mode == PM_KICK_IN_R
			|| mode == PM_FREE_KICK_L || mode == PM_FREE_KICK_R;
}

int activeSide() {
	if (gMatchState.sideFromServer == 1
			|| gMatchState.sideFromServer == 2) {
		return gMatchState.sideFromServer;
	}
	return iSide;
}

bool isOwnKickOffMode() {
	const int side = activeSide();
	return (side == 1 && gMatchState.playMode == PM_KICK_OFF_L)
			|| (side == 2 && gMatchState.playMode == PM_KICK_OFF_R);
}

bool isOwnStructuredRestartMode() {
	const int side = activeSide();
	if (side == 1) {
		return gMatchState.playMode == PM_GOAL_KICK_L
				|| gMatchState.playMode == PM_CORNER_KICK_L
				|| gMatchState.playMode == PM_KICK_IN_L
				|| gMatchState.playMode == PM_FREE_KICK_L;
	}
	return gMatchState.playMode == PM_GOAL_KICK_R
			|| gMatchState.playMode == PM_CORNER_KICK_R
			|| gMatchState.playMode == PM_KICK_IN_R
			|| gMatchState.playMode == PM_FREE_KICK_R;
}

bool isOwnGoalKickMode() {
	const int side = activeSide();
	return (side == 1 && gMatchState.playMode == PM_GOAL_KICK_L)
			|| (side == 2 && gMatchState.playMode == PM_GOAL_KICK_R);
}

bool isPreparingOwnKickOff() {
	const int side = activeSide();
	return isOwnKickOffMode()
			|| (side == 1 && gMatchState.playMode == PM_GOAL_R)
			|| (side == 2 && gMatchState.playMode == PM_GOAL_L);
}

double estimateDistance(double selfToA, double selfToB,
		double dirA, double dirB) {
	const double delta = (dirA - dirB) * 3.141592653589793 / 180.0;
	const double d2 = selfToA * selfToA + selfToB * selfToB
			- 2.0 * selfToA * selfToB * std::cos(delta);
	return d2 > 0.0 ? std::sqrt(d2) : 0.0;
}

int chaseRoleRank(int playerId) {
	if (playerId == 5) return 4;
	if (playerId == 4) return 3;
	if (playerId == 2) return 2;
	if (playerId == 3) return 1;
	return 0;
}

bool parseInitMessage(const char *msg) {
	char sideChar = 0;
	int unum = 0;
	if (std::sscanf(msg, "(init %c %d", &sideChar, &unum) < 2) {
		return false;
	}
	if (sideChar == 'l' || sideChar == 'L') {
		gMatchState.sideFromServer = 1;
	}
	else if (sideChar == 'r' || sideChar == 'R') {
		gMatchState.sideFromServer = 2;
	}
	else {
		return false;
	}
	gMatchState.unumFromServer = unum;
	if (gMatchState.sideFromServer == 1 || gMatchState.sideFromServer == 2) {
		iSide = gMatchState.sideFromServer;
	}
	if (unum >= 1 && unum <= 5) {
		iPlayerId = unum;
	}
	printf("init_parse: sideFromServer=%d unumFromServer=%d iSide=%d iPlayerId=%d\n",
			gMatchState.sideFromServer, gMatchState.unumFromServer,
			iSide, iPlayerId);
	return true;
}

bool parseRefereeMessage(const char *msg, MatchState &state) {
	// (hear <time> referee <play_mode>)
	const char *referee = std::strstr(msg, "referee ");
	if (referee == 0) {
		return false;
	}
	referee += 8;
	if (std::strncmp(referee, "before_kick_off", 15) == 0) {
		state.playMode = PM_BEFORE_KICK_OFF;
	}
	else if (std::strncmp(referee, "play_on", 7) == 0) {
		state.playMode = PM_PLAY_ON;
	}
	else if (std::strncmp(referee, "goal_l", 6) == 0) {
		state.playMode = PM_GOAL_L;
	}
	else if (std::strncmp(referee, "goal_r", 6) == 0) {
		state.playMode = PM_GOAL_R;
	}
	else if (std::strncmp(referee, "kick_off_l", 10) == 0) {
		state.playMode = PM_KICK_OFF_L;
	}
	else if (std::strncmp(referee, "kick_off_r", 10) == 0) {
		state.playMode = PM_KICK_OFF_R;
	}
	else if (std::strncmp(referee, "goal_kick_l", 11) == 0) {
		state.playMode = PM_GOAL_KICK_L;
	}
	else if (std::strncmp(referee, "goal_kick_r", 11) == 0) {
		state.playMode = PM_GOAL_KICK_R;
	}
	else if (std::strncmp(referee, "corner_kick_l", 13) == 0) {
		state.playMode = PM_CORNER_KICK_L;
	}
	else if (std::strncmp(referee, "corner_kick_r", 13) == 0) {
		state.playMode = PM_CORNER_KICK_R;
	}
	else if (std::strncmp(referee, "kick_in_l", 9) == 0) {
		state.playMode = PM_KICK_IN_L;
	}
	else if (std::strncmp(referee, "kick_in_r", 9) == 0) {
		state.playMode = PM_KICK_IN_R;
	}
	else if (std::strncmp(referee, "free_kick_l", 11) == 0
			|| std::strncmp(referee, "indirect_free_kick_l", 20) == 0) {
		state.playMode = PM_FREE_KICK_L;
	}
	else if (std::strncmp(referee, "free_kick_r", 11) == 0
			|| std::strncmp(referee, "indirect_free_kick_r", 20) == 0) {
		state.playMode = PM_FREE_KICK_R;
	}
	else {
		state.playMode = PM_OTHER;
	}
	if (isRestartHoldMode() || isStructuredRestartMode(state.playMode)) {
		clearTransientAttackState();
		lastSeeCycle = -1;
	}
	printf("referee_parse: playMode=%d\n", static_cast<int>(state.playMode));
	return true;
}

void handleHearMessage(const char *msg, int cycle) {
	if (std::strstr(msg, "referee ") != 0) {
		const char *ownCatch = iSide == 2
				? "goalie_catch_ball_r" : "goalie_catch_ball_l";
		const char *ownCatchFault = iSide == 2
				? "catch_fault_r" : "catch_fault_l";
		if (std::strstr(msg, ownCatchFault) != 0) {
			goalieCatchSuccess = false;
			goalieState = GS_RETURNING;
			goalieCatchAttemptCycle = -100;
		}
		else if (std::strstr(msg, ownCatch) != 0) {
			goalieCatchSuccess = true;
			goalieCatchCycle = cycle;
			goalieState = GS_CLEARING;
		}
		parseRefereeMessage(msg, gMatchState);
		if (isOwnKickOffMode()) {
			lastOwnKickOffCycle = cycle;
		}
		return;
	}
	if (std::strstr(msg, "self") != 0) {
		return;
	}

	const char *quoted = std::strchr(msg, '"');
	if (quoted == 0) {
		return;
	}

	if (quoted[1] == 'p') {
		const int target = quoted[2] - '0';
		if (target >= 2 && target <= 5) {
			double sourceDirection = 0.0;
			if (std::sscanf(msg, "(hear %*d %lf", &sourceDirection) == 1) {
				lastPassSourceDirection = sourceDirection;
			}
			lastPassTargetUnum = target;
			lastPassCycle = cycle;
			if (kickoffSecondTouchLocked(cycle)) {
				releaseKickoffSecondTouchLock();
			}
			printf("pass_heard: cycle=%d target=%d self=%d\n",
					cycle, target, iPlayerId);
		}
		return;
	}

	if (quoted[1] == 't') {
		const int toucher = quoted[2] - '0';
		if (toucher >= 2 && toucher <= 5 && toucher != iPlayerId
				&& kickoffSecondTouchLocked(cycle)) {
			releaseKickoffSecondTouchLock();
		}
		return;
	}

	if (quoted[1] != 'c') {
		return;
	}

	const int claimed = quoted[2] - '0';
	if (claimed < 2 || claimed > 5 || claimed == iPlayerId) {
		return;
	}
	lastHeardChaseUnum = claimed;
	lastHeardChaseCycle = cycle;
}

bool parseSenseBodyMessage(const char *msg) {
	int cycle = -1;
	if (std::sscanf(msg, "(sense_body %d", &cycle) != 1) {
		return false;
	}

	const char *stamina = std::strstr(msg, "(stamina ");
	if (stamina == 0) {
		return false;
	}

	double value = 0.0;
	double effort = 1.0;
	if (std::sscanf(stamina, "(stamina %lf %lf", &value, &effort) < 1) {
		return false;
	}

	gBodyState.hasStamina = true;
	gBodyState.stamina = value;
	gBodyState.effort = effort;
	const char *speed = std::strstr(msg, "(speed ");
	if (speed != 0 && std::sscanf(speed, "(speed %lf",
			&gBodyState.speed) == 1) {
		gBodyState.hasSpeed = true;
	}
	const char *headAngle = std::strstr(msg, "(head_angle ");
	if (headAngle != 0) {
		std::sscanf(headAngle, "(head_angle %lf", &gBodyState.headAngle);
	}
	gBodyState.cycle = cycle;
	printf("body_parse: cycle=%d stamina=%.1f effort=%.2f speed=%.2f head=%.1f\n",
			gBodyState.cycle, gBodyState.stamina, gBodyState.effort,
			gBodyState.speed, gBodyState.headAngle);
	return true;
}

int limitPower(int value, int maximum) {
	return value > maximum ? maximum : value;
}

int staminaDashPower(int basePower, bool chasing) {
	if (!gBodyState.hasStamina) {
		return basePower;
	}
	if (gBodyState.effort < 0.65) {
		return chasing ? limitPower(basePower, 65) : limitPower(basePower, 25);
	}
	if (gBodyState.stamina < 1800.0) {
		return chasing ? limitPower(basePower, 55) : limitPower(basePower, 20);
	}
	if (gBodyState.stamina < 3500.0) {
		return chasing ? limitPower(basePower, 75) : limitPower(basePower, 35);
	}
	if (gBodyState.stamina < 5200.0) {
		return chasing ? limitPower(basePower, 90) : limitPower(basePower, 45);
	}
	return basePower;
}

bool findTeammate(const VisualState &state, int unum, SeenPlayer &player) {
	for (int i = 0; i < state.teammateCount; ++i) {
		if (state.teammates[i].unum == unum) {
			player = state.teammates[i];
			return true;
		}
	}
	return false;
}

bool hasNearbyOpponent(const VisualState &state, double distanceLimit) {
	for (int i = 0; i < state.opponentCount; ++i) {
		if (state.opponents[i].distance <= distanceLimit) {
			return true;
		}
	}
	return false;
}

bool findNearestOpponent(const VisualState &state, SeenPlayer &opponent) {
	if (state.opponentCount <= 0) {
		return false;
	}
	opponent = state.opponents[0];
	for (int i = 1; i < state.opponentCount; ++i) {
		if (state.opponents[i].distance < opponent.distance) {
			opponent = state.opponents[i];
		}
	}
	return true;
}

bool hasOtherPlayerTouchingBall(const VisualState &state) {
	if (!state.hasBall) {
		return false;
	}
	for (int i = 0; i < state.teammateCount; ++i) {
		const SeenPlayer &player = state.teammates[i];
		const double playerToBall = estimateDistance(
				player.distance, state.ballDistance,
				player.direction, state.ballDirection);
		if (playerToBall <= 0.8) {
			return true;
		}
	}
	for (int i = 0; i < state.opponentCount; ++i) {
		const SeenPlayer &player = state.opponents[i];
		const double playerToBall = estimateDistance(
				player.distance, state.ballDistance,
				player.direction, state.ballDirection);
		if (playerToBall <= 0.8) {
			return true;
		}
	}
	return false;
}

bool isLikelyFirstToBall(const VisualState &state, double margin) {
	if (!state.hasBall) {
		return false;
	}
	for (int i = 0; i < state.teammateCount; ++i) {
		const SeenPlayer &player = state.teammates[i];
		const double playerToBall = estimateDistance(
				player.distance, state.ballDistance,
				player.direction, state.ballDirection);
		if (playerToBall + margin < state.ballDistance) {
			return false;
		}
	}
	for (int i = 0; i < state.opponentCount; ++i) {
		const SeenPlayer &player = state.opponents[i];
		const double playerToBall = estimateDistance(
				player.distance, state.ballDistance,
				player.direction, state.ballDirection);
		if (playerToBall + margin < state.ballDistance) {
			return false;
		}
	}
	return true;
}

bool hasOpponentInLane(const VisualState &state, double laneDirection,
		double distanceLimit, double angleLimit) {
	for (int i = 0; i < state.opponentCount; ++i) {
		const double diff = normalizeAngle(state.opponents[i].direction
				- laneDirection);
		if (state.opponents[i].distance <= distanceLimit
				&& absDouble(diff) <= angleLimit) {
			return true;
		}
	}
	return false;
}

double lanePressure(const VisualState &state, double laneDirection) {
	double pressure = 0.0;
	for (int i = 0; i < state.opponentCount; ++i) {
		const double diff = absDouble(normalizeAngle(
				state.opponents[i].direction - laneDirection));
		if (state.opponents[i].distance > 16.0 || diff > 32.0) {
			continue;
		}
		pressure += (17.0 - state.opponents[i].distance)
				+ (32.0 - diff) * 0.25;
	}
	return pressure;
}

double chooseOpenAdvanceDirection(const VisualState &state,
		double baseDirection, int playerId) {
	const double preferredSide = playerId == 4 ? -24.0 : 24.0;
	double bestDirection = normalizeAngle(baseDirection);
	double bestPressure = lanePressure(state, bestDirection);
	const double offsets[4] = {
		preferredSide, -preferredSide, preferredSide * 1.6,
		-preferredSide * 1.6
	};
	for (int i = 0; i < 4; ++i) {
		const double candidate = normalizeAngle(baseDirection + offsets[i]);
		const double pressure = lanePressure(state, candidate)
				+ absDouble(offsets[i]) * 0.04;
		if (pressure + 0.5 < bestPressure) {
			bestPressure = pressure;
			bestDirection = candidate;
		}
	}
	return bestDirection;
}

bool shouldChaseBall(const VisualState &state, int playerId) {
	gLastChaseReason = "none";
	// 若当前帧看不到球，用3帧内缓存维持追球意图
	double ballDist, ballDir;
	if (state.hasBall) {
		ballDist = state.ballDistance;
		ballDir  = state.ballDirection;
	}
	else if (gBallTrack.hasSample
			&& state.cycle - gBallTrack.lastCycle <= 3) {
		ballDist = gBallTrack.lastDistance;
		ballDir  = gBallTrack.lastDirection;
	}
	else {
		gLastChaseReason = "no_ball";
		return false;
	}
	if ((playerId == 4 || playerId == 5)
			&& state.cycle - lastOwnTouchCycle >= 0
			&& state.cycle <= ownTouchFollowUntilCycle) {
		gLastChaseReason = "follow_touch";
		return true;
	}

	bool sawComparableTeammate = false;
	bool sawClaimingTeammate = false;
	double claimingTeammateBallDistance = 1000.0;
	for (int i = 0; i < state.teammateCount; ++i) {
		const SeenPlayer &tm = state.teammates[i];
		if (tm.unum < 2 || tm.unum > 5 || tm.unum == playerId) {
			continue;
		}
		sawComparableTeammate = true;
		const double est = estimateDistance(tm.distance, ballDist,
				tm.direction, ballDir);
		if (tm.unum == lastHeardChaseUnum) {
			sawClaimingTeammate = true;
			claimingTeammateBallDistance = est;
		}
		if (est + 1.5 < ballDist) {
			gLastChaseReason = "closer";
			return false;
		}
		if (absDouble(est - ballDist) <= 1.5
				&& (chaseRoleRank(tm.unum) > chaseRoleRank(playerId)
						|| (chaseRoleRank(tm.unum) == chaseRoleRank(playerId)
								&& tm.unum < playerId))) {
			gLastChaseReason = "closer";
			return false;
		}
	}

	// 门前危急：允许最近后卫接管，即使角色优先级低于 4/5。
	if (state.hasOwnGoal && ballDist < 8.0
			&& state.ownGoalDistance < 25.0
			&& (playerId == 2 || playerId == 3)) {
		bool closerThanOtherDefender = true;
		for (int i = 0; i < state.teammateCount; ++i) {
			const SeenPlayer &tm = state.teammates[i];
			if (tm.unum != 2 && tm.unum != 3) continue;
			if (tm.unum == playerId) continue;
			const double est = estimateDistance(tm.distance, ballDist,
					tm.direction, ballDir);
			if (est + 0.5 < ballDist) {
				closerThanOtherDefender = false;
				break;
			}
		}
		if (closerThanOtherDefender) {
			gLastChaseReason = "emergency";
			return true;
		}
	}

	const bool hearFresh = lastHeardChaseUnum >= 2
			&& lastHeardChaseUnum <= 5
			&& state.cycle - lastHeardChaseCycle >= 0
			&& state.cycle - lastHeardChaseCycle <= 4;
	if (hearFresh && lastHeardChaseUnum != playerId) {
		const bool claimantIsCloser = sawClaimingTeammate
				&& claimingTeammateBallDistance + 1.0 < ballDist;
		const bool claimantHasPriority = !sawClaimingTeammate
				&& chaseRoleRank(lastHeardChaseUnum)
						> chaseRoleRank(playerId);
		if (claimantIsCloser || claimantHasPriority) {
			gLastChaseReason = "hear";
			return false;
		}
	}

	// With no shared observation, use a deterministic owner. Otherwise both
	// forwards claim, both yield on hearing the other, and repeatedly oscillate.
	if (!sawComparableTeammate && !hearFresh) {
		if (state.hasOwnGoal && ballDist < 25.0
				&& state.ownGoalDistance < 35.0
				&& playerId == 2) {
			gLastChaseReason = "defender_zone";
			return true;
		}
		if (playerId == 5) {
			gLastChaseReason = "primary_forward";
			return true;
		}
		gLastChaseReason = "wait_info";
		return false;
	}

	gLastChaseReason = "self";
	return true;
}

const char * attackGoalName() {
	return iSide == 2 ? "(goal l)" : "(goal r)";
}

const char * attackGoalShortName() {
	return iSide == 2 ? "(g l)" : "(g r)";
}

const char * ownGoalName() {
	return iSide == 2 ? "(goal r)" : "(goal l)";
}

const char * ownGoalShortName() {
	return iSide == 2 ? "(g r)" : "(g l)";
}

void formationPosition(int playerId, double &x, double &y) {
	// 开球落位一律使用左队坐标系。
	// 右队的 (move) 由服务器自动镜像，客户端不可再取反。
	static const double formationX[6] = { 0.0, -48.0, -36.0, -36.0, -8.0, -8.0 };
	static const double formationY[6] = { 0.0, 0.0, -10.0, 10.0, -7.0, 7.0 };

	if (playerId < 1 || playerId > 5) playerId = 5;
	x = formationX[playerId];
	y = formationY[playerId];
}

void restartFormationPosition(int playerId, double &x, double &y) {
	formationPosition(playerId, x, y);
	if (isPreparingOwnKickOff() && playerId == 4) {
		x = -0.8;
		y = 0.0;
	}
}

class Client {
private:
	rcss::net::Addr M_dest;  // 1)global scope(全局作用域符），用法（::name)
	rcss::net::UDPSocket M_socket;  // 2)class scope(类作用域符），用法(class::name)
	rcss::net::SocketStreamBuf * M_socket_buf; // 3)namespace scope(命名空间作用域符），用法(namespace::name)
	rcss::gz::gzstreambuf * M_gz_buf;
	std::ostream * M_transport;
	int M_comp_level;
	bool M_clean_cycle;
	bool M_setup_complete;

#ifdef HAVE_LIBZ
	Decompressor M_decomp;
#endif

	Client(); // 构造函数
	Client(const Client &);
	Client & operator=(const Client &); //运算符重载
public: // 构造函数初始化列表以一个冒号开始，接着是以逗号分隔的数据成员列表，每个数据成员后面跟一个放在括号中的初始化式。
	Client(const std::string & server, const int port) :
		M_dest(port), M_socket(), M_socket_buf(NULL), M_gz_buf(NULL),
				M_transport(NULL), M_comp_level(-1), M_clean_cycle(true),
				M_setup_complete(false) {
		M_dest.setHost(server);
		open(); // 打开
		bind(); // 绑定

		M_socket_buf->setEndPoint(M_dest); // 指针访问结构或者类的成员，必须使用 -> 运算符
	}

	virtual ~Client() { // 析构函数
		close(); // 关闭
	}

	bool sendCmd(char *command) {
		int len;
		len = strlen(command) + 1; // strlen c++字符串操作函数
		printf("command:%s\n", command);
		M_transport->write(command, len);
		M_transport->flush();
		if (!M_transport->good()) {
			printf("error send socket\n");
			return false;
		}
		double turnMoment = 0.0;
		if (hasAttackDirectionEstimate
				&& std::sscanf(command, "(turn %lf", &turnMoment) == 1) {
			turnMoment = clampDouble(turnMoment, -180.0, 180.0);
			const double speed = gBodyState.hasSpeed
					? gBodyState.speed : 0.0;
			const double actualTurn = turnMoment / (1.0 + 5.0 * speed);
			attackDirectionEstimate = normalizeAngle(
					attackDirectionEstimate - actualTurn);
		}
		return true;
	}

	void run() {
		char command[128];

		if (iSide != 1 && iSide != 2) {
			return;
		}

		if (!teamNameFromArg) {
			teamName = (iSide == 1) ? "team1" : "team2";
		}

		if (iPlayerId == 1) {
			sprintf(command, "(init %.40s (version 13.2) (goalie))",
					teamName.c_str());
		}
		else {
			sprintf(command, "(init %.40s (version 13.2))",
					teamName.c_str());
		}
		if (!sendCmd(command))
			return;

		messageLoop(); // 函数  消息循环 踢球中。。。
	}

private:
	void setupAfterInit() {
		if (M_setup_complete) {
			return;
		}
		M_setup_complete = true;

		char command[128];
		double startX = 0.0;
		double startY = 0.0;
		formationPosition(iPlayerId, startX, startY);

		sprintf(command, "(move %.1f %.1f)", startX, startY);
		if (!sendCmd(command)) return;

		sprintf(command, "(change_view narrow high)");
		if (!sendCmd(command)) return;

		sprintf(command, "(synch_see)");
		sendCmd(command);
	}

	int open() {
		if (M_socket.open()) {
			if (M_socket.setNonBlocking() < 0) {
				std::cerr << __FILE__ << ": " << __LINE__
						<< ": Error setting socket non-blocking: " << strerror(
						errno) << std::endl;
				M_socket.close();
				return -1;
			}
		} else {
			std::cerr << __FILE__ << ": " << __LINE__
					<< ": Error opening socket: " << strerror(errno)
					<< std::endl;
			M_socket.close();
			return -1;
		}

		M_socket_buf = new rcss::net::SocketStreamBuf(M_socket); // new 动态内存
		M_transport = new std::ostream(M_socket_buf);
		return 0;
	}

	bool bind() {
		if (!M_socket.bind(rcss::net::Addr())) { // rcss::net::Addr() 调用无参构造函数创建一个对象，再把对象传给函数
			std::cerr << __FILE__ << ": " << __LINE__
					<< ": Error connecting socket" << std::endl;
			M_socket.close();
			return false;
		}
		return true;
	}

	void close() {
		M_socket.close();

		if (M_transport) {
			delete M_transport;
			M_transport = NULL;
		}

		if (M_gz_buf) {
			delete M_gz_buf;
			M_gz_buf = NULL;
		}

		if (M_socket_buf) {
			delete M_socket_buf;
			M_socket_buf = NULL;
		}
	}

	int setCompression(int level) {
#ifdef HAVE_LIBZ
		if ( level >= 0 )
		{
			if ( ! M_gz_buf )
			{
				M_gz_buf = new rcss::gz::gzstreambuf( *M_socket_buf );
			}
			M_gz_buf->setLevel( level );
			M_transport->rdbuf( M_gz_buf );
		}
		else
		{
			M_transport->rdbuf( M_socket_buf );
		}
		return M_comp_level = level;
#endif
		(void)level;
		return M_comp_level = -1;
	}

	void processMsg(char * msg, const size_t len) { // size_t; cstdlib.h提供的类型之一：size_t, wchar_t, div_t, ldiv_t, lldiv_t
#ifdef HAVE_LIBZ
		if ( M_comp_level >= 0 )
		{
			M_decomp.decompress( msg, len, Z_SYNC_FLUSH );
			char * out;
			int size;
			M_decomp.getOutput( out, size );
			if ( size > 0 )
			{
				parseMsg( out, size );
			}
		}
		else
#endif
		{
			parseMsg(msg, len); // 函数 消息解析
		}
	}

	int readObjectInfo(const char *msg, const char *name,
			double &distance, double &direction) {
		const char *object = strstr(msg, name);
		if (object == 0) return 0;
		return std::sscanf(object + strlen(name), " %lf %lf",
				&distance, &direction) == 2;
	}

	int readBallInfo(const char *msg, double &distance, double &direction,
			double &distanceChange, double &directionChange) {
		const char *object = std::strstr(msg, "(ball)");
		const char *name = "(ball)";
		if (object == 0) {
			object = std::strstr(msg, "(b)");
			name = "(b)";
		}
		if (object == 0) {
			return 0;
		}
		return std::sscanf(object + std::strlen(name),
				" %lf %lf %lf %lf", &distance, &direction,
				&distanceChange, &directionChange);
	}

	int readAttackGoalInfo(const char *msg,
			double &distance, double &direction) {
		return readObjectInfo(msg, attackGoalName(), distance, direction)
				|| readObjectInfo(msg, attackGoalShortName(), distance, direction);
	}

	int readOwnGoalInfo(const char *msg,
			double &distance, double &direction) {
		return readObjectInfo(msg, ownGoalName(), distance, direction)
				|| readObjectInfo(msg, ownGoalShortName(), distance, direction);
	}

	void collectPlayersFromSee(const char *msg, const char *tag,
			VisualState &state) {
		const size_t tagLen = std::strlen(tag);
		const char *cursor = msg;
		while ((cursor = std::strstr(cursor, tag)) != 0) {
			char seenTeam[64];
			int unum = 0;
			double distance = 0.0;
			double direction = 0.0;
			seenTeam[0] = '\0';
			cursor += tagLen;

			const char *nameBegin = cursor;
			while (*nameBegin == ' ') ++nameBegin;
			const char *nameEnd = nameBegin;
			if (*nameBegin == '"') {
				++nameBegin;
				nameEnd = std::strchr(nameBegin, '"');
			}
			else {
				while (*nameEnd != '\0' && *nameEnd != ' '
						&& *nameEnd != ')') {
					++nameEnd;
				}
			}
			if (nameEnd == 0 || nameEnd <= nameBegin) {
				continue;
			}

			size_t nameLength = static_cast<size_t>(nameEnd - nameBegin);
			if (nameLength >= sizeof(seenTeam)) {
				nameLength = sizeof(seenTeam) - 1;
			}
			std::memcpy(seenTeam, nameBegin, nameLength);
			seenTeam[nameLength] = '\0';

			const char *numberBegin = *nameEnd == '"' ? nameEnd + 1 : nameEnd;
			while (*numberBegin == ' ') ++numberBegin;
			if (std::sscanf(numberBegin, "%d", &unum) != 1 || unum < 1) {
				continue;
			}

			const char *objectEnd = std::strchr(numberBegin, ')');
			if (objectEnd == 0
					|| std::sscanf(objectEnd + 1, " %lf %lf",
							&distance, &direction) != 2) {
				continue;
			}

			SeenPlayer seen;
			seen.unum = unum;
			seen.distance = distance;
			seen.direction = normalizeAngle(direction + gBodyState.headAngle);

			if (!teamName.empty() && std::strcmp(seenTeam, teamName.c_str()) == 0) {
				if (unum >= 2 && unum <= 5 && unum != iPlayerId
						&& state.teammateCount < 4) {
					state.teammates[state.teammateCount++] = seen;
				}
			}
			else if (state.opponentCount < 5) {
				state.opponents[state.opponentCount++] = seen;
			}
		}
	}

	bool parseVisualState(const char *msg, VisualState &state) {
		std::memset(&state, 0, sizeof(state));
		if (std::sscanf(msg, "(see %d", &state.cycle) != 1) {
			return false;
		}

		const int ballFields = readBallInfo(msg, state.ballDistance,
				state.ballDirection, state.ballDistanceChange,
				state.ballDirectionChange);
		state.hasBall = ballFields >= 2;
		state.hasBallMotion = ballFields >= 4;
		state.hasOwnGoal = readOwnGoalInfo(msg, state.ownGoalDistance,
				state.ownGoalDirection) != 0;
		state.hasAttackGoal = readAttackGoalInfo(msg, state.attackGoalDistance,
				state.attackGoalDirection) != 0;

		if (state.hasBall) {
			state.ballNeckDirection = state.ballDirection;
			state.ballDirection = normalizeAngle(
					state.ballDirection + gBodyState.headAngle);
		}
		if (state.hasOwnGoal) {
			state.ownGoalDirection = normalizeAngle(
					state.ownGoalDirection + gBodyState.headAngle);
		}
		if (state.hasAttackGoal) {
			state.attackGoalDirection = normalizeAngle(
					state.attackGoalDirection + gBodyState.headAngle);
		}
		if (state.hasAttackGoal) {
			hasAttackDirectionEstimate = true;
			attackDirectionEstimate = state.attackGoalDirection;
			attackDirectionLandmarkCycle = state.cycle;
		}
		else if (state.hasOwnGoal) {
			hasAttackDirectionEstimate = true;
			attackDirectionEstimate = normalizeAngle(
					state.ownGoalDirection + 180.0);
			attackDirectionLandmarkCycle = state.cycle;
		}

		collectPlayersFromSee(msg, "(player ", state);
		collectPlayersFromSee(msg, "(p ", state);
		return true;
	}

	void makeTurnOrDashCommand(double direction, int power, char *command,
			double turnThreshold = 18.0) {
		if (absDouble(direction) > turnThreshold) {
			sprintf(command, "(turn %.1f)", direction);
		}
		else {
			sprintf(command, "(dash %d 0)", power);
		}
	}

	void appendBallNeckCommand(const VisualState &visual, char *command,
			size_t capacity) {
		if (!visual.hasBall) {
			return;
		}
		double moment = visual.ballNeckDirection;
		double bodyTurn = 0.0;
		if (std::sscanf(command, "(turn %lf", &bodyTurn) == 1) {
			moment = normalizeAngle(moment - bodyTurn);
		}
		if (moment > 90.0) moment = 90.0;
		if (moment < -90.0) moment = -90.0;
		char neck[48];
		sprintf(neck, "(turn_neck %.1f)", moment);
		if (std::strlen(command) + std::strlen(neck) + 1 < capacity) {
			std::strcat(command, neck);
		}
	}

	void appendCenterNeckCommand(char *command, size_t capacity) {
		if (std::strstr(command, "(turn_neck ") != 0) {
			return;
		}
		double moment = -gBodyState.headAngle;
		if (moment > 90.0) moment = 90.0;
		if (moment < -90.0) moment = -90.0;
		char neck[48];
		sprintf(neck, "(turn_neck %.1f)", moment);
		if (std::strlen(command) + std::strlen(neck) + 1 < capacity) {
			std::strcat(command, neck);
		}
	}

	void appendSayCommand(char *command, size_t capacity,
			const char *message) {
		if (std::strstr(command, "(say ") != 0) {
			return;
		}
		char say[32];
		sprintf(say, "(say \"%.8s\")", message);
		if (std::strlen(command) + std::strlen(say) + 1 < capacity) {
			std::strcat(command, say);
		}
	}

	int structuredRestartTaker() const {
		if ((gMatchState.playMode == PM_FREE_KICK_L
				|| gMatchState.playMode == PM_FREE_KICK_R)
				&& goalieCatchSuccess) {
			return 1;
		}
		if (gMatchState.playMode == PM_GOAL_KICK_L
				|| gMatchState.playMode == PM_GOAL_KICK_R) {
			return 2;
		}
		if (gMatchState.playMode == PM_CORNER_KICK_L
				|| gMatchState.playMode == PM_CORNER_KICK_R) {
			return 5;
		}
		return 4;
	}

	void makeStructuredRestartKick(const VisualState &visual,
			char *command) {
		if (isOwnGoalKickMode() && iPlayerId == 2) {
			double direction = 180.0;
			if (visual.hasAttackGoal) {
				direction = visual.attackGoalDirection;
			}
			else if (visual.hasOwnGoal) {
				direction = normalizeAngle(visual.ownGoalDirection + 180.0);
			}
			sprintf(command, "(kick 100 %.1f)", direction);
			return;
		}

		const int receiverUnum = iPlayerId == 4 ? 5 : 4;
		SeenPlayer receiver;
		if (findTeammate(visual, receiverUnum, receiver)
				&& receiver.distance >= 4.0 && receiver.distance <= 30.0
				&& !hasOpponentInLane(visual, receiver.direction,
						receiver.distance - 1.0, 12.0)) {
			const int passPower = kickPowerForDistance(receiver.distance + 2.0);
			sprintf(command, "(kick %d %.1f)(say \"p%d\")",
					passPower, receiver.direction, receiverUnum);
			return;
		}

		double direction = 0.0;
		if (visual.hasAttackGoal) {
			direction = visual.attackGoalDirection;
		}
		else if (visual.hasOwnGoal) {
			direction = normalizeAngle(visual.ownGoalDirection + 180.0);
		}
		direction = chooseOpenAdvanceDirection(visual, direction, iPlayerId);
		sprintf(command, "(kick 100 %.1f)", direction);
	}

	bool handleStructuredRestart(const VisualState &visual, int cycle) {
		if (!isStructuredRestartMode(gMatchState.playMode)) {
			return false;
		}

		char command[160];
		const bool ownRestart = isOwnStructuredRestartMode();
		const int taker = structuredRestartTaker();
		if (ownRestart && iPlayerId == taker) {
			if (!visual.hasBall) {
				sprintf(command, "(turn %d)", iPlayerId % 2 == 0 ? 30 : -30);
			}
			else if (visual.ballDistance > 0.98) {
				makeTurnOrDashCommand(visual.ballDirection,
						staminaDashPower(90, true), command);
				if (std::strncmp(command, "(dash", 5) == 0) {
					appendBallNeckCommand(visual, command, sizeof(command));
				}
			}
			else if (cycle - lastSetPieceKickCycle >= 3) {
				makeStructuredRestartKick(visual, command);
				appendBallNeckCommand(visual, command, sizeof(command));
				lastSetPieceKickCycle = cycle;
				if (iPlayerId == 1) {
					goalieCatchSuccess = false;
					goalieState = GS_RETURNING;
					goalieCatchAttemptCycle = -100;
				}
				if (iPlayerId == 2) {
					lastDefenderKickCycle = cycle;
				}
			}
			else {
				sprintf(command, "(turn 0)");
				appendBallNeckCommand(visual, command, sizeof(command));
			}
			sendCmd(command);
			return true;
		}

		if (iPlayerId == 1) {
			if (!makeGoalkeeperGuardCommand(visual, command)) {
				if (visual.hasBall && absDouble(visual.ballDirection) > 10.0) {
					sprintf(command, "(turn %.1f)", visual.ballDirection);
				}
				else {
					sprintf(command, "(turn 0)");
					appendBallNeckCommand(visual, command, sizeof(command));
				}
			}
		}
		else if (!ownRestart && iPlayerId >= 2 && iPlayerId <= 5) {
			if (visual.hasBall && absDouble(visual.ballDirection) > 10.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(turn 0)");
				appendBallNeckCommand(visual, command, sizeof(command));
			}
		}
		else if (iPlayerId == 2 || iPlayerId == 3) {
			makeDefenderShapeCommand(visual, true, command);
		}
		else if (visual.hasBall) {
			makeForwardSupportCommand(visual, command);
		}
		else {
			sprintf(command, "(turn %d)", iPlayerId == 4 ? 25 : -25);
		}
		appendCenterNeckCommand(command, sizeof(command));
		sendCmd(command);
		return true;
	}

	bool handleKickOff(const VisualState &visual, int cycle) {
		if (gMatchState.playMode != PM_KICK_OFF_L
				&& gMatchState.playMode != PM_KICK_OFF_R) {
			return false;
		}

		char command[160];
		if (kickoffSecondTouchLocked(cycle)) {
			if (visual.hasBall) {
				const double supportDirection = normalizeAngle(
						visual.ballDirection - 55.0);
				if (absDouble(supportDirection) > 18.0) {
					sprintf(command, "(turn %.1f)", supportDirection);
				}
				else {
					sprintf(command, "(dash %d)",
							staminaDashPower(72, false));
					appendBallNeckCommand(visual, command, sizeof(command));
				}
			}
			else {
				sprintf(command, "(turn -25)");
			}
		}
		else if (isOwnKickOffMode() && iPlayerId == 4) {
			if (!visual.hasBall) {
				sprintf(command, "(turn 30)");
			}
			else if (visual.ballDistance > 0.95) {
				makeTurnOrDashCommand(visual.ballDirection, 100, command);
				if (std::strncmp(command, "(dash", 5) == 0) {
					appendBallNeckCommand(visual, command, sizeof(command));
				}
			}
			else {
				double receiverDistance = 10.2;
				double direction = visual.hasAttackGoal
						? normalizeAngle(visual.attackGoalDirection + 136.0)
						: 136.0;
				SeenPlayer receiver;
				if (findTeammate(visual, 5, receiver)) {
					receiverDistance = receiver.distance;
					direction = receiver.direction;
				}
				const int passPower = kickPowerForDistance(receiverDistance);
				sprintf(command, "(kick %d %.1f)(say \"p5\")",
						passPower, direction);
				appendBallNeckCommand(visual, command, sizeof(command));
				markOwnTouch(cycle, 0);
				kickoffTouchCycle = cycle;
				kickoffSecondTouchLockUntilCycle = cycle + 12;
				gLastAttackAction = "kickoff_touch";
			}
		}
		else if (visual.hasBall) {
			if (absDouble(visual.ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(turn 0)");
			}
		}
		else {
			sprintf(command, "(turn %d)", iPlayerId % 2 == 0 ? 25 : -25);
		}

		sendCmd(command);
		return true;
	}

	void makeGoalkeeperReturnCommand(const VisualState &visual, char *command) {
		if (visual.hasOwnGoal) {
			if (visual.ownGoalDistance > 5.5) {
				makeTurnOrDashCommand(visual.ownGoalDirection, 75, command);
			}
			else if (visual.ownGoalDistance < 2.5) {
				const double awayFromGoal = normalizeAngle(
						visual.ownGoalDirection + 180.0);
				makeTurnOrDashCommand(awayFromGoal, 38, command);
			}
			else if (visual.hasBall
					&& absDouble(visual.ballDirection) > 10.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
				goalieAdvanceSteps = 0;
			}
			else {
				sprintf(command, "(turn 0)");
				appendBallNeckCommand(visual, command, 160);
				goalieAdvanceSteps = 0;
			}
		}
		else {
			sprintf(command, "(turn 60)");
		}
	}

	bool makeGoalkeeperGuardCommand(const VisualState &visual,
			char *command) {
		// Relative ball and goal vectors are comparable only when observed in
		// the same frame. Cached vectors rotate with the player and cannot be
		// subtracted safely without a global pose estimate.
		if (!visual.hasBall || !visual.hasOwnGoal) {
			return false;
		}
		const double ballDist = visual.ballDistance;
		const double ballDir = visual.ballDirection;
		const double ownGoalDist = visual.ownGoalDistance;
		const double ownGoalDir = visual.ownGoalDirection;

		const double radians = 3.141592653589793 / 180.0;
		const double goalX = ownGoalDist * std::cos(ownGoalDir * radians);
		const double goalY = ownGoalDist * std::sin(ownGoalDir * radians);
		const double ballX = ballDist
				* std::cos(ballDir * radians);
		const double ballY = ballDist
				* std::sin(ballDir * radians);
		const double lineX = ballX - goalX;
		const double lineY = ballY - goalY;
		const double ballToGoal = std::sqrt(lineX * lineX + lineY * lineY);
		if (ballToGoal < 0.5) {
			return false;
		}

		const double guardDepth = ballToGoal < 8.0 ? 4.0 : 5.2;
		const double targetX = goalX + lineX * guardDepth / ballToGoal;
		const double targetY = goalY + lineY * guardDepth / ballToGoal;
		const double targetDistance = std::sqrt(
				targetX * targetX + targetY * targetY);
		if (targetDistance <= 0.65) {
			if (absDouble(ballDir) > 8.0) {
				sprintf(command, "(turn %.1f)", ballDir);
			}
			else {
				sprintf(command, "(turn 0)");
				appendBallNeckCommand(visual, command, 160);
			}
			return true;
		}

		const double targetDirection = std::atan2(targetY, targetX) / radians;
		makeTurnOrDashCommand(targetDirection,
				staminaDashPower(68, true), command, 100.0);
		if (std::strncmp(command, "(dash", 5) == 0) {
			appendBallNeckCommand(visual, command, 160);
		}
		return true;
	}

	void handleGoalkeeper(const VisualState &visual, int cycle) {
		char command[160];

		if (handleStructuredRestart(visual, cycle)) {
			return;
		}

		if (handleKickOff(visual, cycle)) {
			return;
		}

		if (isRestartHoldMode()) {
			double x = 0.0;
			double y = 0.0;
			restartFormationPosition(1, x, y);
			sprintf(command, "(move %.1f %.1f)", x, y);
			goalieAdvanceSteps = 0;
			goalieLastBallCycle = -100;
			goalieCatchSuccess = false;
			goalieState = GS_HOLDING;
			goalieCatchAttemptDistance = 1000.0;
			sendCmd(command);
			return;
		}

		if (visual.hasBall) {
			goalieLastBallCycle = cycle;
			goalieLastBallDirection = visual.ballDirection;
			goalieLastBallDistance = visual.ballDistance;
		}
		if (goalieState == GS_RETURNING && visual.hasOwnGoal
				&& visual.ownGoalDistance <= 5.5) {
			goalieAdvanceSteps = 0;
			goalieState = GS_HOLDING;
		}

		double clearDirection = 0.0;
		if (visual.hasAttackGoal) {
			clearDirection = visual.attackGoalDirection;
		}
		else if (visual.hasOwnGoal) {
			clearDirection = normalizeAngle(visual.ownGoalDirection + 180.0);
		}
		clearDirection = chooseOpenAdvanceDirection(
				visual, clearDirection, iPlayerId);

		if (goalieCatchSuccess || goalieState == GS_CLEARING) {
			if (visual.hasBall && visual.ballDistance <= 1.8) {
				sprintf(command, "(kick 100 %.1f)", clearDirection);
				appendBallNeckCommand(visual, command, sizeof(command));
				goalieCatchSuccess = false;
				goalieState = GS_RETURNING;
				goalieCatchAttemptCycle = -100;
				sendCmd(command);
				return;
			}
			if (cycle - goalieCatchCycle > 3) {
				goalieCatchSuccess = false;
				goalieState = GS_RETURNING;
			}
		}

		const double ballToOwnGoal = visual.hasOwnGoal
				? estimateDistance(visual.ballDistance,
						visual.ownGoalDistance, visual.ballDirection,
						visual.ownGoalDirection)
				: 1000.0;
		if (visual.hasBall && visual.hasOwnGoal) {
			goalieLastConfirmedAreaCycle = cycle;
			goalieLastBallToOwnGoal = ballToOwnGoal;
		}
		const bool catchAreaConfirmed = visual.hasOwnGoal
				? ballToOwnGoal <= 16.0
				: goalieAdvanceSteps <= 3
						|| (cycle - goalieLastConfirmedAreaCycle <= 5
								&& goalieLastBallToOwnGoal <= 16.5);
		const bool fastApproachingShot = visual.hasBall
				&& gBallTrack.hasMotion
				&& gBallTrack.closingStreak >= 2
				&& gBallTrack.closingRate > 0.25
				&& visual.ballDistance <= 18.0;
		const double catchReach = fastApproachingShot ? 2.1 : 1.6;
		if (goalieState == GS_CATCHING
				&& cycle > goalieCatchAttemptCycle && visual.hasBall) {
			goalieState = GS_CHASING;
		}
		const bool catchRetryReady = cycle - goalieCatchAttemptCycle >= 3
				|| visual.ballDistance + 0.15
						< goalieCatchAttemptDistance;
		if (visual.hasBall && visual.ballDistance <= catchReach
				&& catchAreaConfirmed
				&& catchRetryReady) {
			sprintf(command, "(catch %.1f)", visual.ballDirection);
			goalieCatchAttemptCycle = cycle;
			goalieCatchAttemptDistance = visual.ballDistance;
			goalieState = GS_CATCHING;
			sendCmd(command);
			return;
		}

		if (visual.hasBall && visual.ballDistance <= 1.05) {
			sprintf(command, "(kick 100 %.1f)", clearDirection);
			appendBallNeckCommand(visual, command, sizeof(command));
			goalieState = GS_RETURNING;
			sendCmd(command);
			return;
		}

		if (visual.hasBall && visual.ballDistance <= 2.5
				&& hasNearbyOpponent(visual, 3.0)
				&& cycle - lastTackleCycle >= 10) {
			sprintf(command, "(tackle 100)");
			lastTackleCycle = cycle;
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		if (!visual.hasBall) {
			const int unseenCycles = cycle - goalieLastBallCycle;
			const bool blindCatchReady = cycle - goalieCatchAttemptCycle >= 3
					|| goalieLastBallDistance + 0.15
							< goalieCatchAttemptDistance;
			if (unseenCycles == 1 && goalieLastBallDistance <= 2.2
					&& goalieAdvanceSteps <= 3 && blindCatchReady) {
				sprintf(command, "(catch %.1f)", goalieLastBallDirection);
				goalieCatchAttemptCycle = cycle;
				goalieCatchAttemptDistance = goalieLastBallDistance;
				goalieState = GS_CATCHING;
				sendCmd(command);
				return;
			}
			if (unseenCycles >= 1 && unseenCycles <= 3
					&& (goalieState == GS_CHASING
							|| goalieState == GS_CATCHING)) {
				double continueDirection = goalieLastBallDirection;
				if (gBallTrack.hasMotion) {
					continueDirection += gBallTrack.directionRate * unseenCycles;
				}
				continueDirection = clampDouble(
						continueDirection, -75.0, 75.0);
				makeTurnOrDashCommand(continueDirection,
						staminaDashPower(100, true), command, 100.0);
			}
			else if (goalieState == GS_RETURNING || goalieAdvanceSteps > 0) {
				goalieState = GS_RETURNING;
				makeGoalkeeperReturnCommand(visual, command);
			}
			else if (visual.hasOwnGoal) {
				const double faceFieldDirection = normalizeAngle(
						visual.ownGoalDirection + 180.0);
				if (absDouble(faceFieldDirection) > 10.0) {
					sprintf(command, "(turn %.1f)", faceFieldDirection);
				}
				else {
					const double scanTarget = (cycle / 3) % 2 == 0
							? -65.0 : 65.0;
					const double neckMoment = clampDouble(
							scanTarget - gBodyState.headAngle,
							-180.0, 180.0);
					sprintf(command, "(turn 0)(turn_neck %.1f)",
							neckMoment);
				}
			}
			else {
				const double scanTarget = (cycle / 3) % 2 == 0
						? -65.0 : 65.0;
				const double neckMoment = clampDouble(
						scanTarget - gBodyState.headAngle,
						-180.0, 180.0);
				sprintf(command, "(turn 0)(turn_neck %.1f)", neckMoment);
			}
			appendCenterNeckCommand(command, sizeof(command));
			sendCmd(command);
			return;
		}

		// 1. 球近距离快速逼近或在禁区内：主动冲出拦截
		const bool looseBallInArea = visual.ballDistance <= 7.5
				&& (!visual.hasOwnGoal || ballToOwnGoal <= 17.0);
		const bool emergencyOverride = visual.ballDistance <= 4.5
				&& (fastApproachingShot
						|| !visual.hasOwnGoal || ballToOwnGoal <= 12.0);
		const bool canAdvance = goalieAdvanceSteps < 3
				&& (emergencyOverride
						|| (goalieState != GS_RETURNING
						&& goalieAdvanceSteps < 2
						&& (!visual.hasOwnGoal
								|| visual.ownGoalDistance <= 6.5)));
		if ((fastApproachingShot && visual.ballDistance <= 12.0
					&& canAdvance)
				|| (looseBallInArea && canAdvance
						&& isLikelyFirstToBall(visual, 0.5))) {
			makeTurnOrDashCommand(predictedBallDirection(visual, 2),
					100, command, 100.0);
			if (std::strncmp(command, "(dash", 5) == 0) {
				++goalieAdvanceSteps;
				appendBallNeckCommand(visual, command, sizeof(command));
			}
			goalieState = GS_CHASING;
			sendCmd(command);
			return;
		}
		if (fastApproachingShot && !canAdvance
				&& makeGoalkeeperGuardCommand(visual, command)) {
			goalieState = GS_ALIGNING;
			sendCmd(command);
			return;
		}
		// 2. 离球门太远或需要回防：返回球门
		const bool tooFarFromGoal = visual.hasOwnGoal
				&& visual.ownGoalDistance > 5.8;
		if (goalieState == GS_RETURNING || tooFarFromGoal
				|| (goalieAdvanceSteps > 0
						&& visual.ballDistance > 8.0)) {
			goalieState = GS_RETURNING;
			makeGoalkeeperReturnCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			if (goalieAdvanceSteps == 0 && !tooFarFromGoal) {
				goalieState = GS_HOLDING;
			}
			sendCmd(command);
			return;
		}

		// 3. 离球门太近：后退拉开距离
		if (visual.hasOwnGoal && visual.ownGoalDistance < 3.5) {
			const double awayFromGoal = normalizeAngle(
					visual.ownGoalDirection + 180.0);
			makeTurnOrDashCommand(awayFromGoal, 35, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		// 4. 默认行为：站位在球门线上（球-门连线上），而非原地转身
		if (makeGoalkeeperGuardCommand(visual, command)) {
			goalieState = GS_ALIGNING;
			sendCmd(command);
			return;
		}

		// 5. 最后fallback：如果makeGoalkeeperGuardCommand失败（极罕见），转向球
		goalieState = GS_HOLDING;
		if (absDouble(visual.ballDirection) > 10.0) {
			sprintf(command, "(turn %.1f)", visual.ballDirection);
		}
		else {
			sprintf(command, "(turn 0)");
			appendBallNeckCommand(visual, command, sizeof(command));
		}
		sendCmd(command);
	}

	bool isForwardPlayer() {
		return iPlayerId == 4 || iPlayerId == 5;
	}

	bool isFreshPassTarget(int cycle) {
		return isForwardPlayer()
				&& lastPassTargetUnum == iPlayerId
				&& cycle - lastPassCycle >= 0
				&& cycle - lastPassCycle <= 14;
	}

	void appendReceptionTouchSignal(int cycle, char *command,
			size_t capacity) {
		if (iPlayerId == 4 || !isFreshPassTarget(cycle)
				|| cycle - lastOwnKickOffCycle > 40) {
			return;
		}
		char message[8];
		sprintf(message, "t%d", iPlayerId);
		appendSayCommand(command, capacity, message);
	}

	int stableForwardChaseDecision(int mayChase, double ballDistance,
			double chaseLimit, int cycle) {
		if (!isForwardPlayer()) {
			return mayChase;
		}

		const int desiredMode = (mayChase && ballDistance <= chaseLimit) ? 1 : 2;
		if (lastForwardMode != 0 && desiredMode != lastForwardMode
				&& cycle - lastForwardModeCycle < 4) {
			return lastForwardMode == 1;
		}

		if (desiredMode != lastForwardMode) {
			lastForwardMode = desiredMode;
			lastForwardModeCycle = cycle;
		}
		return desiredMode == 1;
	}

	void logAttackDecision(int cycle, const VisualState &visual,
			const char *action, const char *command) {
		printf("attack_debug: cycle=%d id=%d action=%s stamina=%.1f ball=%.1f goal=%d %.1f partner=%d opp=%d command=%s\n",
				cycle, iPlayerId, action,
				gBodyState.hasStamina ? gBodyState.stamina : -1.0,
				visual.hasBall ? visual.ballDistance : -1.0,
				visual.hasAttackGoal ? 1 : 0,
				visual.hasAttackGoal ? visual.attackGoalDistance : -1.0,
				visual.teammateCount, visual.opponentCount, command);
	}

	void makeForwardSupportCommand(const VisualState &visual, char *command) {
		const bool farFromBall = visual.ballDistance > 24.0;
		const int supportPower = staminaDashPower(
				farFromBall ? 76 : 82, false);
		double laneBase = visual.ballDirection;
		getAttackDirection(visual, laneBase);
		double supportOffset = iPlayerId == 5
				? (farFromBall ? 34.0 : 26.0)
				: (farFromBall ? -34.0 : -26.0);
		SeenPlayer partner;
		const int partnerUnum = iPlayerId == 4 ? 5 : 4;
		const bool partnerOwnsChase = lastHeardChaseUnum == partnerUnum
				&& visual.cycle - lastHeardChaseCycle >= 0
				&& visual.cycle - lastHeardChaseCycle <= 4;
		if (partnerOwnsChase) {
			supportOffset = iPlayerId == 5 ? 42.0 : -42.0;
		}
		if (findTeammate(visual, partnerUnum, partner)) {
			const double partnerSide = normalizeAngle(
					partner.direction - laneBase);
			if (partner.distance < 10.0) {
				supportOffset = partnerSide >= 0.0 ? -40.0 : 40.0;
			}
			else if ((partnerSide > 5.0 && supportOffset > 0.0)
					|| (partnerSide < -5.0 && supportOffset < 0.0)) {
				supportOffset = -supportOffset;
			}
		}
		double laneDirection = normalizeAngle(laneBase + supportOffset);
		if (hasOpponentInLane(visual, laneDirection, 10.0, 20.0)) {
			supportOffset = clampDouble(supportOffset * 1.35, -48.0, 48.0);
			laneDirection = normalizeAngle(laneBase + supportOffset);
		}
		gLastAttackAction = farFromBall ? "support_outlet" : "support_lane";
		makeTurnOrDashCommand(laneDirection, supportPower, command);
		if (std::strncmp(command, "(dash", 5) == 0) {
			appendBallNeckCommand(visual, command, 160);
		}
	}

	void makeKickoffTakerSupportCommand(const VisualState &visual,
			char *command) {
		if (!visual.hasAttackGoal) {
			sprintf(command, "(turn 20)");
			gLastAttackAction = "kickoff_support_scan";
			return;
		}

		gLastAttackAction = "kickoff_support_forward";
		if (absDouble(visual.attackGoalDirection) > 14.0) {
			sprintf(command, "(turn %.1f)", visual.attackGoalDirection);
		}
		else {
			sprintf(command, "(dash %d)", staminaDashPower(70, false));
			appendBallNeckCommand(visual, command, 160);
		}
	}

	void makeForwardPossessionCommand(const VisualState &visual, int cycle,
			char *command) {
		const int partnerUnum = iPlayerId == 4 ? 5 : 4;
		SeenPlayer partner = { 0, 0.0, 0.0 };
		const bool hasPartner = findTeammate(visual, partnerUnum, partner);
		const bool underPressure = hasNearbyOpponent(visual, 5.0);
		const bool goalBlocked = visual.hasAttackGoal
				&& hasOpponentInLane(visual, visual.attackGoalDirection,
						10.0, 18.0);
		bool partnerAhead = false;
		if (hasPartner && visual.hasAttackGoal) {
			const double partnerGoalDistance = estimateDistance(
					partner.distance, visual.attackGoalDistance,
					partner.direction, visual.attackGoalDirection);
			partnerAhead = partnerGoalDistance + 3.0
					< visual.attackGoalDistance;
		}
		else if (hasPartner && visual.hasOwnGoal) {
			const double partnerOwnGoalDistance = estimateDistance(
					partner.distance, visual.ownGoalDistance,
					partner.direction, visual.ownGoalDirection);
			partnerAhead = partnerOwnGoalDistance
					> visual.ownGoalDistance + 3.0;
		}

		const bool usefulPass = hasPartner
				&& partner.distance >= 4.0
				&& partner.distance <= 26.0
				&& absDouble(partner.direction) <= 65.0
				&& (underPressure || goalBlocked || partnerAhead);
		if (usefulPass) {
			gLastAttackAction = "pass";
			double passDirection = partner.direction;
			double passDistance = partner.distance;
			if (visual.hasAttackGoal) {
				const double leadAngle = clampDouble(normalizeAngle(
						visual.attackGoalDirection - partner.direction) * 0.25,
						-12.0, 12.0);
				passDirection = normalizeAngle(partner.direction + leadAngle);
				passDistance += 3.0;
			}
			const int passPower = kickPowerForDistance(passDistance);
			sprintf(command, "(kick %d %.1f)(say \"p%d\")",
					passPower,
					passDirection, partnerUnum);
			appendBallNeckCommand(visual, command, 160);
			markOwnTouch(cycle, 1);
			return;
		}

		if (underPressure && (!visual.hasAttackGoal
				|| visual.attackGoalDistance > 20.0)) {
			double escapeBase = 0.0;
			getAttackDirection(visual, escapeBase);
			const double escapeDirection = chooseOpenAdvanceDirection(
					visual, escapeBase, iPlayerId);
			sprintf(command, "(kick 82 %.1f)", escapeDirection);
			appendReceptionTouchSignal(cycle, command, 160);
			appendBallNeckCommand(visual, command, 160);
			markOwnTouch(cycle, 11);
			gLastAttackAction = "escape_touch";
			return;
		}

		const bool estimatedShotRange = !visual.hasAttackGoal
				&& visual.hasOwnGoal && visual.ownGoalDistance >= 75.0;
		const bool clearShot = visual.hasAttackGoal
				&& visual.attackGoalDistance <= 32.0
				&& (!goalBlocked || visual.attackGoalDistance <= 18.0);
		if (clearShot || estimatedShotRange) {
			gLastAttackAction = "shoot";
			const int shootPower = gBodyState.hasStamina
					&& gBodyState.stamina < 3000.0 ? 82 : 100;
			double shootDirection = visual.hasAttackGoal
					? visual.attackGoalDirection
					: normalizeAngle(visual.ownGoalDirection + 180.0);
			if (visual.hasAttackGoal) {
				const double radians = 3.141592653589793 / 180.0;
				const double cornerOffset = std::atan2(
						4.5, visual.attackGoalDistance) / radians;
				const double firstCorner = normalizeAngle(
						shootDirection + cornerOffset);
				const double secondCorner = normalizeAngle(
						shootDirection - cornerOffset);
				const double firstPressure = lanePressure(
						visual, firstCorner);
				const double secondPressure = lanePressure(
						visual, secondCorner);
				if (absDouble(firstPressure - secondPressure) < 0.5) {
					shootDirection = ((cycle / 25 + iPlayerId) % 2 == 0)
							? firstCorner : secondCorner;
				}
				else {
					shootDirection = firstPressure < secondPressure
							? firstCorner : secondCorner;
				}
			}
			sprintf(command, "(kick %d %.1f)", shootPower, shootDirection);
			appendReceptionTouchSignal(cycle, command, 160);
			appendBallNeckCommand(visual, command, 160);
			markOwnTouch(cycle, 9);
			return;
		}

		double baseDirection = 0.0;
		getAttackDirection(visual, baseDirection);
		const double advanceDirection = chooseOpenAdvanceDirection(
				visual, baseDirection, iPlayerId);
		int touchPower = visual.hasAttackGoal
				&& visual.attackGoalDistance <= 55.0 ? 58 : 50;
		if (underPressure) {
			touchPower = 78;
			gLastAttackAction = "escape_touch";
		}
		else {
			gLastAttackAction = "dribble_touch";
		}
		sprintf(command, "(kick %d %.1f)", touchPower, advanceDirection);
		appendReceptionTouchSignal(cycle, command, 160);
		appendBallNeckCommand(visual, command, 160);
		const int followCycles = touchPower >= 70 ? 11 : 8;
		markOwnTouch(cycle, followCycles);
	}

	void makeDefenderClearCommand(const VisualState &visual, char *command) {
		SeenPlayer receiver = { 0, 0.0, 0.0 };
		double receiverPressure = 1000.0;
		for (int unum = 4; unum <= 5; ++unum) {
			SeenPlayer candidate;
			if (!findTeammate(visual, unum, candidate)
					|| candidate.distance < 5.0
					|| candidate.distance > 32.0
					|| hasOpponentInLane(visual, candidate.direction,
							candidate.distance - 1.0, 12.0)) {
				continue;
			}
			const double pressure = lanePressure(
					visual, candidate.direction);
			if (pressure < receiverPressure) {
				receiver = candidate;
				receiverPressure = pressure;
			}
		}
		if (receiver.unum != 0) {
			const int passPower = kickPowerForDistance(receiver.distance);
			sprintf(command, "(kick %d %.1f)(say \"p%d\")",
					passPower, receiver.direction, receiver.unum);
			return;
		}

		double clearDirection = 0.0;
		if (visual.hasAttackGoal) {
			clearDirection = visual.attackGoalDirection;
		}
		else if (visual.hasOwnGoal) {
			clearDirection = normalizeAngle(
					visual.ownGoalDirection + 180.0);
		}
		clearDirection = chooseOpenAdvanceDirection(
				visual, clearDirection, iPlayerId);
		sprintf(command, "(kick 100 %.1f)", clearDirection);
	}

	void setDefenderMode(DefenderMode mode, int cycle) {
		if (mode != defenderMode) {
			defenderMode = mode;
			defenderModeSinceCycle = cycle;
		}
	}

	bool makeDefenderGeometricShapeCommand(const VisualState &visual,
			int power, char *command) {
		if (!visual.hasBall || !visual.hasOwnGoal) {
			return false;
		}

		const double radians = 3.141592653589793 / 180.0;
		const double goalX = visual.ownGoalDistance
				* std::cos(visual.ownGoalDirection * radians);
		const double goalY = visual.ownGoalDistance
				* std::sin(visual.ownGoalDirection * radians);
		const double ballX = visual.ballDistance
				* std::cos(visual.ballDirection * radians);
		const double ballY = visual.ballDistance
				* std::sin(visual.ballDirection * radians);
		const double lineX = ballX - goalX;
		const double lineY = ballY - goalY;
		const double ballToGoal = std::sqrt(lineX * lineX + lineY * lineY);
		if (ballToGoal < 0.5) {
			return false;
		}

		const double unitX = lineX / ballToGoal;
		const double unitY = lineY / ballToGoal;
		double depth = clampDouble(9.0 + ballToGoal * 0.16, 9.0, 20.0);
		if (iPlayerId == 3) {
			depth = clampDouble(7.0 + ballToGoal * 0.10, 8.0, 13.0);
		}
		if (depth > ballToGoal - 2.0) {
			depth = ballToGoal > 6.0 ? ballToGoal - 2.0 : 4.0;
		}
		const double laneOffset = iPlayerId == 2 ? -2.0 : 2.0;
		const double targetX = goalX + unitX * depth - unitY * laneOffset;
		const double targetY = goalY + unitY * depth + unitX * laneOffset;
		const double targetDistance = std::sqrt(
				targetX * targetX + targetY * targetY);
		const double targetDirection = std::atan2(targetY, targetX) / radians;

		if (targetDistance <= 1.4) {
			if (absDouble(visual.ballDirection) > 8.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(turn 0)");
				appendBallNeckCommand(visual, command, 160);
			}
		}
		else {
			makeTurnOrDashCommand(targetDirection,
					staminaDashPower(power, false), command);
			if (std::strncmp(command, "(dash", 5) == 0) {
				appendBallNeckCommand(visual, command, 160);
			}
		}
		return true;
	}

	void makeDefenderShapeCommand(const VisualState &visual,
			bool recovering, char *command) {
		if (makeDefenderGeometricShapeCommand(
				visual, recovering ? 78 : 62, command)) {
			return;
		}

		if (!visual.hasBall) {
			const double homeLimit = iPlayerId == 2 ? 21.0 : 16.0;
			const double attackGoalMinimum = iPlayerId == 2 ? 78.0 : 86.0;
			if (visual.hasOwnGoal
					&& visual.ownGoalDistance > homeLimit) {
				makeTurnOrDashCommand(visual.ownGoalDirection,
						staminaDashPower(62, false), command);
			}
			else if (visual.hasAttackGoal
					&& visual.attackGoalDistance < attackGoalMinimum) {
				const double homeDirection = normalizeAngle(
						visual.attackGoalDirection + 180.0);
				makeTurnOrDashCommand(homeDirection,
						staminaDashPower(70, false), command);
			}
			else {
				sprintf(command, "(turn %d)", iPlayerId == 2 ? 25 : -25);
			}
			return;
		}

		const bool ballClosing = gBallTrack.hasMotion
				&& gBallTrack.closingStreak >= 2
				&& gBallTrack.closingRate > 0.35;
		if (iPlayerId == 3 && !recovering && !visual.hasOwnGoal
				&& visual.ballDistance > 24.0) {
			if (absDouble(visual.ballDirection) > 12.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(turn 0)");
				appendBallNeckCommand(visual, command, 160);
			}
			return;
		}
		if (recovering && (visual.ballDistance < 30.0 || ballClosing)) {
			if (absDouble(visual.ballDirection) > 16.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(dash %d)",
						-staminaDashPower(100, true));
				appendBallNeckCommand(visual, command, 160);
			}
			return;
		}

		const double attackGoalMinimum = iPlayerId == 2 ? 78.0 : 86.0;
		if (visual.hasAttackGoal
				&& visual.attackGoalDistance < attackGoalMinimum) {
			const double homeDirection = normalizeAngle(
					visual.attackGoalDirection + 180.0);
			makeTurnOrDashCommand(homeDirection,
					staminaDashPower(78, true), command);
			if (std::strncmp(command, "(dash", 5) == 0) {
				appendBallNeckCommand(visual, command, 160);
			}
		}
		else if (absDouble(visual.ballDirection) > 12.0) {
			sprintf(command, "(turn %.1f)", visual.ballDirection);
		}
		else {
			sprintf(command, "(turn 0)");
			appendBallNeckCommand(visual, command, 160);
		}
	}

	void handleDefender(const VisualState &visual, int cycle) {
		char command[160];
		if (!visual.hasBall) {
			const int unseenCycles = cycle - lastSeenBallCycle;
			if (lastSeenBallDistance <= 3.0
					&& unseenCycles >= 1 && unseenCycles <= 2
					&& absDouble(lastSeenBallDirection) <= 40.0
					&& cycle - lastTackleCycle >= 10) {
				sprintf(command, "(tackle 100)");
				lastTackleCycle = cycle;
			}
			else if (defenderMode == DM_CHALLENGE
					&& cycle - lastSeenBallCycle >= 0
					&& cycle - lastSeenBallCycle <= 4) {
				if (absDouble(lastSeenBallDirection) > 18.0) {
					sprintf(command, "(turn %.1f)", lastSeenBallDirection);
				}
				else {
					sprintf(command, "(dash %d)",
							staminaDashPower(75, true));
				}
			}
			else {
				makeDefenderShapeCommand(visual,
						defenderMode == DM_RECOVER, command);
			}
			appendCenterNeckCommand(command, sizeof(command));
			sendCmd(command);
			return;
		}

		lastSeenBallCycle = cycle;
		lastSeenBallDirection = visual.ballDirection;
		lastSeenBallDistance = visual.ballDistance;

		if (visual.ballDistance <= 1.15) {
			if (visual.ballDistance > 0.98) {
				makeTurnOrDashCommand(visual.ballDirection,
						staminaDashPower(55, true), command);
			}
			else if (cycle - lastDefenderKickCycle >= 3) {
				makeDefenderClearCommand(visual, command);
				lastDefenderKickCycle = cycle;
			}
			else {
				sprintf(command, "(turn 0)");
			}
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		if (visual.ballDistance <= 3.0
				&& absDouble(visual.ballDirection) <= 35.0
				&& (visual.ballDistance <= 1.8
						|| hasNearbyOpponent(visual, 4.0))
				&& cycle - lastTackleCycle >= 10) {
			sprintf(command, "(tackle 100)");
			lastTackleCycle = cycle;
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		const int partnerUnum = iPlayerId == 2 ? 3 : 2;
		SeenPlayer partner;
		bool partnerCloser = false;
		if (findTeammate(visual, partnerUnum, partner)) {
			const double partnerBallDistance = estimateDistance(
					partner.distance, visual.ballDistance,
					partner.direction, visual.ballDirection);
			partnerCloser = partnerBallDistance + 1.0
					< visual.ballDistance;
			if (absDouble(partnerBallDistance - visual.ballDistance) <= 1.0
					&& partner.unum < iPlayerId) {
				partnerCloser = true;
			}
		}
		else if (lastHeardChaseUnum == partnerUnum
				&& cycle - lastHeardChaseCycle <= 4) {
			partnerCloser = true;
		}
		else if (iPlayerId == 3) {
			partnerCloser = visual.ballDistance > 24.0;
		}

		const double ballToOwnGoal = visual.hasOwnGoal
				? estimateDistance(visual.ballDistance,
						visual.ownGoalDistance, visual.ballDirection,
						visual.ownGoalDirection)
				: 1000.0;
		const bool ballClosingFast = gBallTrack.hasMotion
				&& gBallTrack.closingStreak >= 2
				&& gBallTrack.closingRate > 0.45;
		const bool emergency = visual.ballDistance <= 13.0
				|| (visual.hasOwnGoal && ballToOwnGoal <= 26.0)
				|| (ballClosingFast && visual.ballDistance <= 22.0);
		const bool lastLineEmergency = iPlayerId == 3
				&& visual.ballDistance <= 16.0
				&& (!visual.hasOwnGoal || ballToOwnGoal <= 24.0);
		const double challengeLimit = iPlayerId == 2 ? 24.0 : 16.0;
		const bool shouldChallenge = (!partnerCloser || lastLineEmergency)
				&& visual.ballDistance <= challengeLimit;
		DefenderMode desiredMode = shouldChallenge
				? DM_CHALLENGE : (emergency ? DM_RECOVER : DM_SHAPE);

		if (desiredMode != defenderMode) {
			const bool urgentChallenge = desiredMode == DM_CHALLENGE
					&& emergency;
			if (urgentChallenge || cycle - defenderModeSinceCycle >= 6) {
				setDefenderMode(desiredMode, cycle);
			}
		}

		if (defenderMode == DM_CHALLENGE) {
			const double targetDirection = predictedBallDirection(visual, 3);
			makeTurnOrDashCommand(targetDirection,
					staminaDashPower(100, true), command);
			if (std::strncmp(command, "(dash", 5) == 0) {
				if (cycle - lastChaseSayCycle >= 5) {
					char message[8];
					sprintf(message, "c%d", iPlayerId);
					appendSayCommand(command, sizeof(command), message);
					lastChaseSayCycle = cycle;
				}
				appendBallNeckCommand(visual, command, sizeof(command));
			}
		}
		else {
			makeDefenderShapeCommand(visual,
					defenderMode == DM_RECOVER, command);
		}
		sendCmd(command);
	}

	void handleFieldPlayer(const VisualState &visual, int cycle) {
		char command[160];

		if (handleStructuredRestart(visual, cycle)) {
			return;
		}

		if (handleKickOff(visual, cycle)) {
			return;
		}

		if (isRestartHoldMode()) {
			gLastAttackAction = "restart_wait";
			double x = 0.0;
			double y = 0.0;
			restartFormationPosition(iPlayerId, x, y);
			sprintf(command, "(move %.1f %.1f)", x, y);
			defenderMode = DM_SHAPE;
			defenderModeSinceCycle = cycle;
			sendCmd(command);
			return;
		}

		if (kickoffSecondTouchLocked(cycle)) {
			if (hasOtherPlayerTouchingBall(visual)) {
				releaseKickoffSecondTouchLock();
			}
			else {
				makeKickoffTakerSupportCommand(visual, command);
				logAttackDecision(cycle, visual,
						gLastAttackAction, command);
				sendCmd(command);
				return;
			}
		}

		if (iPlayerId == 2 || iPlayerId == 3) {
			handleDefender(visual, cycle);
			return;
		}

		const int passTarget = isFreshPassTarget(cycle);
		if (!visual.hasBall) {
			const int passAge = cycle - lastPassCycle;
			const bool arrivingInBlindCycle = passTarget
					&& cycle - lastSeenBallCycle == 1
					&& lastSeenBallDistance <= 1.3
					&& gBallTrack.hasMotion
					&& gBallTrack.closingRate > 0.15;
			if (arrivingInBlindCycle) {
				double trapDirection = 0.0;
				getAttackDirection(visual, trapDirection);
				sprintf(command, "(kick 45 %.1f)", trapDirection);
				markOwnTouch(cycle, 5);
				gLastAttackAction = "receive_trap";
				logAttackDecision(cycle, visual,
						gLastAttackAction, command);
			}
			else if (passTarget && passAge <= 5) {
				if (absDouble(lastPassSourceDirection) > 10.0) {
					sprintf(command, "(turn %.1f)",
							lastPassSourceDirection);
					lastPassSourceDirection *= 0.45;
					gLastAttackAction = "receive_scan";
				}
				else {
					sprintf(command, "(turn 0)");
					gLastAttackAction = "receive_track";
				}
				logAttackDecision(cycle, visual,
						gLastAttackAction, command);
			}
			else if (cycle - lastSeenBallCycle >= 0
					&& cycle - lastSeenBallCycle <= 3) {
				if (absDouble(lastSeenBallDirection) > 20.0) {
					sprintf(command, "(turn %.1f)",
							lastSeenBallDirection);
					lastSeenBallDirection = 0.0;
					gLastAttackAction = "continue_turn";
				}
				else {
					sprintf(command, "(dash %d)",
							staminaDashPower(72, false));
					gLastAttackAction = "continue_run";
				}
			}
			else {
				sprintf(command, "(turn %d)",
						iPlayerId % 2 == 0 ? 30 : -30);
				gLastAttackAction = "search_ball";
			}
			appendCenterNeckCommand(command, sizeof(command));
			sendCmd(command);
			return;
		}

		lastSeenBallCycle = cycle;
		lastSeenBallDirection = visual.ballDirection;
		lastSeenBallDistance = visual.ballDistance;
		const double ballDistance = visual.ballDistance;
		const double chaseLimit = iPlayerId == 5 ? 50.0 : 42.0;

		const int mayChase = shouldChaseBall(visual, iPlayerId);
		printf("chase_debug: cycle=%d id=%d side=%d srvSide=%d playMode=%d ball=%.1f tm=%d chase=%d pass=%d reason=%s\n",
				cycle, iPlayerId, iSide, gMatchState.sideFromServer,
				static_cast<int>(gMatchState.playMode), ballDistance,
				visual.teammateCount, mayChase, passTarget, gLastChaseReason);
		int activeChase = stableForwardChaseDecision(mayChase,
				ballDistance, chaseLimit, cycle);
		double effectiveChaseLimit = chaseLimit;
		if (cycle - lastOwnTouchCycle >= 0
				&& cycle <= ownTouchFollowUntilCycle) {
			activeChase = 1;
			effectiveChaseLimit = 45.0;
			gLastAttackAction = "follow_touch";
		}
		if (passTarget) {
			activeChase = 1;
			effectiveChaseLimit = 30.0;
			gLastAttackAction = "receive";
		}

		if (!activeChase || ballDistance > effectiveChaseLimit) {
			makeForwardSupportCommand(visual, command);
			logAttackDecision(cycle, visual, gLastAttackAction, command);
			sendCmd(command);
			return;
		}

		if (ballDistance > 1.10) {
			const double chaseDir = predictedBallDirection(visual, 3);
			if (absDouble(chaseDir) > 18.0) {
				sprintf(command, "(turn %.1f)", chaseDir);
				gLastAttackAction = passTarget
						? "receive_turn" : "chase_turn";
			}
			else {
				int dashPower = 100;
				if (ballDistance < 1.8) {
					const bool ballApproaching = gBallTrack.hasMotion
							&& gBallTrack.closingRate > 0.15;
					dashPower = ballApproaching
							? 35 : (passTarget ? 76 : 70);
				}
				else if (ballDistance < 2.5) {
					dashPower = passTarget ? 88 : 80;
				}
				dashPower = staminaDashPower(dashPower, true);
				if (cycle - lastChaseSayCycle >= 5) {
					sprintf(command, "(dash %d 0)(say \"c%d\")",
							dashPower, iPlayerId);
					lastChaseSayCycle = cycle;
				}
				else {
					sprintf(command, "(dash %d 0)", dashPower);
				}
				gLastAttackAction = passTarget
						? "receive_dash" : "chase_dash";
				appendBallNeckCommand(visual, command, sizeof(command));
			}
			logAttackDecision(cycle, visual, gLastAttackAction, command);
			sendCmd(command);
			return;
		}

		makeForwardPossessionCommand(visual, cycle, command);
		logAttackDecision(cycle, visual, gLastAttackAction, command);
		sendCmd(command);
	}

	void parseMsg(char * msg, const size_t len) {
		if (len > 0) {
			msg[len] = '\0';
		}

		if (!std::strncmp(msg, "(ok compression", 15)) {
			int level;
			if (std::sscanf(msg, "(ok compression %d", &level) == 1) {
				setCompression(level);
			}
			return;
		}

		if (!std::strncmp(msg, "(init", 5)) {
			if (parseInitMessage(msg)) {
				setupAfterInit();
			}
			M_clean_cycle = true;
			return;
		}

		if (!std::strncmp(msg, "(hear", 5)) {
			int hearCycle = 0;
			if (std::sscanf(msg, "(hear %d", &hearCycle) == 1) {
				handleHearMessage(msg, hearCycle);
			}
			else {
				handleHearMessage(msg, lastSeeCycle);
			}
			M_clean_cycle = true;
			return;
		}

		if (!std::strncmp(msg, "(sense_body", 11)) {
			parseSenseBodyMessage(msg);
			M_clean_cycle = true;
			return;
		}

		if (!std::strncmp(msg, "(see_global", 11)) {
			M_clean_cycle = true;
		}

		if (std::strncmp(msg, "(see ", 5)) return;

		int cycle = -1;
		if (std::sscanf(msg, "(see %d", &cycle) != 1) return;
		if (cycle == lastSeeCycle) return;
		lastSeeCycle = cycle;

		VisualState visual;
		if (!parseVisualState(msg, visual)) {
			return;
		}
		updateBallTrack(visual);

		if (iPlayerId == 1) {
			handleGoalkeeper(visual, cycle);
		}
		else if (iPlayerId >= 2 && iPlayerId <= 5) {
			handleFieldPlayer(visual, cycle);
		}
	}

	void messageLoop() {
		fd_set read_fds;
		fd_set read_fds_back;
		char buf[8192];
		std::memset(&buf, 0, sizeof(char) * 8192);

		int in = fileno( stdin );
		FD_ZERO(&read_fds);
		FD_SET(in, &read_fds);
		FD_SET(M_socket.getFD(), &read_fds);
		read_fds_back = read_fds;

#ifdef RCSS_WIN
		int max_fd = 0;
#else
		int max_fd = M_socket.getFD() + 1;
#endif
		while (1) {

			read_fds = read_fds_back;
			int ret = ::select(max_fd, &read_fds, NULL, NULL, NULL); //select
			if (ret < 0) {
				perror("Error selecting input");
				break;
			} else if (ret != 0) {
				// 从标准输入读取
				if (FD_ISSET(in, &read_fds)) {
					if (std::fgets(buf, sizeof(buf), stdin) != NULL) {
						size_t len = std::strlen(buf);
						if (buf[len - 1] == '\n') {
							buf[len - 1] = '\0';
							--len;
						}

						M_transport->write(buf, len + 1);
						M_transport->flush();
						if (!M_transport->good()) {
							if (errno != ECONNREFUSED) {
								std::cerr << __FILE__ << ": " << __LINE__
										<< ": Error sending to socket: "
										<< strerror(errno) << std::endl
										<< "msg = [" << buf << "]\n";
							}
							M_socket.close();
						}
						std::cout << buf << std::endl;
					}
				}

				// 从套接字读取
				if (FD_ISSET(M_socket.getFD(), &read_fds)) {
					rcss::net::Addr from;
					int len = M_socket.recv(buf, sizeof(buf) - 1, from);
					if (len == -1 && errno != EWOULDBLOCK) {
						if (errno != ECONNREFUSED) {
							std::cerr << __FILE__ << ": " << __LINE__
									<< ": Error receiving from socket: "
									<< strerror(errno) << std::endl;
						}
						M_socket.close();
					} else if (len > 0) {
						M_dest.setPort(from.getPort());
						M_socket_buf->setEndPoint(M_dest);
						processMsg(buf, len);
					}
				}
			}
		}
	}
};

namespace {
Client * client = static_cast<Client *> (0);

void sig_exit_handle(int) {
	std::cerr << "\nKilled. Exiting..." << std::endl;
	if (client) {
		delete client;
		client = static_cast<Client *> (0);
	}
	std::exit(EXIT_FAILURE);
}
}

int main(int argc, char **argv) {
	if (std::signal(SIGINT, &sig_exit_handle) == SIG_ERR || std::signal(  // 查看csignal.h 的功能
			SIGTERM, &sig_exit_handle) == SIG_ERR || std::signal(SIGHUP,
			&sig_exit_handle) == SIG_ERR) {
		std::cerr << __FILE__ << ": " << __LINE__
				<< ": could not set signal handler: " << std::strerror(errno)
				<< std::endl;
		std::exit(EXIT_FAILURE);
	}

	std::cerr << "Hit Ctrl-C to exit." << std::endl;

	std::string server = "localhost";
	int port = 6000;

	for (int i = 0; i < argc; ++i) {
		if (std::strcmp(argv[i], "-server") == 0) {
			if (i + 1 < argc) {
				server = argv[i + 1];
				++i;
			}
		} else if (std::strcmp(argv[i], "-port") == 0) {
			if (i + 1 < argc) {
				port = std::atoi(argv[i + 1]);
				++i;
			}
		}
		if (std::strcmp(argv[i], "-id") == 0) {
			if (i + 1 < argc) {
				iPlayerId = std::atoi(argv[i + 1]);
				++i;
			}
		}
		if (std::strcmp(argv[i], "-team") == 0) {
			if (i + 1 < argc) {
				teamName = argv[i + 1];
				teamNameFromArg = true;
				++i;
			}
		}
		if (std::strcmp(argv[i], "-sidel") == 0) {
			iSide = 1;
		}
		if (std::strcmp(argv[i], "-sider") == 0) {
			iSide = 2;
		}
	}

	if (iPlayerId < 1 || iPlayerId > 5) {
		std::cerr << "error: -id must be 1..5, got " << iPlayerId << std::endl;
		return EXIT_FAILURE;
	}
	if (iSide != 1 && iSide != 2) {
		std::cerr << "error: specify -sidel or -sider" << std::endl;
		return EXIT_FAILURE;
	}

	client = new Client(server, port);
	client->run();

	return EXIT_SUCCESS;
}
