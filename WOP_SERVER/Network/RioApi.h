#pragma once
#include "NetCommon.h"

namespace Wop
{
    // Loads the Registered I/O extension function pointers
    // (RIOReceive, RIOSend, RIORegisterBuffer, ...) once per process.
    // These are not statically linkable; they must be fetched at runtime
    // via WSAIoctl(SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER).
    class RioApi
    {
    public:
        static RioApi& Get()
        {
            static RioApi instance;
            return instance;
        }

        // `probeSocket` only needs to exist long enough for the WSAIoctl
        // call; it does not have to be the socket RIO will later operate on.
        bool Load(SOCKET probeSocket)
        {
            if (loaded_)
                return true;

            GUID functionTableId = WSAID_MULTIPLE_RIO;
            DWORD bytes = 0;
            const int result = WSAIoctl(
                probeSocket,
                SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
                &functionTableId, sizeof(functionTableId),
                &table_, sizeof(table_),
                &bytes, nullptr, nullptr);

            loaded_ = (result != SOCKET_ERROR);
            return loaded_;
        }

        const RIO_EXTENSION_FUNCTION_TABLE& Table() const { return table_; }

    private:
        RioApi() = default;

        RIO_EXTENSION_FUNCTION_TABLE table_{};
        bool loaded_ = false;
    };
}
