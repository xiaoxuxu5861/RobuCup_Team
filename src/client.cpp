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
bool goalieCatchSuccess = false;
int goalieCatchCycle = -100;
int goalieAdvanceSteps = 0;
int goalieLastBallCycle = -100;
double goalieLastBallDirection = 0.0;
int defenderAdvanceSteps = 0;
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

enum PlayMode {
	PM_UNKNOWN = 0,
	PM_BEFORE_KICK_OFF,
	PM_PLAY_ON,
	PM_GOAL_L,
	PM_GOAL_R,
	PM_KICK_OFF_L,
	PM_KICK_OFF_R,
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
	double stamina;
	double effort;
	double headAngle;
	int cycle;
};

BodyState gBodyState = { false, 8000.0, 1.0, 0.0, -1 };
int lastHeardChaseUnum = 0;
int lastHeardChaseCycle = -100;
int lastPassTargetUnum = 0;
int lastPassCycle = -100;
double lastPassSourceDirection = 0.0;
int lastChaseSayCycle = -100;
int lastOwnTouchCycle = -100;
int lastSeenBallCycle = -100;
double lastSeenBallDirection = 0.0;
const char *gLastChaseReason = "none";
const char *gLastAttackAction = "none";
int lastForwardMode = 0; // 1 chase, 2 support
int lastForwardModeCycle = -100;

double absDouble(double value);
double normalizeAngle(double angle);

void clearTransientAttackState() {
	lastHeardChaseUnum = 0;
	lastHeardChaseCycle = -100;
	lastPassTargetUnum = 0;
	lastPassCycle = -100;
	lastPassSourceDirection = 0.0;
	lastChaseSayCycle = -100;
	lastOwnTouchCycle = -100;
	lastSeenBallCycle = -100;
	lastSeenBallDirection = 0.0;
	lastForwardMode = 0;
	lastForwardModeCycle = -100;
	gLastChaseReason = "restart";
	gLastAttackAction = "restart_wait";
}

