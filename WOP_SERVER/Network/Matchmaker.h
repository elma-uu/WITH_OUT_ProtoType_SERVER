#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace Wop
{
    class Session;

    // Groups newly-logged-in sessions into small squads before handing them
    // a Room -- step 2 of the Login/Game server split (매칭 서버 설계).
    // Session itself has no idea this exists: EchoServer::EnqueueForMatch is
    // the only entry point, called once from Session's own C2S_Login case
    // (see Room::AnnounceNewMember for what a session sees once it's
    // actually matched).
    //
    // A squad forms the INSTANT enough sessions are queued (checked
    // synchronously inside Enqueue -- no polling delay for the common
    // case of a squad-mate already waiting), or, if nobody else shows up,
    // once a lone session has waited soloTimeout with the queue still
    // under minSquadSize -- see TickTimeouts. Either way formRoom is
    // invoked with whatever sessions were grouped; what it does with them
    // (create a Room, add them to it) is EchoServer's business, not this
    // class's.
    class Matchmaker
    {
    public:
        Matchmaker(uint32_t minSquadSize, uint32_t maxSquadSize,
                   std::chrono::milliseconds soloTimeout,
                   std::function<void(std::vector<std::shared_ptr<Session>>)> formRoom);

        // Adds `session` to the queue. Forms a squad immediately (invoking
        // formRoom_ with up to maxSquadSize_ sessions, oldest-queued
        // first) if the queue now holds at least minSquadSize_ -- so the
        // common case (a squad-mate is already waiting) never waits on
        // TickTimeouts's polling cadence at all.
        void Enqueue(std::shared_ptr<Session> session);

        // Removes a queued session that disconnected before ever being
        // matched, so it doesn't linger in the queue (or get handed to
        // formRoom_ as a stale entry once it times out) forever. No-op if
        // it already matched (and so isn't queued anymore) or was never
        // queued to begin with.
        void Cancel(uint32_t sessionId);

        // Called periodically (see EchoServer::BackgroundTickLoop): forms
        // an under-sized squad (as small as just 1) for whoever has been
        // waiting alone longer than soloTimeout_, so a player is never
        // stuck waiting for a squad-mate who never arrives. A no-op
        // whenever the queue is empty or still fresh.
        void TickTimeouts();

    private:
        struct QueuedSession
        {
            std::shared_ptr<Session> session;
            std::chrono::steady_clock::time_point queuedAt;
        };

        // Caller must already hold lock_. Pops up to maxSquadSize_ sessions
        // off the FRONT of queue_ (oldest-queued first) and invokes
        // formRoom_ with them; a no-op if the queue is empty.
        void FormSquadLocked();

        std::mutex lock_;
        std::vector<QueuedSession> queue_;

        uint32_t minSquadSize_;
        uint32_t maxSquadSize_;
        std::chrono::milliseconds soloTimeout_;
        std::function<void(std::vector<std::shared_ptr<Session>>)> formRoom_;
    };
}
