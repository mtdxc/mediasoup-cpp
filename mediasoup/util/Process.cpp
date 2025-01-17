
#include "Process.hpp"
#include "Logger.hpp"

namespace mediasoup
{

Process::Process() {
	MS_lOGF();
	uv_env_item_t* envItem = nullptr;
	int envCount = 0;
	int ret = uv_os_environ(&envItem, &envCount);
	MS_ASSERT_R_LOGE(0 == ret, "uv_os_environ failed:{}", ret);
	if (envItem) {
		for (int i = 0; i < envCount; i++) {
			m_mapEnvs[envItem[i].name] = envItem[i].value;
		}
		uv_os_free_environ(envItem, envCount);
		envItem = nullptr;
		envCount = 0;
	}
}

Process::~Process() {
	MS_lOGF();
	m_mapEnvs.clear();
}

const std::string& Process::Environ(const std::string& name) {
	MS_lOGF();
	if (m_mapEnvs.find(name) == m_mapEnvs.end()) {
		return m_emptyEnv;
	}
    
	return m_mapEnvs[name];
}

const std::string& Process::ExePath() {
	#ifndef MAX_PATH
		#define MAX_PATH PATH_MAX
	#endif
	MS_lOGF();
	if (m_exePath.empty()) {
		char exePath[MAX_PATH] = { 0 };
		size_t exePathLen = MAX_PATH;
		uv_exepath(exePath, &exePathLen);
		m_exePath = exePath;
	}

	return m_exePath;
}

}
