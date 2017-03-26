#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <limits.h>
#include <sys/resource.h>
#include <errno.h>

#include <string>
#include <fstream>
#include <condition_variable>
#include <thread>
#include <map>
#include <set>
#include <vector>

#include <sqlite3.h>

#include "src/common/util/util.h"
#include "src/common/messages/Message.h"
#include "src/common/tracing/Trace.h"
#include "src/common/errors/errors.h"
#include "src/common/const/Const.h"

#include "src/common/comm/ltfsdm.pb.h"
#include "src/common/comm/LTFSDmComm.h"

#include "src/connector/Connector.h"
#include "DataBase.h"
#include "Receiver.h"
#include "SubServer.h"
#include "Scheduler.h"
#include "TransRecall.h"
#include "Server.h"

std::atomic<bool> terminate;
std::mutex Server::termmtx;
std::condition_variable Server::termcond;

void Server::signalHandler(sigset_t set, long key)

{
	int sig;
	int requestNumber = ++globalReqNumber;

    while ( true ) {
        if ( sigwait(&set, &sig))
            continue;

        if ( sig == SIGPIPE ) {
            MSG(LTFSDMS0048E);
            continue;
        }

		MSG(LTFSDMS0049I, sig);

		break;
	}

	LTFSDmCommClient commCommand;

	try {
		commCommand.connect();
	}
	catch(int error) {
		return;
	}

	TRACE(Trace::little, requestNumber);
	LTFSDmProtocol::LTFSDmStopRequest *stopreq = commCommand.mutable_stoprequest();
	stopreq->set_key(key);
	stopreq->set_reqnumber(requestNumber);

	try {
		commCommand.send();
	}
	catch(int error) {
		return;
	}

	std::unique_lock<std::mutex> lock(termmtx);

	try {
		commCommand.recv();
	}
	catch(...) {
		return;
	}

	MSG(LTFSDMS0009I);
	terminate = true;
	lock.unlock();
	termcond.notify_one();

	return;
}

void Server::lockServer()

{
	int lockfd;

	if ( (lockfd = open(Const::SERVER_LOCK_FILE.c_str(), O_RDWR | O_CREAT, 0600)) == -1 ) {
		MSG(LTFSDMS0001E);
		TRACE(Trace::error, Const::SERVER_LOCK_FILE);
		TRACE(Trace::error, errno);
		throw(Error::LTFSDM_GENERAL_ERROR);
	}

	if ( flock(lockfd, LOCK_EX | LOCK_NB) == -1 ) {
		TRACE(Trace::error, errno);
		if ( errno == EWOULDBLOCK ) {
			MSG(LTFSDMS0002I);
			throw(Error::LTFSDM_OK);
		}
		else {
			MSG(LTFSDMS0001E);
			TRACE(Trace::error, errno);
			throw(Error::LTFSDM_GENERAL_ERROR);
		}
	}
}

void Server::writeKey()

{
	std::ofstream keyFile;

	keyFile.exceptions(std::ofstream::failbit | std::ofstream::badbit);

	try {
		keyFile.open(Const::KEY_FILE, std::fstream::out | std::fstream::trunc);
	}
	catch(...) {
		MSG(LTFSDMS0003E);
		throw(Error::LTFSDM_GENERAL_ERROR);
	}

	srandom(time(NULL));
	key = random();
	keyFile << key << std::endl;

	keyFile.close();
}


void Server::initialize()

{
	if ( setrlimit(RLIMIT_NOFILE, &Const::NOFILE_LIMIT) == -1 ) {
		MSG(LTFSDMS0046E);
		throw(errno);
	}

	if ( setrlimit(RLIMIT_NPROC, &Const::NPROC_LIMIT) == -1 ) {
		MSG(LTFSDMS0046E);
		throw(errno);
	}

	lockServer();
	writeKey();

	try {
		DB.cleanup();
		DB.open();
		DB.createTables();
	}
	catch (int error) {
		MSG(LTFSDMS0014E);
		throw(error);
	}
}

void Server::daemonize()

{
	pid_t pid, sid;
	int dev_null;

	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}

	if (pid > 0) {
		throw(Error::LTFSDM_OK);
	}

	sid = setsid();
	if (sid < 0) {
		MSG(LTFSDMS0012E);
		throw(Error::LTFSDM_GENERAL_ERROR);
	}

	TRACE(Trace::little, "Server started");
	TRACE(Trace::little, getpid());

	messageObject.setLogType(Message::LOGFILE);

	/* redirect stdout to log file */
	if ( (dev_null = open("/dev/null", O_RDWR)) == -1 ) {
		MSG(LTFSDMS0013E);
		throw(Error::LTFSDM_GENERAL_ERROR);
	}
	dup2(dev_null, STDIN_FILENO);
	dup2(dev_null, STDOUT_FILENO);
	dup2(dev_null, STDERR_FILENO);
	close(dev_null);
}

void Server::run(Connector *connector, sigset_t set)

{
	SubServer subs;
	Scheduler sched;
	Receiver recv;
	TransRecall trec;

	terminate = false;

	subs.enqueue("Scheduler", &Scheduler::run, &sched, key);
	subs.enqueue("Receiver", &Receiver::run, &recv, key, connector);
	subs.enqueue("Signal Handler", &Server::signalHandler, set, key);
	subs.enqueue("RecallD", &TransRecall::run, &trec, connector);

	subs.waitAllRemaining();
}
