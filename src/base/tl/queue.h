#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

template <typename T>
class CQueue
{
public:
	void Push(const T& v)
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_Queue.push(v);
		m_CV.notify_one();
	}

	template <class... Args>
	void Emplace(Args&&... args)
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_Queue.emplace(std::forward<Args>(args)...);
		m_CV.notify_one();
	}

	template <typename F, typename... Args>
	void Consume(F&& Func, Args&&... args)
	{
		std::unique_lock<std::mutex> Lock(m_Mutex);
		while (m_Queue.empty())
			m_CV.wait(Lock);
		Func(m_Queue.front(), std::forward<Args>(args)...);
		m_Queue.pop();
	}

	template <typename F, typename... Args>
	bool TryConsume(F&& Func, Args&&... args)
	{
		std::unique_lock<std::mutex> Lock(m_Mutex, std::defer_lock);
		if (Lock.try_lock() && !m_Queue.empty())
		{
			Func(m_Queue.front(), std::forward<Args>(args)...);
			m_Queue.pop();
			return true;
		}
		return false;
	}

private:
	std::queue<T> m_Queue;
	std::condition_variable m_CV;
	std::mutex m_Mutex;
};
