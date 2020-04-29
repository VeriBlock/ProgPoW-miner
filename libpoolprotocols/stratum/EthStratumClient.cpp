#include "EthStratumClient.h"
#include <libdevcore/Log.h>
#include <libethash/endian.h>
#include <ethminer-buildinfo.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

using boost::asio::ip::tcp;


static void diffToTarget(uint32_t *target, double diff)
{
	uint32_t target2[8];
	uint64_t m;
	int k;

	for (k = 6; k > 0 && diff > 1.0; k--)
		diff /= 4294967296.0;
	m = (uint64_t)(4294901760.0 / diff);
	if (m == 0 && k == 6)
		memset(target2, 0xff, 32);
	else {
		memset(target2, 0, 32);
		target2[k] = (uint32_t)m;
		target2[k + 1] = (uint32_t)(m >> 32);
	}

	for (int i = 0; i < 32; i++)
		((uint8_t*)target)[31 - i] = ((uint8_t*)target2)[i];
}


EthStratumClient::EthStratumClient(int const & worktimeout, string const & email, bool const & submitHashrate) : PoolClient(),
        m_socket(nullptr),
	m_worktimer(m_io_service),
	m_responsetimer(m_io_service),
	m_hashrate_event(m_io_service),
        m_resolver(m_io_service)
{
	m_authorized = false;
	m_pending = 0;
	m_worktimeout = worktimeout;

	m_email = email;

	m_submit_hashrate = submitHashrate;
	m_submit_hashrate_id = h256::random().hex();
}

EthStratumClient::~EthStratumClient()
{
	m_io_service.stop();
	m_serviceThread.join();
}

void EthStratumClient::connect()
{
	m_connection = m_conn;

	m_authorized = false;
	m_connected.store(false, std::memory_order_relaxed);

	stringstream ssPort;
	ssPort << m_connection.Port();
	tcp::resolver::query q(m_connection.Host(), ssPort.str());

	//cnote << "Resolving stratum server " + m_connection.host + ":" + m_connection.port;

	if (m_connection.SecLevel() != SecureLevel::NONE) {

		boost::asio::ssl::context::method method = boost::asio::ssl::context::tls;
		if (m_connection.SecLevel() == SecureLevel::TLS12)
			method = boost::asio::ssl::context::tlsv12;

		boost::asio::ssl::context ctx(method);
		m_securesocket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >(m_io_service, ctx);
		m_socket = &m_securesocket->next_layer();

		if (m_connection.SecLevel() != SecureLevel::ALLOW_SELFSIGNED) {
			m_securesocket->set_verify_mode(boost::asio::ssl::verify_peer);

#ifdef _WIN32
			HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
			if (hStore == NULL) {
				return;
			}

			X509_STORE *store = X509_STORE_new();
			PCCERT_CONTEXT pContext = NULL;
			while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != NULL) {
				X509 *x509 = d2i_X509(NULL,
					(const unsigned char **)&pContext->pbCertEncoded,
					pContext->cbCertEncoded);
				if (x509 != NULL) {
					X509_STORE_add_cert(store, x509);
					X509_free(x509);
				}
			}

			CertFreeCertificateContext(pContext);
			CertCloseStore(hStore, 0);

			SSL_CTX_set_cert_store(ctx.native_handle(), store);
#else
			char *certPath = getenv("SSL_CERT_FILE");
			try {
				ctx.load_verify_file(certPath ? certPath : "/etc/ssl/certs/ca-certificates.crt");
			}
			catch (...) {
				cwarn << "Failed to load ca certificates. Either the file '/etc/ssl/certs/ca-certificates.crt' does not exist";
				cwarn << "or the environment variable SSL_CERT_FILE is set to an invalid or inaccessable file.";
				cwarn << "It is possible that certificate verification can fail.";
			}
#endif
		}
	}
	else {
	  m_nonsecuresocket = std::make_shared<boost::asio::ip::tcp::socket>(m_io_service);
	  m_socket = m_nonsecuresocket.get();
	}

	// Activate keep alive to detect disconnects
	unsigned int keepAlive = 10000;

