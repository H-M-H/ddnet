#pragma once

#include <chrono>
#include <future>

template <typename T>
class CFuture
{
public:
	CFuture(std::future<T>&& Fut) : m_Fut(std::move(Fut)) {}
	T Get() const { return m_Fut.get(); }
	bool Ready() const
	{
		return m_Fut.valid() && m_Fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
	}

private:
	std::shared_future<T> m_Fut;
};
