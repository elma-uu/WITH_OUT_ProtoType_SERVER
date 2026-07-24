#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

namespace Wop
{
    // Backing store for one RIO-registered buffer.
    //
    // RIO needs a single contiguous memory region to hand to RIOReceive /
    // RIOSend, so this is not a true wraparound ring: data is appended
    // linearly and, once the tail runs out of room, the unread bytes are
    // compacted (memmove'd) back to offset 0. The underlying storage is
    // allocated once and never resized, so the pointer registered with
    // RIORegisterBuffer stays valid for the lifetime of the object.
    class RingBuffer
    {
    public:
        explicit RingBuffer(uint32_t capacity)
            : buffer_(capacity)
            , capacity_(capacity)
        {
        }

        uint32_t Capacity() const { return capacity_; }
        uint32_t DataSize() const { return writePos_ - readPos_; }

        char* Base() { return buffer_.data(); }
        char* ReadPos() { return buffer_.data() + readPos_; }
        char* WritePos() { return buffer_.data() + writePos_; }

        uint32_t ReadOffset() const { return readPos_; }
        uint32_t WriteOffset() const { return writePos_; }

        // Contiguous free space available at the tail, without compacting.
        uint32_t TailFreeSize() const { return capacity_ - writePos_; }

        // Makes sure at least `needed` contiguous bytes are free at the tail,
        // compacting first if the total free space allows it.
        // Returns false if `needed` can never fit (bigger than capacity).
        bool ReserveWritable(uint32_t needed)
        {
            if (needed > capacity_)
                return false;

            if (TailFreeSize() < needed)
                Compact();

            return TailFreeSize() >= needed;
        }

        // Call after bytes have actually been written into WritePos().
        void OnWrite(uint32_t len)
        {
            writePos_ += len;
        }

        // Call after bytes have been consumed from ReadPos().
        void OnRead(uint32_t len)
        {
            readPos_ += len;
            if (readPos_ == writePos_)
            {
                // Fully drained: reset to the front instead of waiting for a
                // compaction to be forced later.
                readPos_ = 0;
                writePos_ = 0;
            }
        }

    private:
        void Compact()
        {
            if (readPos_ == 0)
                return;

            const uint32_t used = DataSize();
            if (used > 0)
                std::memmove(buffer_.data(), buffer_.data() + readPos_, used);

            readPos_ = 0;
            writePos_ = used;
        }

        std::vector<char> buffer_;
        uint32_t capacity_;
        uint32_t readPos_ = 0;
        uint32_t writePos_ = 0;
    };
}
