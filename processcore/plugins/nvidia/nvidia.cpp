/*
    SPDX-FileCopyrightText: 2019 David Edmundson <davidedmundson@kde.org>
    SPDX-FileCopyrightText: 2026 Juho Ovaska <ovaska.juho@gmail.com>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "nvidia.h"

#include <QDebug>

#include <KLocalizedString>
#include <KPluginFactory>

#include "Unit.h"

#include <qvariant.h>

#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <unordered_map>

#include <dlfcn.h>
#include <cassert>

#include "nvml.h"

using namespace KSysGuard;

class nvmlLib {
public:
    struct nvmlFuncs
    {
#define NV_FUNC(name) typeof(&::name) name;
NV_FUNC(nvmlInit)
NV_FUNC(nvmlShutdown)
NV_FUNC(nvmlDeviceGetCount)
NV_FUNC(nvmlDeviceGetHandleByIndex)
NV_FUNC(nvmlDeviceGetMemoryInfo)
NV_FUNC(nvmlDeviceGetProcessUtilization)
NV_FUNC(nvmlDeviceGetComputeRunningProcesses)
NV_FUNC(nvmlDeviceGetGraphicsRunningProcesses)
NV_FUNC(nvmlDeviceGetMPSComputeRunningProcesses)
#undef NV_FUNC
    };

    nvmlLib()
        : m_loaded(false), m_handle(nullptr), m_funcs{}
    {
    }

    ~nvmlLib()
    {
        if (m_loaded)
        {
            assert(getFuncs().nvmlShutdown != nullptr);
            getFuncs().nvmlShutdown();
            unloadLib();
        }
    }

    bool isLoaded()
    {
        return m_loaded;
    }

    bool tryLoad()
    {
        if (m_loaded)
        {
            return true;
        }

        void *handle = dlopen("libnvidia-ml.so", RTLD_NOW);
        if (handle == nullptr)
        {
            return false;
        }
    
        // try to dlsym() all NVML functions
        nvmlFuncs funcs{};
#define NV_STR(name) #name
#define NV_FUNC(name) do {                                            \
        auto *ptr = dlsym(handle, NV_STR(name));                      \
        if (ptr == nullptr) {                                         \
            goto _fail;                                               \
        } else {                                                      \
            funcs.name = reinterpret_cast<decltype(funcs.name)>(ptr); \
        }                                                             \
    } while (0);
NV_FUNC(nvmlInit)
NV_FUNC(nvmlShutdown)
NV_FUNC(nvmlDeviceGetCount)
NV_FUNC(nvmlDeviceGetHandleByIndex)
NV_FUNC(nvmlDeviceGetMemoryInfo)
NV_FUNC(nvmlDeviceGetProcessUtilization)
NV_FUNC(nvmlDeviceGetComputeRunningProcesses)
NV_FUNC(nvmlDeviceGetGraphicsRunningProcesses)
NV_FUNC(nvmlDeviceGetMPSComputeRunningProcesses)
#undef NV_FUNC
#undef NV_STR

        // initialize NVML here
        if (funcs.nvmlInit() != NVML_SUCCESS)
        {
            goto _fail;
        }

        // mark loaded
        m_funcs  = funcs;
        m_handle = handle;
        m_loaded = true;

        return true;
_fail:
        dlclose(handle);
        return false;
    }

    const nvmlFuncs &getFuncs() const
    {
        assert(m_loaded);
        return m_funcs;    
    }

private:
    void unloadLib()
    {
        if (m_loaded)
        {
            m_funcs = nvmlFuncs{};
            assert(m_handle != nullptr);
            dlclose(m_handle);
            m_handle = nullptr;
            m_loaded = false;
        }
    }

    bool m_loaded;
    void *m_handle;
    nvmlFuncs m_funcs;
};

void nvmlLibDeleter::operator()(nvmlLib *lib)
{
    delete lib;
}

class nvmlDetail {
    struct processInfo {
        unsigned int gpuUtil;
        unsigned long long memUsage;
    };

public:
    explicit nvmlDetail(const nvmlLib &lib)
        : m_lib(lib)
        , m_pidListSelect(false)
        , m_bufferSize(0)
    {
    }

    nvmlDetail(const nvmlDetail &other)
        : m_lib(other.m_lib)
        , m_pidListSelect(false)
        , m_bufferSize(0)
        , m_gpus(other.m_gpus)
    {
    }

    bool loadGpus()
    {
        clearGpus();

        unsigned int num;
        auto ret = getFuncs().nvmlDeviceGetCount(&num);
        if (ret != NVML_SUCCESS)
        {
            return false;
        }

        m_gpus.reserve(num);

        for (unsigned int i = 0; i < num; i++)
        {
            nvmlDevice_t gpu;
            ret = getFuncs().nvmlDeviceGetHandleByIndex(i, &gpu);
            if (ret != NVML_SUCCESS)
            {
                clearGpus();
                return false;
            }
            
            m_gpus.push_back(gpu);
        }

        return true;
    }

    void clearGpus()
    {
        m_gpus.clear();
    }

    void update()
    {
        m_pidListSelect = !m_pidListSelect;
        m_infoList.clear();
        pidsNow().clear();

        for (unsigned int i = 0; i < m_gpus.size(); i++)
        {
            auto &gpu = m_gpus[i];
            queryUtilization(gpu);
            queryMemoryUsage(gpu);            
        }

        m_pidListDelta.clear();
        std::set_difference(
            pidsOld().cbegin(), pidsOld().cend(),
            pidsNow().cbegin(), pidsNow().cend(),
            std::inserter(m_pidListDelta, m_pidListDelta.begin())
        );
    }

    void removedPids(std::function<void(unsigned int pid)> &&callback)
    {
        for (auto pid : m_pidListDelta)
        {
            callback(pid);
        }
    }

    void setUsage(std::function<void(unsigned int pid, unsigned int usage, unsigned int memory)> &&callback)
    {
        for (auto &item : m_infoList)
        {
            // report memory usage in MiB
            callback(item.first, item.second.gpuUtil, item.second.memUsage / (1024 * 1024));
        }
    }

private:

    const nvmlLib::nvmlFuncs &getFuncs()
    {
        return m_lib.getFuncs();
    }
    
    template <typename T>
    size_t bufferSize()
    {
        return m_bufferSize / sizeof(T);
    }

    template <typename T>
    T *reserveBuffer(unsigned int count)
    {
        count *= sizeof(T);
        if (count > m_bufferSize)
        {
            m_buffer = std::unique_ptr<char[]>(new char[count]);
            const_cast<unsigned int &>(m_bufferSize) = count; // only allow modification here!
        }
        return reinterpret_cast<T*>(&m_buffer[0]);
    }

    std::set<unsigned int> &pidsNow()
    {
        return m_pidListSelect ? m_pidList1 : m_pidList2;
    }

    std::set<unsigned int> &pidsOld()
    {
        return m_pidListSelect ? m_pidList2 : m_pidList1;
    }

    processInfo *getInfo(unsigned int pid)
    {
        // process is using GPU at the moment
        pidsNow().insert(pid);

        // find or insert into list
        auto it = m_infoList.find(pid);
        if (it != m_infoList.cend())
        {
            return &(*it).second;
        }
        else
        {
            auto ret = m_infoList.emplace(std::make_pair(pid, processInfo{}));
            if (ret.second)
            {
                return &(*ret.first).second;
            }
            else
            {
                assert(false);
                return nullptr;
            }
        }
    }

    void queryUtilization(nvmlDevice_t gpu)
    {
        unsigned int num;
        auto ret = getFuncs().nvmlDeviceGetProcessUtilization(gpu, nullptr, &num, 0);
        if (ret != NVML_ERROR_INSUFFICIENT_SIZE)
        {
            return;
        }
        
        auto *buf = reserveBuffer<nvmlProcessUtilizationSample_t>(num);
        num = bufferSize<nvmlProcessUtilizationSample_t>();
        
        ret = getFuncs().nvmlDeviceGetProcessUtilization(gpu, buf, &num, 0);
        if (ret != NVML_SUCCESS)
        {
            return;
        }

        for (unsigned int i = 0; i < num; i++)
        {
            auto &item = buf[i];
            processInfo *info = getInfo(item.pid);
            if (info != nullptr)
            {
                info->gpuUtil += static_cast<double>(item.smUtil) / static_cast<double>(m_gpus.size()) + 0.5;
            }
        }
    }

    void queryMemoryUsage(nvmlDevice_t gpu)
    {
        for (auto &func : { getFuncs().nvmlDeviceGetComputeRunningProcesses, getFuncs().nvmlDeviceGetGraphicsRunningProcesses, getFuncs().nvmlDeviceGetMPSComputeRunningProcesses })
        {
            unsigned int num = 0;
            auto ret = func(gpu, &num, nullptr);
            if (ret != NVML_ERROR_INSUFFICIENT_SIZE)
            {
                continue;
            }

            auto *buf = reserveBuffer<nvmlProcessInfo_t>(num);
            num = bufferSize<nvmlProcessInfo_t>();

            ret = func(gpu, &num, buf);
            if (ret != NVML_SUCCESS)
            {
                continue;
            }
            
            for (unsigned int i = 0; i < num; i++)
            {
                auto &item = buf[i];
                if (item.usedGpuMemory == NVML_VALUE_NOT_AVAILABLE)
                {
                    continue;
                }
                processInfo *info = getInfo(item.pid);
                if (info != nullptr)
                {
                    info->memUsage += item.usedGpuMemory;
                }
            }
        }
    }

    const nvmlLib &m_lib;

    // used to select between m_pidList1/2 to avoid moves/reallocations
    bool m_pidListSelect;

    // buffer used for NVML operations
    const unsigned int m_bufferSize;
    std::unique_ptr<char[]> m_buffer;

    std::vector<nvmlDevice_t> m_gpus;

    // need to be sorted for std::set_difference()
    std::set<unsigned int> m_pidList1;
    std::set<unsigned int> m_pidList2;

    // doesn't need to be sorted
    std::unordered_set<unsigned int> m_pidListDelta;

    std::unordered_map<unsigned int, processInfo> m_infoList;
};

void nvmlDetailDeleter::operator()(nvmlDetail *detail)
{
    delete detail;
}


NvidiaPlugin::NvidiaPlugin(QObject *parent, const QVariantList &args)
    : ProcessDataProvider(parent, args)
{
    m_nvmlLib = std::unique_ptr<nvmlLib, nvmlLibDeleter>(new nvmlLib);
    if (!m_nvmlLib->tryLoad()) {
        m_nvmlLib.reset();
        return;
    }

    m_usage = std::make_unique<ProcessAttribute>(QStringLiteral("nvidia_usage"), i18n("GPU Usage"), this);
    m_usage->setUnit(KSysGuard::UnitPercent);
    m_memory = std::make_unique<ProcessAttribute>(QStringLiteral("nvidia_memory"), i18n("GPU Memory"), this);
    m_memory->setUnit(KSysGuard::UnitMegaByte);

    addProcessAttribute(m_usage.get());
    addProcessAttribute(m_memory.get());
}

void NvidiaPlugin::handleEnabledChanged(bool enabled)
{
    if (!m_nvmlLib->isLoaded())
        return;

    if (enabled) {
        setup();
    } else {
        cleanup();
    }
}

void NvidiaPlugin::setup()
{
    m_nvmlDetail = std::unique_ptr<nvmlDetail, nvmlDetailDeleter>(new nvmlDetail(*m_nvmlLib.get()));
    m_nvmlDetail->loadGpus();
}

void NvidiaPlugin::cleanup()
{
    m_nvmlDetail.reset();
}

void NvidiaPlugin::update()
{
    if (!m_nvmlLib->isLoaded())
        return;

    m_nvmlDetail->update();

    // clear usage data for processes that have stopped using GPU resources but are still alive
    m_nvmlDetail->removedPids([&](unsigned int pid){
        auto *proc = getProcess(pid);
        if (proc != nullptr)
        {
            m_usage->clearData(proc);
            m_memory->clearData(proc);
        }
    });

    // set usage data for processes with utilization / memory usage
    m_nvmlDetail->setUsage([&](unsigned int pid, unsigned int usage, unsigned int memory){
        auto *proc = getProcess(pid);
        if (proc != nullptr)
        {
            m_usage->setData(proc, usage);
            m_memory->setData(proc, memory);
        }
    });
}

K_PLUGIN_FACTORY_WITH_JSON(PluginFactory, "nvidia.json", registerPlugin<NvidiaPlugin>();)

#include "nvidia.moc"