bool isRestartHoldMode() {
	return gMatchState.playMode == PM_BEFORE_KICK_OFF
			|| gMatchState.playMode == PM_GOAL_L
			|| gMatchState.playMode == PM_GOAL_R;
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
	if (playerId == 4) return 4;
	if (playerId == 5) return 3;
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
	else {
		state.playMode = PM_OTHER;
	}
	if (isRestartHoldMode()) {
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
			printf("pass_heard: cycle=%d target=%d self=%d\n",
					cycle, target, iPlayerId);
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
	const char *headAngle = std::strstr(msg, "(head_angle ");
	if (headAngle != 0) {
		std::sscanf(headAngle, "(head_angle %lf", &gBodyState.headAngle);
	}
	gBodyState.cycle = cycle;
	printf("body_parse: cycle=%d stamina=%.1f effort=%.2f head=%.1f\n",
			gBodyState.cycle, gBodyState.stamina, gBodyState.effort,
			gBodyState.headAngle);
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
	if (!state.hasBall) {
		gLastChaseReason = "no_ball";
		return false;
	}
	if ((playerId == 4 || playerId == 5)
			&& state.cycle - lastOwnTouchCycle >= 0
			&& state.cycle - lastOwnTouchCycle <= 6) {
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
		const double est = estimateDistance(tm.distance, state.ballDistance,
				tm.direction, state.ballDirection);
		if (tm.unum == lastHeardChaseUnum) {
			sawClaimingTeammate = true;
			claimingTeammateBallDistance = est;
		}
		if (est + 1.5 < state.ballDistance) {
			gLastChaseReason = "closer";
			return false;
		}
		if (absDouble(est - state.ballDistance) <= 1.5 && tm.unum < playerId) {
			gLastChaseReason = "closer";
			return false;
		}
	}

	// 门前危急：允许最近后卫接管，即使角色优先级低于 4/5。
	if (state.hasOwnGoal && state.ballDistance < 8.0
			&& state.ownGoalDistance < 25.0
			&& (playerId == 2 || playerId == 3)) {
		bool closerThanOtherDefender = true;
		for (int i = 0; i < state.teammateCount; ++i) {
			const SeenPlayer &tm = state.teammates[i];
			if (tm.unum != 2 && tm.unum != 3) continue;
			if (tm.unum == playerId) continue;
			const double est = estimateDistance(tm.distance, state.ballDistance,
					tm.direction, state.ballDirection);
			if (est + 0.5 < state.ballDistance) {
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
	if (hearFresh && lastHeardChaseUnum != playerId
			&& (!sawClaimingTeammate
					|| claimingTeammateBallDistance
							<= state.ballDistance + 2.0)) {
		gLastChaseReason = "hear";
		return false;
	}

	if (!sawComparableTeammate && !hearFresh
			&& playerId == 5 && state.ballDistance <= 28.0) {
		gLastChaseReason = "secondary";
		return true;
	}
	if (!sawComparableTeammate && !hearFresh) {
		// 信息不足：仅 4 号主动追，避免四人同冲。
		if (chaseRoleRank(playerId) < 4) {
			gLastChaseReason = "priority";
			return false;
		}
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

	int readBallInfo(const char *msg, double &distance, double &direction) {
		return readObjectInfo(msg, "(ball)", distance, direction)
				|| readObjectInfo(msg, "(b)", distance, direction);
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

		state.hasBall = readBallInfo(msg, state.ballDistance,
				state.ballDirection) != 0;
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

		collectPlayersFromSee(msg, "(player ", state);
		collectPlayersFromSee(msg, "(p ", state);
		return true;
	}

	void makeTurnOrDashCommand(double direction, int power, char *command) {
		if (absDouble(direction) > 16.0) {
			sprintf(command, "(turn %.1f)", direction);
		}
		else {
			sprintf(command, "(dash %d)", power);
		}
	}

	void appendBallNeckCommand(const VisualState &visual, char *command,
			size_t capacity) {
		if (!visual.hasBall) {
			return;
		}
		double moment = visual.ballNeckDirection;
		if (moment > 90.0) moment = 90.0;
		if (moment < -90.0) moment = -90.0;
		char neck[48];
		sprintf(neck, "(turn_neck %.1f)", moment);
		if (std::strlen(command) + std::strlen(neck) + 1 < capacity) {
			std::strcat(command, neck);
		}
	}

	bool handleKickOff(const VisualState &visual, int cycle) {
		if (gMatchState.playMode != PM_KICK_OFF_L
				&& gMatchState.playMode != PM_KICK_OFF_R) {
			return false;
		}

		char command[160];
		if (isOwnKickOffMode() && iPlayerId == 4) {
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
				double direction = visual.hasAttackGoal
						? visual.attackGoalDirection : 0.0;
				if (hasOpponentInLane(visual, direction, 10.0, 18.0)) {
					direction = normalizeAngle(direction
							+ (iPlayerId == 4 ? 22.0 : -22.0));
				}
				sprintf(command, "(kick 55 %.1f)", direction);
				appendBallNeckCommand(visual, command, sizeof(command));
				lastOwnTouchCycle = cycle;
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
			makeTurnOrDashCommand(visual.ownGoalDirection, 75, command);
			if (absDouble(visual.ownGoalDirection) <= 10.0
					&& goalieAdvanceSteps > 0) {
				--goalieAdvanceSteps;
			}
		}
		else {
			sprintf(command, "(dash -60)");
			if (goalieAdvanceSteps > 0) --goalieAdvanceSteps;
		}
	}

	void handleGoalkeeper(const VisualState &visual, int cycle) {
		char command[160];

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
			sendCmd(command);
			return;
		}

		if (visual.hasBall) {
			goalieLastBallCycle = cycle;
			goalieLastBallDirection = visual.ballDirection;
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
		const bool catchAreaConfirmed = visual.hasOwnGoal
				? ballToOwnGoal <= 18.0
				: goalieAdvanceSteps <= 7;
		if (visual.hasBall && visual.ballDistance <= 1.3
				&& catchAreaConfirmed
				&& cycle - goalieCatchAttemptCycle >= 2) {
			sprintf(command, "(catch %.1f)", visual.ballDirection);
			goalieCatchAttemptCycle = cycle;
			goalieState = GS_CATCHING;
			sendCmd(command);
			return;
		}

		if (visual.hasBall && visual.ballDistance <= 1.0) {
			sprintf(command, "(kick 100 %.1f)", clearDirection);
			appendBallNeckCommand(visual, command, sizeof(command));
			goalieState = GS_RETURNING;
			sendCmd(command);
			return;
		}

		if (visual.hasBall && visual.ballDistance <= 2.5
				&& hasNearbyOpponent(visual, 3.0)) {
			sprintf(command, "(tackle 100)");
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		const bool immediateThreat = visual.hasBall
				&& visual.ballDistance <= 14.0
				&& (!visual.hasOwnGoal || ballToOwnGoal <= 18.0);
		if (immediateThreat && goalieAdvanceSteps < 7) {
			makeTurnOrDashCommand(visual.ballDirection, 90, command);
			if (std::strncmp(command, "(dash", 5) == 0) {
				++goalieAdvanceSteps;
				appendBallNeckCommand(visual, command, sizeof(command));
			}
			goalieState = GS_CHASING;
			sendCmd(command);
			return;
		}

		if (!visual.hasBall) {
			if (cycle - goalieLastBallCycle >= 0
					&& cycle - goalieLastBallCycle <= 2
					&& goalieAdvanceSteps < 7) {
				makeTurnOrDashCommand(goalieLastBallDirection, 78, command);
				if (std::strncmp(command, "(dash", 5) == 0) {
					++goalieAdvanceSteps;
				}
			}
			else if (goalieAdvanceSteps > 0) {
				goalieState = GS_RETURNING;
				makeGoalkeeperReturnCommand(visual, command);
			}
			else {
				sprintf(command, "(turn 25)");
			}
			sendCmd(command);
			return;
		}

		const bool tooFarFromGoal = visual.hasOwnGoal
				&& visual.ownGoalDistance > 9.0;
		if (goalieState == GS_RETURNING || tooFarFromGoal
				|| (goalieAdvanceSteps > 0
						&& visual.ballDistance > 16.0)) {
			goalieState = GS_RETURNING;
			makeGoalkeeperReturnCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			if (goalieAdvanceSteps == 0 && !tooFarFromGoal) {
				goalieState = GS_HOLDING;
			}
			sendCmd(command);
			return;
		}

		if (visual.hasOwnGoal && visual.ownGoalDistance < 3.5) {
			const double awayFromGoal = normalizeAngle(
					visual.ownGoalDirection + 180.0);
			makeTurnOrDashCommand(awayFromGoal, 35, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		goalieState = GS_HOLDING;
		if (absDouble(visual.ballDirection) > 14.0) {
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
				&& cycle - lastPassCycle <= 20;
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
		const int supportPower = staminaDashPower(82, false);
		if (visual.ballDistance > 24.0) {
			gLastAttackAction = "support_catchup";
			if (absDouble(visual.ballDirection) > 20.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				sprintf(command, "(dash %d)", supportPower);
				appendBallNeckCommand(visual, command, 160);
			}
			return;
		}

		const double laneBase = visual.hasAttackGoal
				? visual.attackGoalDirection : visual.ballDirection;
		double supportOffset = iPlayerId == 5 ? 18.0 : -18.0;
		double laneDirection = normalizeAngle(laneBase + supportOffset);
		if (hasOpponentInLane(visual, laneDirection, 10.0, 20.0)) {
			supportOffset = -supportOffset;
			laneDirection = normalizeAngle(laneBase + supportOffset);
		}
		gLastAttackAction = "support_lane";
		if (absDouble(laneDirection) > 20.0) {
			sprintf(command, "(turn %.1f)", laneDirection);
		}
		else {
			sprintf(command, "(dash %d)", supportPower);
			appendBallNeckCommand(visual, command, 160);
		}
	}

	void makeForwardPossessionCommand(const VisualState &visual, int cycle,
			char *command) {
		const int partnerUnum = iPlayerId == 4 ? 5 : 4;
		SeenPlayer partner;
		const bool hasPartner = findTeammate(visual, partnerUnum, partner);
		const bool underPressure = hasNearbyOpponent(visual, 5.0);
		const bool goalBlocked = visual.hasAttackGoal
				&& hasOpponentInLane(visual, visual.attackGoalDirection,
						10.0, 18.0);
		const double partnerGoalDistance = hasPartner
				&& visual.hasAttackGoal
				? estimateDistance(partner.distance,
						visual.attackGoalDistance, partner.direction,
						visual.attackGoalDirection)
				: 1000.0;
		const bool partnerAhead = visual.hasAttackGoal
				&& partnerGoalDistance + 3.0 < visual.attackGoalDistance;

		const bool usefulPass = hasPartner
				&& partner.distance >= 4.0
				&& partner.distance <= 26.0
				&& absDouble(partner.direction) <= 65.0
				&& (underPressure || goalBlocked || partnerAhead);
		if (usefulPass) {
			gLastAttackAction = "pass";
			int passPower = static_cast<int>(45.0 + partner.distance * 2.0);
			if (passPower < 55) passPower = 55;
			if (passPower > 82) passPower = 82;
			sprintf(command, "(kick %d %.1f)(say p%d)",
					passPower,
					partner.direction, partnerUnum);
			appendBallNeckCommand(visual, command, 160);
			lastOwnTouchCycle = cycle;
			return;
		}

		if (visual.hasAttackGoal && visual.attackGoalDistance <= 36.0) {
			gLastAttackAction = "shoot";
			const int shootPower = gBodyState.hasStamina
					&& gBodyState.stamina < 3000.0 ? 82 : 100;
			sprintf(command, "(kick %d %.1f)", shootPower,
					visual.attackGoalDirection);
			appendBallNeckCommand(visual, command, 160);
			lastOwnTouchCycle = cycle;
			return;
		}

		const double baseDirection = visual.hasAttackGoal
				? visual.attackGoalDirection : 0.0;
		const double advanceDirection = chooseOpenAdvanceDirection(
				visual, baseDirection, iPlayerId);
		int touchPower = visual.hasAttackGoal
				&& visual.attackGoalDistance <= 45.0 ? 52 : 44;
		if (underPressure) {
			touchPower = 62;
			gLastAttackAction = "escape_touch";
		}
		else {
			gLastAttackAction = "dribble_touch";
		}
		sprintf(command, "(kick %d %.1f)", touchPower, advanceDirection);
		appendBallNeckCommand(visual, command, 160);
		lastOwnTouchCycle = cycle;
	}

	void makeDefenderClearCommand(const VisualState &visual, char *command) {
		double clearDirection = iPlayerId == 2 ? 55.0 : -55.0;
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

	void makeDefenderReturnCommand(const VisualState &visual, char *command) {
		if (visual.hasOwnGoal) {
			makeTurnOrDashCommand(visual.ownGoalDirection, 65, command);
			if (absDouble(visual.ownGoalDirection) <= 10.0
					&& defenderAdvanceSteps > 0) {
				--defenderAdvanceSteps;
			}
		}
		else {
			sprintf(command, "(dash -45)");
			if (defenderAdvanceSteps > 0) --defenderAdvanceSteps;
		}
	}

	void handleDefender(const VisualState &visual, int cycle) {
		char command[160];
		if (!visual.hasBall) {
			if (cycle - lastSeenBallCycle >= 0
					&& cycle - lastSeenBallCycle <= 3) {
				if (absDouble(lastSeenBallDirection) > 20.0) {
					sprintf(command, "(turn %.1f)",
							lastSeenBallDirection);
					lastSeenBallDirection = 0.0;
				}
				else {
					sprintf(command, "(dash %d)",
							staminaDashPower(72, true));
					if (defenderAdvanceSteps < 15) {
						++defenderAdvanceSteps;
					}
				}
			}
			else if (defenderAdvanceSteps > 0) {
				makeDefenderReturnCommand(visual, command);
			}
			else {
				sprintf(command, "(turn %d)", iPlayerId == 2 ? 25 : -25);
			}
			sendCmd(command);
			return;
		}

		lastSeenBallCycle = cycle;
		lastSeenBallDirection = visual.ballDirection;

		if (visual.ballDistance <= 1.1) {
			makeDefenderClearCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		if (visual.ballDistance <= 3.0
				&& absDouble(visual.ballDirection) <= 35.0
				&& (visual.ballDistance <= 1.8
						|| hasNearbyOpponent(visual, 4.0))) {
			sprintf(command, "(tackle 100)");
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

		const double ballToOwnGoal = visual.hasOwnGoal
				? estimateDistance(visual.ballDistance,
						visual.ownGoalDistance, visual.ballDirection,
						visual.ownGoalDirection)
				: 1000.0;
		const bool emergency = visual.ballDistance <= 14.0
				|| (visual.hasOwnGoal && ballToOwnGoal <= 25.0);
		const int advanceLimit = iPlayerId == 2 ? 12 : 10;
		const bool shouldChallenge = !partnerCloser
				&& (emergency || visual.ballDistance <= 30.0)
				&& (defenderAdvanceSteps < advanceLimit || emergency);
		if (shouldChallenge) {
			if (absDouble(visual.ballDirection) > 20.0) {
				sprintf(command, "(turn %.1f)", visual.ballDirection);
			}
			else {
				const int dashPower = staminaDashPower(100, true);
				sprintf(command, "(dash %d)", dashPower);
				appendBallNeckCommand(visual, command, sizeof(command));
				if (defenderAdvanceSteps < 15) {
					++defenderAdvanceSteps;
				}
			}
			sendCmd(command);
			return;
		}

		if (defenderAdvanceSteps >= advanceLimit && !emergency) {
			makeDefenderReturnCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		SeenPlayer nearestOpponent;
		const bool hasMarkingTarget = findNearestOpponent(
				visual, nearestOpponent)
				&& nearestOpponent.distance <= 14.0;
		if (partnerCloser && hasMarkingTarget && !emergency
				&& defenderAdvanceSteps < 8) {
			if (absDouble(nearestOpponent.direction) > 22.0) {
				sprintf(command, "(turn %.1f)", nearestOpponent.direction);
			}
			else {
				sprintf(command, "(dash %d)",
						staminaDashPower(82, true));
				appendBallNeckCommand(visual, command, sizeof(command));
				if (defenderAdvanceSteps < 8) {
					++defenderAdvanceSteps;
				}
			}
			sendCmd(command);
			return;
		}

		const bool needsDepthSupport = partnerCloser
				&& visual.ballDistance <= 32.0
				&& defenderAdvanceSteps < 7;
		if (needsDepthSupport) {
			const double offset = iPlayerId == 2 ? 16.0 : -16.0;
			const double coverDirection = normalizeAngle(
					visual.ballDirection + offset);
			if (absDouble(coverDirection) > 22.0) {
				sprintf(command, "(turn %.1f)", coverDirection);
			}
			else {
				sprintf(command, "(dash %d)",
						staminaDashPower(72, false));
				appendBallNeckCommand(visual, command, sizeof(command));
				++defenderAdvanceSteps;
			}
			sendCmd(command);
			return;
		}

		if (partnerCloser && defenderAdvanceSteps >= 7 && !emergency) {
			makeDefenderReturnCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		const double homeMaximum = iPlayerId == 2 ? 23.0 : 21.0;
		if (visual.hasOwnGoal
				&& visual.ownGoalDistance > homeMaximum) {
			makeDefenderReturnCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}
		if (defenderAdvanceSteps > 0 && visual.ballDistance > 34.0) {
			makeDefenderReturnCommand(visual, command);
			appendBallNeckCommand(visual, command, sizeof(command));
			sendCmd(command);
			return;
		}

		if (absDouble(visual.ballDirection) > 18.0) {
			sprintf(command, "(turn %.1f)", visual.ballDirection);
		}
		else {
			sprintf(command, "(dash %d)", staminaDashPower(55, false));
			appendBallNeckCommand(visual, command, sizeof(command));
			if (defenderAdvanceSteps < 7) {
				++defenderAdvanceSteps;
			}
		}
		sendCmd(command);
	}

	void handleFieldPlayer(const VisualState &visual, int cycle) {
		char command[160];

		if (handleKickOff(visual, cycle)) {
			return;
		}

		if (isRestartHoldMode()) {
			gLastAttackAction = "restart_wait";
			double x = 0.0;
			double y = 0.0;
			restartFormationPosition(iPlayerId, x, y);
			sprintf(command, "(move %.1f %.1f)", x, y);
			defenderAdvanceSteps = 0;
			sendCmd(command);
			return;
		}

		if (iPlayerId == 2 || iPlayerId == 3) {
			handleDefender(visual, cycle);
			return;
		}

		const int passTarget = isFreshPassTarget(cycle);
		if (!visual.hasBall) {
			if (passTarget) {
				if (absDouble(lastPassSourceDirection) > 10.0) {
					sprintf(command, "(turn %.1f)",
							lastPassSourceDirection);
					lastPassSourceDirection = 0.0;
					gLastAttackAction = "receive_scan";
				}
				else {
					sprintf(command, "(dash 85)");
					gLastAttackAction = "receive_meet";
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
			sendCmd(command);
			return;
		}

		lastSeenBallCycle = cycle;
		lastSeenBallDirection = visual.ballDirection;
		const double ballDistance = visual.ballDistance;
		const double ballDirection = visual.ballDirection;
		const double chaseLimit = iPlayerId == 4 ? 42.0 : 34.0;

		const int mayChase = shouldChaseBall(visual, iPlayerId);
		printf("chase_debug: cycle=%d id=%d side=%d srvSide=%d playMode=%d ball=%.1f tm=%d chase=%d pass=%d reason=%s\n",
				cycle, iPlayerId, iSide, gMatchState.sideFromServer,
				static_cast<int>(gMatchState.playMode), ballDistance,
				visual.teammateCount, mayChase, passTarget, gLastChaseReason);
		int activeChase = stableForwardChaseDecision(mayChase,
				ballDistance, chaseLimit, cycle);
		double effectiveChaseLimit = chaseLimit;
		if (cycle - lastOwnTouchCycle >= 0
				&& cycle - lastOwnTouchCycle <= 6) {
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

		if (ballDistance > 0.95) {
			if (absDouble(ballDirection) > 18.0) {
				sprintf(command, "(turn %.1f)", ballDirection);
				gLastAttackAction = passTarget
						? "receive_turn" : "chase_turn";
			}
			else {
				int dashPower = 100;
				if (ballDistance < 4.0) {
					dashPower = passTarget ? 88 : 85;
				}
				dashPower = staminaDashPower(dashPower, true);
				if (cycle - lastChaseSayCycle >= 5) {
					sprintf(command, "(dash %d)(say c%d)", dashPower, iPlayerId);
					lastChaseSayCycle = cycle;
				}
				else {
					sprintf(command, "(dash %d)", dashPower);
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
