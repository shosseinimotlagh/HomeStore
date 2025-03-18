#pragma once

#ifdef _PRERELEASE
#include <functional>
#include <sisl/utility/urcu_helper.hpp>
#include <iomgr/iomgr_flip.hpp>

namespace homestore {

class CrashSimulator {
public:
    CrashSimulator(std::function< void(bool) > cb = nullptr) : m_restart_cb{std::move(cb)} {}
    ~CrashSimulator() = default;

    void crash(bool skip_crash=false) {
        if(skip_crash) {
            LOGINFO("Skipping crash");
            m_restart_cb(true);
            return;
        }
        if (m_restart_cb) {
            LOGINFO("restarting due to crash");
            m_crashed.update([](auto* s) { *s = true; });

            // We can restart on a new thread to allow other operations to continue
            std::thread t([cb = std::move(m_restart_cb)]() {
                // Restart could destroy this pointer, so we are storing in local variable and then calling.
                cb(false);
            });
            t.detach();
        } else {
            raise(SIGKILL);
        }
    }

    bool is_crashed() const { return *(m_crashed.access().get()); }
    void skip_crash()
    {
        crash(true);
    }
    bool crash_if_flip_set(const std::string& flip_name) {
        if (iomgr_flip::instance()->test_flip(flip_name)) {
            this->crash();
            return true;
        } else {
            return false;
        }
    }

private:
    std::function< void(bool) > m_restart_cb{nullptr};
    sisl::urcu_scoped_ptr< bool > m_crashed;
};
} // namespace homestore
#endif
