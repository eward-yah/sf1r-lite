#include "LogServerProcess.h"

#include <iostream>

#include <log-manager/LogServerRequest.h>

namespace sf1r
{

#define RETURN_ON_FAILURE(condition)                                        \
if (! (condition))                                                          \
{                                                                           \
    std::cerr << "Log Server initialization assertion failed: " #condition  \
              << std::endl;                                                 \
    return false;                                                           \
}


LogServerProcess::LogServerProcess()
{
}

LogServerProcess::~LogServerProcess()
{
    stop();
}

bool LogServerProcess::init(const std::string& cfgFile)
{
    // Parse config first
    RETURN_ON_FAILURE(LogServerCfg::get()->parse(cfgFile));

    RETURN_ON_FAILURE(initRpcLogServer());
    RETURN_ON_FAILURE(initDriverLogServer());
    RETURN_ON_FAILURE(initDrum());

    return true;
}

void LogServerProcess::start()
{
    LOG(INFO) << "\tRPC Server    : port=" << rpcLogServer_->getPort();
    rpcLogServer_->start();

    LOG(INFO) << "\tDriver Server : port=" << driverLogServer_->getPort();
    driverLogServer_->start();
}

void LogServerProcess::join()
{
    rpcLogServer_->join();
    driverLogServer_->join();
}

void LogServerProcess::stop()
{
    if (rpcLogServer_)
        rpcLogServer_->stop();
    if (driverLogServer_)
        driverLogServer_->stop();
}

bool LogServerProcess::initRpcLogServer()
{
    rpcLogServer_.reset(
            new RpcLogServer(
                LogServerCfg::get()->getLocalHost(),
                LogServerCfg::get()->getRpcServerPort(),
                LogServerCfg::get()->getThreadNum()
            ));

    return rpcLogServer_;
}

bool LogServerProcess::initDriverLogServer()
{
    driverLogServer_.reset(
            new DriverLogServer(
                LogServerCfg::get()->getDriverServerPort(),
                LogServerCfg::get()->getThreadNum()
            ));

    if (driverLogServer_)
    {
        return driverLogServer_->init();
    }
    return false;
}

bool LogServerProcess::initDrum()
{
    drum_.reset(
            new DrumType(
                LogServerCfg::get()->getDrumName(),
                LogServerCfg::get()->getDrumNumBuckets(),
                LogServerCfg::get()->getDrumBucketBuffElemSize(),
                LogServerCfg::get()->getDrumBucketByteSize()
            ));

    return drum_;
}

}