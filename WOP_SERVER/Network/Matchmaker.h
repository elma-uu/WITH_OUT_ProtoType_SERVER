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
    // a Room -- part of the Login/Game server split (매칭 서버 설계).
    // Session itself has no idea this exists: EchoServer::EnqueueForMatch is
    // the only entry point, called once from Session's own C2S_Login/
    // C2S_JoinMatch handling (see Room::AnnounceNewMember for what a
    // session sees once it's actually matched).
    //
    // 재설계(사용자 요청): 예전엔 대기열이 2명(minSquadSize)에 닿는 즉시,
    // 또는 혼자 200ms 넘게 기다리면 그 즉시 방을 형성했다 -- 그래서 "몇 명이
    // 모였는지" 클라이언트가 지켜볼 시간 자체가 사실상 없었다. 지금은 항상
    // 최대 대기 시간(matchWindow, 기본 10초)만큼 기다렸다가 그때까지 모인
    // 인원으로(1명이어도) 방을 형성한다 -- 유일한 예외는 대기열이
    // maxSquadSize(4)로 꽉 찼을 때뿐이고, 그때는 굳이 남은 시간을 기다리지
    // 않고 즉시 형성한다. 대기열에 세션이 들어오거나 빠질 때마다 그 순간의
    // 대기열 전원에게 S2C_MatchmakingStatus{current, max}를 다시 보내고,
    // 실제로 방이 형성되는 순간엔 그 스쿼드 전원에게 S2C_MatchmakingComplete를
    // 딱 한 번 보낸다 -- 클라이언트(LevelChangeSelectWidget)는 이 Complete
    // 신호 하나만 기다리면 되고, 그게 꽉 차서 즉시 형성됐는지 시간 초과로
    // 형성됐는지 구분할 필요가 없다.
    class Matchmaker
    {
    public:
        Matchmaker(uint32_t maxSquadSize, std::chrono::milliseconds matchWindow,
                   std::function<void(std::vector<std::shared_ptr<Session>>)> formRoom);

        // Adds `session` to the queue and immediately tells every currently
        // queued session (including this new one) the updated current/max
        // count via S2C_MatchmakingStatus. Starts (or keeps running) the
        // shared matchWindow_ countdown from the moment the FIRST session
        // lands in an otherwise-empty queue -- late arrivals don't restart
        // it, they just join whatever's left of it. Forms the squad
        // immediately (see FormSquadLocked) if the queue now holds
        // maxSquadSize_ sessions, without waiting for TickTimeouts.
        void Enqueue(std::shared_ptr<Session> session);

        // Removes a queued session that disconnected before ever being
        // matched, so it doesn't linger in the queue (or get handed to
        // formRoom_ as a stale entry once it times out) forever. Re-
        // broadcasts the updated count to whoever's left, so their overlay
        // doesn't keep showing a headcount that just dropped. No-op if it
        // already matched (and so isn't queued anymore) or was never
        // queued to begin with.
        void Cancel(uint32_t sessionId);

        // Called periodically (see EchoServer::BackgroundTickLoop): forms
        // whatever's queued (as small as just 1) once matchWindow_ has
        // elapsed since the queue's oldest still-pending wait started, so a
        // player is never stuck waiting past that cap for squad-mates who
        // never arrive. A no-op whenever the queue is empty or the window
        // hasn't elapsed yet.
        void TickTimeouts();

    private:
        struct QueuedSession
        {
            std::shared_ptr<Session> session;
        };

        // Caller must already hold lock_. Pops every currently queued
        // session (never more than maxSquadSize_, since Enqueue forms
        // immediately upon reaching it) and invokes formRoom_ with them --
        // a no-op if the queue is empty. Sends each of them
        // S2C_MatchmakingComplete first (see this class's header comment).
        void FormSquadLocked();

        // Caller must already hold lock_. Sends every currently queued
        // session S2C_MatchmakingStatus{current: queue_.size(), max:
        // maxSquadSize_} -- called after any change to queue_'s membership
        // short of it being fully formed (see FormSquadLocked, which sends
        // the terminal Complete message instead).
        void BroadcastStatusLocked();

        std::mutex lock_;
        std::vector<QueuedSession> queue_;

        // Set to now() whenever queue_ transitions from empty to non-empty
        // (see Enqueue); only meaningful while queue_ is non-empty.
        std::chrono::steady_clock::time_point windowStartedAt_{};

        uint32_t maxSquadSize_;
        std::chrono::milliseconds matchWindow_;
        std::function<void(std::vector<std::shared_ptr<Session>>)> formRoom_;
    };
}
