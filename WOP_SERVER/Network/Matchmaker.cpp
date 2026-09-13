#include "Matchmaker.h"
#include "Session.h"

namespace Wop
{
    Matchmaker::Matchmaker(uint32_t minSquadSize, uint32_t maxSquadSize,
                           std::chrono::milliseconds soloTimeout,
                           std::function<void(std::vector<std::shared_ptr<Session>>)> formRoom)
        : minSquadSize_(minSquadSize == 0 ? 1 : minSquadSize)
        , maxSquadSize_(maxSquadSize < minSquadSize_ ? minSquadSize_ : maxSquadSize)
        , soloTimeout_(soloTimeout)
        , formRoom_(std::move(formRoom))
    {
    }

    void Matchmaker::Enqueue(std::shared_ptr<Session> session)
    {
        if (!session)
            return;

        std::lock_guard<std::mutex> guard(lock_);
        queue_.push_back({ std::move(session), std::chrono::steady_clock::now() });

        if (queue_.size() >= minSquadSize_)
            FormSquadLocked();
    }

    void Matchmaker::Cancel(uint32_t sessionId)
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
        {
            if (it->session->GetId() == sessionId)
            {
                queue_.erase(it);
                return;
            }
        }
    }

    void Matchmaker::TickTimeouts()
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (queue_.empty())
            return;

        // Only the OLDEST queued session's age matters -- Enqueue already
        // forms a squad the instant the queue reaches minSquadSize_, so
        // this only ever has to deal with a queue that's been stuck below
        // that size (down to just 1) for a while. Form whatever's queued
        // now rather than leaving it stuck forever.
        const auto now = std::chrono::steady_clock::now();
        if (now - queue_.front().queuedAt >= soloTimeout_)
            FormSquadLocked();
    }

    void Matchmaker::FormSquadLocked()
    {
        const size_t count = queue_.size() < maxSquadSize_ ? queue_.size() : static_cast<size_t>(maxSquadSize_);
        if (count == 0)
            return;

        std::vector<std::shared_ptr<Session>> squad;
        squad.reserve(count);
        for (size_t i = 0; i < count; ++i)
            squad.push_back(std::move(queue_[i].session));
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(count));

        formRoom_(std::move(squad));
    }
}
