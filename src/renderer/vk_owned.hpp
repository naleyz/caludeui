#pragma once

#include <functional>
#include <utility>
#include <vulkan/vulkan.h>

// RAII wrapper for a Vulkan handle + its destroy function. Declare members
// outermost-first (matching Vulkan's teardown order) and they clean up
// automatically in reverse declaration order.
template<typename T>
class VkOwned
{
public:
    using Deleter = std::function<void(T)>;

    VkOwned() = default;
    VkOwned(T handle, Deleter deleter)
        : m_handle(handle), m_deleter(std::move(deleter)) {}

    ~VkOwned() { reset(); }

    VkOwned(const VkOwned&)            = delete;
    VkOwned& operator=(const VkOwned&) = delete;

    VkOwned(VkOwned&& o) noexcept
        : m_handle(std::exchange(o.m_handle, VK_NULL_HANDLE))
        , m_deleter(std::move(o.m_deleter)) {}

    VkOwned& operator=(VkOwned&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            m_handle  = std::exchange(o.m_handle, VK_NULL_HANDLE);
            m_deleter = std::move(o.m_deleter);
        }
        return *this;
    }

    void reset()
    {
        if (m_handle != VK_NULL_HANDLE && m_deleter)
        {
            m_deleter(m_handle);
            m_handle = VK_NULL_HANDLE;
        }
    }

    T        get()      const { return m_handle; }
    operator T()         const { return m_handle; }

private:
    T       m_handle = VK_NULL_HANDLE;
    Deleter m_deleter;
};
