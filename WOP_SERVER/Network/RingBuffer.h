#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

namespace Wop
{
    // Backing store for one RIO-registered buffer. RIO needs a single
    // contiguous region, so this isn't a true wraparound ring: data is
    // appended linearly and compacted (memmove) back to offset 0 once the
    // tail runs out of room. Storage is never resized, so the pointer
    // registered with RIORegisterBuffer stays valid for the object's life.
    class RingBuffer
    {
    public:
        explicit RingBuffer(uint32_t capacity)
            : buffer_(capacity)
            , capacity_(capacity)
        {
        }

        /*-------------------
         조회
        -------------------*/
        uint32_t Capacity() const { return capacity_; }
        uint32_t DataSize() const { return writePos_ - readPos_; }

        char* Base() { return buffer_.data(); }
        char* ReadPos() { return buffer_.data() + readPos_; }
        char* WritePos() { return buffer_.data() + writePos_; }

        uint32_t ReadOffset() const { return readPos_; }
        uint32_t WriteOffset() const { return writePos_; }

        uint32_t TailFreeSize() const { return capacity_ - writePos_; }

        /*-------------------
         쓰기/읽기
        -------------------*/
        bool ReserveWritable(uint32_t needed)
        {
            if (needed > capacity_)
                return false;

            if (TailFreeSize() < needed)
                Compact();

            return TailFreeSize() >= needed;
        }

        void OnWrite(uint32_t len)
        {
            writePos_ += len;
        }

        void OnRead(uint32_t len)
        {
            readPos_ += len;
            if (readPos_ == writePos_)
            {
                readPos_ = 0;
                writePos_ = 0;
            }
        }

    private:
        /*-------------------
         내부: 압축(memmove)
        -------------------*/
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
