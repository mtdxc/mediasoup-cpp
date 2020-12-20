
#include "Worker.hpp"
#include "Logger.hpp"
#include "Process.hpp"
#include "Mediasoup.hpp"
#include "AutoRelease.hpp"

#include <sstream>

namespace mediasoup
{

#define MEDIASOUP_VERSION "MEDIASOUP_VERSION=3.5.15"

// If env MEDIASOUP_WORKER_BIN is given, use it as worker binary.
// Otherwise if env MEDIASOUP_BUILDTYPE is 'Debug' use the Debug binary.
// Otherwise use the Release binary.
std::string Worker::s_workerBin = "";

std::vector<std::string> StringSplit(const std::string& s, const std::string& delim = ",")
{
	std::vector<std::string> elems;
	size_t pos = 0;
	size_t len = s.length();
	size_t delim_len = delim.length();
	if (delim_len == 0) {
		return elems;
	}

	while (pos < len) {
		int find_pos = s.find(delim, pos);
		if (find_pos < 0) {
			elems.push_back(s.substr(pos, len - pos));
			break;
		}
		elems.push_back(s.substr(pos, find_pos - pos));
		pos = find_pos + delim_len;
	}
	return elems;
}

void StaticExitCB(uv_process_t* process, int64_t exitStatus, int termSignal) {
	((Worker*)process->data)->OnExit(exitStatus, termSignal);
}

Worker::Worker(IWorker::Observer* obs, const WorkerSettings& workerSettings) {
	MS_lOGF();
	m_settings = workerSettings;

	getWorkBinPath();
	MS_lOGI("mediasoup-worker path:{}", s_workerBin);

	RegisterObserver(obs);
}

Worker::~Worker() {
	MS_lOGF();
	Close();
	UnregisterObserver();
}

bool Worker::Init() {
	MS_lOGF();

	std::string spawnBin = s_workerBin;
	std::vector<std::string> spawnArgs;

	if ("true" == Process::GetInstance().Environ("MEDIASOUP_USE_VALGRIND")) {
		spawnBin = Process::GetInstance().Environ("MEDIASOUP_VALGRIND_BIN").empty() ?
			"valgrind" : Process::GetInstance().Environ("MEDIASOUP_VALGRIND_BIN");

		if (!Process::GetInstance().Environ("MEDIASOUP_VALGRIND_OPTIONS").empty()) {
			std::vector<std::string> options = StringSplit(Process::GetInstance().Environ("MEDIASOUP_VALGRIND_OPTIONS"), " ");
			std::move(options.begin(), options.end(), std::back_inserter(spawnArgs));			
		}
		spawnArgs.push_back(spawnBin);
	}

	if (!m_settings.logLevel.empty()) {
		spawnArgs.push_back("--logLevel=" + m_settings.logLevel);
	}
	
	for (const auto& tag : m_settings.logTags) {
		if (tag.empty()) {
			continue;
		}
		spawnArgs.push_back("--logTags=" + tag);
	}

	spawnArgs.push_back("--rtcMinPort=" + std::to_string(m_settings.rtcMinPort));
	spawnArgs.push_back("--rtcMaxPort=" + std::to_string(m_settings.rtcMaxPort));

	if (!m_settings.dtlsCertificateFile.empty()) {
		spawnArgs.push_back("--dtlsCertificateFile=" + m_settings.dtlsCertificateFile);
	}

	if (!m_settings.dtlsPrivateKeyFile.empty()) {
		spawnArgs.push_back("--dtlsPrivateKeyFile=" + m_settings.dtlsPrivateKeyFile);
	}

	std::string strSpawnArgs;
	for (const auto& arg : spawnArgs) {
		strSpawnArgs += arg + " ";
	}

	MS_lOGD("spawning worker process : {} {}", spawnBin, strSpawnArgs);

	char** args = new char*[spawnArgs.size() + 1];
	AutoRelease releaeArgs([&args]() { delete[] args; });
	for (int i = 0; i < spawnArgs.size(); i++) {
		args[i] = const_cast<char*>(spawnArgs[i].c_str());
	}
	args[spawnArgs.size()] = nullptr;
	
	uv_process_options_t options = {};
	options.file = spawnBin.c_str();
	options.args = args;

	// env
	char* env[2];
	env[0] = MEDIASOUP_VERSION;
	env[1] = nullptr;
	options.env = env;

	m_childStdOut.reset(new UVPipeWrapper(this, 1024, UVPipeWrapper::Role::CONSUMER));
	m_childStdOut->Init();
	m_childStdErr.reset(new UVPipeWrapper(this, 1024, UVPipeWrapper::Role::CONSUMER));
	m_childStdErr->Init();

	m_channel.reset(new Channel(this));
	m_channel->Init();

	// options
	options.flags = UV_PROCESS_DETACHED;
	options.exit_cb = StaticExitCB;

	// fd 0 (stdin)   : Just ignore it.
	// fd 1 (stdout)  : Pipe it for 3rd libraries that log their own stuff.
	// fd 2 (stderr)  : Same as stdout.
	// fd 3 (channel) : Producer Channel fd.
	// fd 4 (channel) : Consumer Channel fd.
	uv_stdio_container_t childStdio[5];
	childStdio[0].flags = UV_IGNORE;
	// UV_WRITABLE_PIPE/UV_READABLE_PIPE is relative to the child process
	childStdio[1].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);	
	childStdio[1].data.stream = (uv_stream_t*)m_childStdOut->GetPipe();
	childStdio[2].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
	childStdio[2].data.stream = (uv_stream_t*)m_childStdErr->GetPipe();
	childStdio[3].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_READABLE_PIPE);
	childStdio[3].data.stream = (uv_stream_t*)m_channel->GetProducer()->GetPipe();
	childStdio[4].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
	childStdio[4].data.stream = (uv_stream_t*)m_channel->GetConsumer()->GetPipe();
	options.stdio = childStdio;
	options.stdio_count = 5;
	
	m_child = new uv_process_t;
	m_child->data = (void*)this;

	int ret = uv_spawn(Mediasoup::GetInstance().GetLoop(), m_child, &options);
	if (0 != ret) {
		Close();
		MS_lOGE("uv_spawn failed:{}", uv_strerror(ret));
		NotifyObserver(std::bind(&IWorker::Observer::OnFailure, std::placeholders::_1, uv_strerror(ret)));
		return false;
	}

	m_childStdOut->Start();
	m_childStdErr->Start();
	m_channel->Start();
	
	m_channel->SetPid(m_child->pid);
	m_pid = m_child->pid;

	return true;
}

