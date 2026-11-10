#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <sys/socket.h>

using namespace std;

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files.

// ---- Shared Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t IO_BUFFER_SIZE = 64 * 1024;

// ---- Shared Structs ----
struct Variable
{
    string name;
    int32_t value;
};

struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

struct ClientInfo
{
    int sock;
    string address;
    int port;
    time_t arrivedAt;
};

// ---- Shared CRC-32 & Socket Functions ----
inline uint32_t computeRunningCRC(uint32_t prior_crc, const char *byte_data, size_t data_len)
{
    uint32_t crc_accumulator = ~prior_crc;

    for (size_t byte_idx = 0; byte_idx < data_len; byte_idx++)
    {
        uint8_t current_byte_val = (uint8_t)byte_data[byte_idx];

        crc_accumulator ^= current_byte_val;

        for (int bit_idx = 0; bit_idx < 8; bit_idx++)
        {
            if (crc_accumulator & 1)
            {
                crc_accumulator = (crc_accumulator >> 1) ^ 0xEDB88320U;
            }
            else
            {
                crc_accumulator = (crc_accumulator >> 1);
            }
        }
    }

    return ~crc_accumulator;
}

struct TrailerChecker
{

    char trailer_window[4];
    int window_fill_count;
    uint32_t calculated_crc;

    TrailerChecker()
    {
        window_fill_count = 0;
        calculated_crc = 0;
    }

    void feedByte(char current_byte, bool &has_output_byte, char &output_byte)
    {
        has_output_byte = false;

        if (window_fill_count < 4)
        {
            trailer_window[window_fill_count] = current_byte;

            window_fill_count++;
        }
        else
        {
            output_byte = trailer_window[0];

            has_output_byte = true;

            calculated_crc = computeRunningCRC(calculated_crc, &output_byte, 1);

            trailer_window[0] = trailer_window[1];
            trailer_window[1] = trailer_window[2];
            trailer_window[2] = trailer_window[3];
            trailer_window[3] = current_byte;
        }
    }

    bool verify()
    {
        if (window_fill_count != 4)
        {
            return false;
        }

        uint32_t received_crc = 0;

        memcpy(&received_crc, trailer_window, 4);

        return received_crc == calculated_crc;
    }
};




inline bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.

    size_t total_received_bytes = 0;

    while (total_received_bytes < n)
    {
        ssize_t received_chunk = recv(sock, buf + total_received_bytes, n - total_received_bytes, 0);

        if (received_chunk <= 0)
        {
            return false;
        }

        total_received_bytes += (size_t)received_chunk;
    }

    return true;
}






inline bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.

    size_t total_sent_bytes = 0;

    while (total_sent_bytes < n)
    {
        ssize_t sent_chunk = send(sock, buf + total_sent_bytes, n - total_sent_bytes, MSG_NOSIGNAL);

        if (sent_chunk <= 0)
        {
            return false;
        }

        total_sent_bytes += (size_t)sent_chunk;
    }

    return true;
}

