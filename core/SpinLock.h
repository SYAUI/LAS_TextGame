#pragma once
#include <atomic>

class SpinLock {
public:
    void lock() noexcept {
        // 先试一次无竞争路径
        if (!m_flag.exchange(true, std::memory_order_acquire))
            return;
        // 有竞争：自旋 + 让出 CPU
        while (m_flag.exchange(true, std::memory_order_acquire)) {
            while (m_flag.load(std::memory_order_relaxed))
                /* 可加 _mm_pause() / yield */;
        }
    }
    void unlock() noexcept {
        m_flag.store(false, std::memory_order_release);
    }
private:
    std::atomic<bool> m_flag{ false };
};

class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& l) : m_l(l) { m_l.lock(); }
    ~SpinLockGuard() { m_l.unlock(); }
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;
private:
    SpinLock& m_l;
};