#include "Matchmaker.h"
#include "Session.h"
#include "packet.h"

namespace Wop
{
    Matchmaker::Matchmaker(uint32_t maxSquadSize, std::chrono::milliseconds matchWindow,
                           std::function<void(std::vector<std::shared_ptr<Session>>)> formRoom)
        : maxSquadSize_(maxSquadSize == 0 ? 1 : maxSquadSize)
        , matchWindow_(matchWindow)
        , formRoom_(std::move(formRoom))
    {
    }

    void Matchmaker::Enqueue(std::shared_ptr<Session> session)
    {
        if (!session)
            return;

        std::lock_guard<std::mutex> guard(lock_);

        if (queue_.empty())
            windowStartedAt_ = std::chrono::steady_clock::now();

        queue_.push_back({ std::move(session) });

        if (queue_.size() >= maxSquadSize_)
        {
            // Room to spare over -- see this class's header comment: full
            // is the one case that doesn't wait out the rest of matchWindow_.
            FormSquadLocked();
            return;
        }

        BroadcastStatusLocked();
    }

    void Matchmaker::Cancel(uint32_t sessionId)
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
        {
            if (it->session->GetId() == sessionId)
            {
                queue_.erase(it);
                // Whoever's left should see the headcount drop too, not
                // keep showing a stale (now-too-high) number.
                if (!queue_.empty())
                    BroadcastStatusLocked();
                return;
            }
        }
    }

    void Matchmaker::TickTimeouts()
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (queue_.empty())
            return;

        const auto now = std::chrono::steady_clock::now();
        if (now - windowStartedAt_ >= matchWindow_)
            FormSquadLocked();
    }

    void Matchmaker::FormSquadLocked()
    {
        if (queue_.empty())
            return;

        std::vector<std::shared_ptr<Session>> squad;
        squad.reserve(queue_.size());
        for (auto& queued : queue_)
            squad.push_back(std::move(queued.session));
        queue_.clear();

        // Tell every matched member the wait is over BEFORE formRoom_ (which
        // actually creates the Room and calls Room::AddSession -- see
        // EchoServer::CreateRoomForSquad) starts sending them roster/door
        // replays, so the client's "matching complete" signal always arrives
        // first.
        {
            using namespace ProtoType::Net;
            flatbuffers::FlatBufferBuilder fbb;
            auto complete = CreateS2C_MatchmakingComplete(fbb, static_cast<uint16_t>(squad.size()));
            auto packet = CreatePacket(fbb, Payload::S2C_MatchmakingComplete, complete.Union());
            FinishSizePrefixedPacketBuffer(fbb, packet);
            for (const auto& member : squad)
            {
                member->Send(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                             static_cast<uint32_t>(fbb.GetSize()));
            }
        }

        formRoom_(std::move(squad));
    }

    void Matchmaker::BroadcastStatusLocked()
    {
        using namespace ProtoType::Net;
        flatbuffers::FlatBufferBuilder fbb;
        auto status = CreateS2C_MatchmakingStatus(
            fbb, static_cast<uint16_t>(queue_.size()), static_cast<uint16_t>(maxSquadSize_));
        auto packet = CreatePacket(fbb, Payload::S2C_MatchmakingStatus, status.Union());
        FinishSizePrefixedPacketBuffer(fbb, packet);
        for (const auto& queued : queue_)
        {
            queued.session->Send(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                                  static_cast<uint32_t>(fbb.GetSize()));
        }
    }
}
