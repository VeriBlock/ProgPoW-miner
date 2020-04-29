#pragma once

#include <libethcore/Farm.h>
#include <libethcore/Miner.h>
#include <jsonrpccpp/server.h>

using namespace jsonrpc;
using namespace dev;
using namespace dev::eth;
using namespace std::chrono;

class ApiServer : public AbstractServer<ApiServer>
{
public:
	ApiServer(AbstractServerConnector *conn, serverVersion_t type, Farm &farm, bool &readonly);
private:
	Farm &m_farm;
	void getMinerStat1(const Json::Value& request, Json::Value& response);
	void getMinerStatHR(const Json::Value& request, Json::Value& response);
	void doMinerRestart(const Json::Value& request, Json::Value& response);
	void doMinerReboot(const Json::Value& request, Json::Value& response);
};

