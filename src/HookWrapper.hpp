//
// Created by 11252 on 2025/11/5.
//

#ifndef UNITYPLUGIN_HOOKWRAPPER_HPP
#define UNITYPLUGIN_HOOKWRAPPER_HPP

#include <MinHook.h>
#include <IUnityLog.h>

extern IUnityLog *g_Log;

template<typename T>
class HookWrapper {
public:
    explicit HookWrapper(LPVOID pTarget) : m_pTarget(pTarget), m_pOriginal(nullptr) {
    }

    bool CreateAndEnable(T pDetour) {
        const MH_STATUS createStatus = MH_CreateHook(
            m_pTarget,
            reinterpret_cast<LPVOID>(pDetour),
            reinterpret_cast<LPVOID *>(&m_pOriginal)
        );
        if (createStatus != MH_OK) {
            UNITY_LOG_ERROR(g_Log, "MH_CreateHook failure");
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(m_pTarget);
        if (enableStatus != MH_OK) {
            UNITY_LOG_ERROR(g_Log, "MH_EnableHook failure");
            MH_RemoveHook(m_pTarget);
            m_pOriginal = nullptr;
            return false;
        }

        return true;
    }

    bool Disable() const {
        const MH_STATUS status = MH_DisableHook(m_pTarget);
        if (status != MH_OK && status != MH_ERROR_DISABLED && status != MH_ERROR_NOT_CREATED) {
            UNITY_LOG_ERROR(g_Log, "MH_DisableHook failure");
            return false;
        }

        return true;
    }

    T GetOriginalPtr() const {
        return m_pOriginal;
    }

private:
    LPVOID m_pTarget;
    T m_pOriginal;
};


#endif //UNITYPLUGIN_HOOKWRAPPER_HPP