void Worker::OnRead(UVPipeWrapper* pipe, uint8_t* data, size_t len) {
	std::string s((char*)data, len);
	if (pipe == m_childStdOut.get()) {
		MS_lOGD("Worker::OnRead: {}", s);
	}

	if (pipe == m_childStdErr.get()) {
		MS_lOGE("Worker::OnRead: {}", s);
	}
}

void Worker::OnClose(UVPipeWrapper* pipe) {

}

void Worker::OnRunning() {
	m_spawnDone = true;
	NotifyObserver(std::bind(&IWorker::Observer::OnSuccess, std::placeholders::_1));
}

void Worker::OnMsg(std::string targetId, std::string event, std::string data) {

}

void Worker::getWorkBinPath() {
	MS_lOGF();

	if (!s_workerBin.empty()) {
		return;
	}

	std::string workerBinPath = Process::GetInstance().Environ("MEDIASOUP_WORKER_BIN");
	if (!workerBinPath.empty()) {
		s_workerBin = workerBinPath;
		return;
	}

	std::string strexePath = Process::GetInstance().ExePath();

	// replace all '\' with '/'
	while (true) {
		std::string::size_type pos(0);
		if ((pos = strexePath.find("\\")) != std::string::npos) {
			strexePath.replace(pos, 1, "/");
			continue;
		}
		break;
	}
	strexePath = strexePath.substr(0, strexePath.rfind('/') + 1);

	s_workerBin += strexePath + "mediasoup-worker";
}

void Worker::Close() {
	MS_lOGF();
	MS_ASSERT_R_LOGI(m_child, "m_child is nullptr");

	// Kill the worker process.
	m_child->close_cb = nullptr;
	m_child->exit_cb = nullptr;
	uv_close((uv_handle_t*)m_child, nullptr);
	delete m_child;
	m_child = nullptr;

	if (m_channel) {
		m_channel.reset(nullptr);
	}

	// clear router

	// emit close
	NotifyObserver(std::bind(&IWorker::Observer::OnClose, std::placeholders::_1));
}

void Worker::OnExit(int64_t exitStatus, int termSignal) {
	if (!m_spawnDone && exitStatus == 42) {
		m_spawnDone = true;
		MS_lOGE("worker process failed due to wrong settings [pid:{}]", m_pid);
		NotifyObserver(std::bind(&IWorker::Observer::OnFailure, std::placeholders::_1, "wrong settings"));
		return;
	}

	MS_lOGE("worker process died unexpectedly [pid:{}, code:{}, signal:{}]", m_pid, exitStatus, termSignal);

	std::string error = "${ this._pid }, code : ${ code }, signal : ${ signal }]";
	std::stringstream in(error);
	in << "[pid:" << m_pid << ", code:" << exitStatus << ", signal:" << termSignal << "]";
	NotifyObserver(std::bind(&IWorker::Observer::OnFailure, std::placeholders::_1, error));
}

}