#if defined _WIN32 || defined WIN32 || defined OS_WIN64 || defined _WIN64 || defined WIN64 || defined WINNT
	int32_t timeout = keepAlive;
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	struct timeval tv;
	tv.tv_sec = keepAlive / 1000;
	tv.tv_usec = keepAlive % 1000;
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

	m_resolver.async_resolve(q, boost::bind(&EthStratumClient::resolve_handler,
		this, boost::asio::placeholders::error,
		boost::asio::placeholders::iterator));

    if (m_serviceThread.joinable())
	{
		// If the service thread have been created try to reset the service.
		m_io_service.reset();
	}
	else
	{
		// Otherwise, if the first time here, create new thread.
		m_serviceThread = std::thread{boost::bind(&boost::asio::io_service::run, &m_io_service)};
	}
}

#define BOOST_ASIO_ENABLE_CANCELIO 

void EthStratumClient::disconnect()
{
	m_worktimer.cancel();
	m_responsetimer.cancel();
	m_response_pending = false;
	m_linkdown = true;

	try {
		if (m_connection.SecLevel() != SecureLevel::NONE) {
			boost::system::error_code sec;
			m_securesocket->shutdown(sec);
		}

		m_socket->close();
		m_io_service.stop();
	}
	catch (std::exception const& _e) {
		cwarn << "Error while disconnecting:" << _e.what();
	}

        if (m_connection.SecLevel() != SecureLevel::NONE) {
                if (m_securesocket)
                        m_securesocket = nullptr;
        }
        else {
                if (m_socket)
                        m_socket = nullptr;
        }

	m_authorized = false;
	m_connected.store(false, std::memory_order_relaxed);

	if (m_onDisconnected) {
		m_onDisconnected();
	}
}

void EthStratumClient::resolve_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	dev::setThreadName("stratum");
	if (!ec)
	{
		//cnote << "Connecting to stratum server " + m_connection.Host() + ":" + m_connection.Port();
		tcp::resolver::iterator end;
		async_connect(*m_socket, i, end, boost::bind(&EthStratumClient::connect_handler,
						this, boost::asio::placeholders::error,
						boost::asio::placeholders::iterator));
	}
	else
	{
		stringstream ss;
		ss << "Could not resolve host " << m_connection.Host() << ':' << m_connection.Port() << ", " << ec.message();
		cwarn << ss.str();
		disconnect();
	}
}

void EthStratumClient::reset_work_timeout()
{
	m_worktimer.cancel();
	m_worktimer.expires_from_now(boost::posix_time::seconds(m_worktimeout));
	m_worktimer.async_wait(boost::bind(&EthStratumClient::work_timeout_handler, this, boost::asio::placeholders::error));
}

void EthStratumClient::async_write_with_response()
{
	if (m_connection.SecLevel() != SecureLevel::NONE) {
		async_write(*m_securesocket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
				boost::asio::placeholders::error));
	}
	else {
		async_write(*m_nonsecuresocket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleResponse, this,
				boost::asio::placeholders::error));
	}
}

void EthStratumClient::connect_handler(const boost::system::error_code& ec, tcp::resolver::iterator i)
{
	(void)i;

	dev::setThreadName("stratum");
	
	if (!ec)
	{
		m_connected.store(true, std::memory_order_relaxed);
		m_linkdown = false;

		//cnote << "Connected to stratum server " + i->host_name() + ":" + m_connection.port;
		if (m_onConnected) {
			m_onConnected();
		}

		if (m_connection.SecLevel() != SecureLevel::NONE) {
			boost::system::error_code hec;
			m_securesocket->handshake(boost::asio::ssl::stream_base::client, hec);
			if (hec) {
				cwarn << "SSL/TLS Handshake failed: " << hec.message();
				if (hec.value() == 337047686) { // certificate verification failed
					cwarn << "This can have multiple reasons:";
					cwarn << "* Root certs are either not installed or not found";
					cwarn << "* Pool uses a self-signed certificate";
					cwarn << "Possible fixes:";
					cwarn << "* Make sure the file '/etc/ssl/certs/ca-certificates.crt' exists and is accessible";
					cwarn << "* Export the correct path via 'export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt' to the correct file";
					cwarn << "  On most systems you can install the 'ca-certificates' package";
					cwarn << "  You can also get the latest file here: https://curl.haxx.se/docs/caextract.html";
					cwarn << "* Disable certificate verification all-together via command-line option.";
				}
				disconnect();
				return;
			}
		}

		// Successfully connected so we start our work timeout timer
		reset_work_timeout();

		std::ostream os(&m_requestBuffer);

		string user;
		size_t p;

		switch (m_connection.Version()) {
			case EthStratumClient::STRATUM:
				m_authorized = true;
				os << "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": []}\n";
				break;
			case EthStratumClient::ETHPROXY:
				p = m_connection.User().find_first_of(".");
				user = m_connection.User().substr(0, p);
				if (p + 1 <= m_connection.User().length())
					m_worker = m_connection.User().substr(p + 1);
				else
					m_worker = "";

				if (m_email.empty())
				{
					os << "{\"id\": 1, \"worker\":\"" << m_worker << "\", \"method\": \"eth_submitLogin\", \"params\": [\"" << user << "\"]}\n";
				}
				else
				{
					os << "{\"id\": 1, \"worker\":\"" << m_worker << "\", \"method\": \"eth_submitLogin\", \"params\": [\"" << user << "\", \"" << m_email << "\"]}\n";
				}
				break;
			case EthStratumClient::ETHEREUMSTRATUM:
				m_authorized = true;
				os << "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": [\"ethminer/" << ethminer_get_buildinfo()->project_version << "\",\"EthereumStratum/1.0.0\"]}\n";
				break;
		}

		async_write_with_response();
	}
	else
	{
		stringstream ss;
		ss << "Could not connect to stratum server " << m_connection.Host() << ':' << m_connection.Port() << ", " << ec.message();
		cwarn << ss.str();
		disconnect();
	}

}

void EthStratumClient::readline() {
	x_pending.lock();
	if (m_pending == 0) {
		if (m_connection.SecLevel() != SecureLevel::NONE) {
			async_read_until(*m_securesocket, m_responseBuffer, "\n",
				boost::bind(&EthStratumClient::readResponse, this,
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		}
		else {
			async_read_until(*m_socket, m_responseBuffer, "\n",
				boost::bind(&EthStratumClient::readResponse, this,
					boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		}
	
		m_pending++;
		
	}
	x_pending.unlock();
}

void EthStratumClient::handleHashrateResponse(const boost::system::error_code& ec) {
	(void)ec;
}

void EthStratumClient::handleResponse(const boost::system::error_code& ec) {
	if (!ec)
	{
		readline();
	}
	else
	{
		dev::setThreadName("stratum");
		cwarn << "Handle response failed: " + ec.message();
	}
}

void EthStratumClient::readResponse(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	dev::setThreadName("stratum");
	x_pending.lock();
	m_pending = m_pending > 0 ? m_pending - 1 : 0;
	x_pending.unlock();

	if (!ec && bytes_transferred)
	{
		std::istream is(&m_responseBuffer);
		std::string response;
		getline(is, response);

		if (!response.empty() && response.front() == '{' && response.back() == '}') 
		{
			Json::Value responseObject;
			Json::Reader reader;
			if (reader.parse(response.c_str(), responseObject))
			{
				processReponse(responseObject);
			}
			else 
			{
				cwarn << "Parse response failed: " + reader.getFormattedErrorMessages();
			}
		}
		else if (m_connection.Version() != EthStratumClient::ETHPROXY)
		{
			cwarn << "Discarding incomplete response";
		}
		if (m_connected.load(std::memory_order_relaxed))
			readline();
	}
	else
	{
		if (m_connected.load(std::memory_order_relaxed)) {
			cwarn << "Read response failed: " + ec.message();
			disconnect();
		}
	}
}

void EthStratumClient::processExtranonce(std::string& enonce)
{
	m_extraNonceHexSize = enonce.length();

	cnote << "Extranonce set to " + enonce;

	for (int i = enonce.length(); i < 16; ++i) enonce += "0";
	m_extraNonce = h64(enonce);
}

void EthStratumClient::processReponse(Json::Value& responseObject)
{
	Json::Value error = responseObject.get("error", {});
	if (error.isArray())
	{
		cnote << error.get(1, "Unknown error").asString();
	}
	std::ostream os(&m_requestBuffer);
	Json::Value params;
	int id = responseObject.get("id", Json::Value::null).asInt();
	switch (id)
	{
		case 1:
		if (m_connection.Version() == EthStratumClient::ETHEREUMSTRATUM)
		{
			m_nextWorkDifficulty = 1;
			params = responseObject.get("result", Json::Value::null);
			if (params.isArray())
			{
				std::string enonce = params.get((Json::Value::ArrayIndex)1, "").asString();
				processExtranonce(enonce);
			}

			os << "{\"id\": 2, \"method\": \"mining.extranonce.subscribe\", \"params\": []}\n";
		}
		if (m_connection.Version() != EthStratumClient::ETHPROXY)
		{
			cnote << "Subscribed to stratum server";
			os << "{\"id\": 3, \"method\": \"mining.authorize\", \"params\": [\"" << m_connection.User() << "\",\"" << m_connection.Pass() << "\"]}\n";
		}
		else
		{
			m_authorized = true;
			os << "{\"id\": 5, \"method\": \"eth_getWork\", \"params\": []}\n"; // not strictly required but it does speed up initialization
		}

		async_write_with_response();

		break;
	case 2:
		// nothing to do...
		break;
	case 3:
		m_authorized = responseObject.get("result", Json::Value::null).asBool();
		if (!m_authorized)
		{
			cnote << "Worker not authorized:" + m_connection.User();
			disconnect();
			return;
		}
		cnote << "Authorized worker " + m_connection.User();
		break;
	case 4:
		{
			m_responsetimer.cancel();
			m_response_pending = false;
			if (responseObject.get("result", false).asBool()) {
				if (m_onSolutionAccepted) {
					m_onSolutionAccepted(m_stale);
				}
			}
			else {
				if (m_onSolutionRejected) {
					m_onSolutionRejected(m_stale);
				}
			}
		}
		break;
	default:
		string method, workattr;
		unsigned index;
		if (m_connection.Version() != EthStratumClient::ETHPROXY)
		{
			method = responseObject.get("method", "").asString();
			workattr = "params";
			index = 1;
		}
		else
		{
			method = "mining.notify";
			workattr = "result";
			index = 0;
		}

		if (method == "mining.notify")
		{
			params = responseObject.get(workattr.c_str(), Json::Value::null);
			if (params.isArray())
			{
				string job = params.get((Json::Value::ArrayIndex)0, "").asString();
				if (m_response_pending)
					m_stale = true;
				if (m_connection.Version() == EthStratumClient::ETHEREUMSTRATUM)
				{
					string sSeedHash = params.get(1, "").asString();
					string sHeaderHash = params.get(2, "").asString();
					uint64_t iBlockHeight = params.get(3, "").asInt64();

					if (sHeaderHash != "" && sSeedHash != "")
					{
						reset_work_timeout();

						m_current.header = h256(sHeaderHash);
						m_current.epoch = EthashAux::toEpoch(h256(sSeedHash));
						m_current.height = iBlockHeight;
						m_current.boundary = h256();
						diffToTarget((uint32_t*)m_current.boundary.data(), m_nextWorkDifficulty);
						m_current.startNonce = ethash_swap_u64(*((uint64_t*)m_extraNonce.data()));
						m_current.exSizeBits = m_extraNonceHexSize * 4;
						m_current.job_len = job.size();
						if (m_connection.Version() == EthStratumClient::ETHEREUMSTRATUM)
							job.resize(64, '0');
						m_current.job = h256(job);

						if (m_onWorkReceived) {
							m_onWorkReceived(m_current);
						}
					}
				}
				else
				{
					string sHeaderHash = params.get((Json::Value::ArrayIndex)index++, "").asString();
					string sSeedHash = params.get((Json::Value::ArrayIndex)index++, "").asString();
					string sShareTarget = params.get((Json::Value::ArrayIndex)index++, "").asString();
					uint64_t iBlockHeight = params.get((Json::Value::ArrayIndex)index++, "").asInt64();

					// coinmine.pl fix
					int l = sShareTarget.length();
					if (l < 66)
						sShareTarget = "0x" + string(66 - l, '0') + sShareTarget.substr(2);


					if (sHeaderHash != "" && sSeedHash != "" && sShareTarget != "")
					{
						h256 headerHash = h256(sHeaderHash);

						if (headerHash != m_current.header)
						{
							reset_work_timeout();

							m_current.header = h256(sHeaderHash);
							m_current.epoch = EthashAux::toEpoch(h256(sSeedHash));
							m_current.boundary = h256(sShareTarget);
							m_current.height = iBlockHeight;
							m_current.job = h256(job);

							if (m_onWorkReceived) {
								m_onWorkReceived(m_current);
							}
						}
					}
				}
			}
		}
		else if (method == "mining.set_difficulty" && m_connection.Version() == EthStratumClient::ETHEREUMSTRATUM)
		{
			params = responseObject.get("params", Json::Value::null);
			if (params.isArray())
			{
				m_nextWorkDifficulty = params.get((Json::Value::ArrayIndex)0, 1).asDouble();
				if (m_nextWorkDifficulty <= 0.0001) m_nextWorkDifficulty = 0.0001;
				cnote << "Difficulty set to "  << m_nextWorkDifficulty;
			}
		}
		else if (method == "mining.set_extranonce" && m_connection.Version() == EthStratumClient::ETHEREUMSTRATUM)
		{
			params = responseObject.get("params", Json::Value::null);
			if (params.isArray())
			{
				std::string enonce = params.get((Json::Value::ArrayIndex)0, "").asString();
				processExtranonce(enonce);
			}
		}
		else if (method == "client.get_version")
		{
			os << "{\"error\": null, \"id\" : " << id << ", \"result\" : \"" << ethminer_get_buildinfo()->project_version << "\"}\n";
			async_write_with_response();
		}
		break;
	}

}

void EthStratumClient::hashrate_event_handler(const boost::system::error_code& ec)
{
	if (ec || m_linkdown) {
		return;
	}

	// There is no stratum method to submit the hashrate so we use the rpc variant.
	string json = "{\"id\": 6, \"jsonrpc\":\"2.0\", \"method\": \"eth_submitHashrate\", \"params\": [\"" + m_rate + "\",\"0x" + this->m_submit_hashrate_id + "\"]}\n";
	std::ostream os(&m_requestBuffer);
	os << json;

	if (m_connection.SecLevel() != SecureLevel::NONE)
		async_write(*m_securesocket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleHashrateResponse, this, boost::asio::placeholders::error));
	else
		async_write(*m_nonsecuresocket, m_requestBuffer,
			boost::bind(&EthStratumClient::handleHashrateResponse, this, boost::asio::placeholders::error));
}

void EthStratumClient::work_timeout_handler(const boost::system::error_code& ec) {
	if (!ec) {
		cwarn << "No new work received in " << m_worktimeout << " seconds.";
		disconnect();
	}
}

void EthStratumClient::response_timeout_handler(const boost::system::error_code& ec) {
	if (!ec) {
		cwarn << "No no response received in 2 seconds.";
		disconnect();
	}
}

void EthStratumClient::submitHashrate(string const & rate) {
	if (!m_submit_hashrate || m_linkdown) {
		return;
	}

	m_rate = rate;
	m_hashrate_event.cancel();
	m_hashrate_event.expires_from_now(boost::posix_time::milliseconds(100));
	m_hashrate_event.async_wait(boost::bind(&EthStratumClient::hashrate_event_handler, this, boost::asio::placeholders::error));
}

void EthStratumClient::submitSolution(Solution solution) {

	string nonceHex = toHex(solution.nonce);
	string json;

	m_responsetimer.cancel();

	switch (m_connection.Version()) {
		case EthStratumClient::STRATUM:
			json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" +
				m_connection.User() + "\",\"" + solution.work.job.hex() + "\",\"0x" +
				nonceHex + "\",\"0x" + solution.work.header.hex() + "\",\"0x" +
				solution.mixHash.hex() + "\"]}\n";
			break;
		case EthStratumClient::ETHPROXY:
			json = "{\"id\": 4, \"worker\":\"" +
				m_worker + "\", \"method\": \"eth_submitWork\", \"params\": [\"0x" +
				nonceHex + "\",\"0x" + solution.work.header.hex() + "\",\"0x" +
				solution.mixHash.hex() + "\"]}\n";
			break;
		case EthStratumClient::ETHEREUMSTRATUM:
			json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" +
				m_connection.User() + "\",\"" + solution.work.job.hex().substr(0, solution.work.job_len) + "\",\"" +
				nonceHex.substr(m_extraNonceHexSize, 16 - m_extraNonceHexSize) + "\"]}\n";
			break;
	}
	std::ostream os(&m_requestBuffer);
	os << json;
	m_stale = solution.stale;

	async_write_with_response();

	m_response_pending = true;
	m_responsetimer.expires_from_now(boost::posix_time::seconds(2));
	m_responsetimer.async_wait(boost::bind(&EthStratumClient::response_timeout_handler, this, boost::asio::placeholders::error));
}

